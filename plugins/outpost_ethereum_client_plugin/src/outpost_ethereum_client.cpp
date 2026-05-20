#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>

#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>

namespace sysio {

namespace {

namespace eth = fc::network::ethereum;

// ── Op labels used for deadline-exceeded error messages ──────────────────
constexpr std::string_view OP_DELIVER_OUTBOUND = "deliver_outbound_envelope";
constexpr std::string_view OP_READ_INBOUND     = "read_inbound_envelope";

} // namespace

outpost_ethereum_client::outpost_ethereum_client(
   ethereum_client_entry_ptr                         entry,
   std::string                                       opp_addr,
   std::string                                       opp_inbound_addr,
   std::vector<fc::network::ethereum::abi::contract> abis,
   uint64_t                                          outpost_id,
   uint32_t                                          chain_id)
   : _entry(std::move(entry))
   , _opp_addr(std::move(opp_addr))
   , _opp_inbound_addr(std::move(opp_inbound_addr))
   , _outpost_id(outpost_id)
   , _chain_id(chain_id) {
   FC_ASSERT(_entry && _entry->client, "ethereum_client_entry must carry a client");
   FC_ASSERT(!_opp_addr.empty(),         "OPP address is required");
   FC_ASSERT(!_opp_inbound_addr.empty(), "OPPInbound address is required");

   _opp_client         = _entry->client->get_contract<opp_contract_client>(_opp_addr, abis);
   _opp_inbound_client = _entry->client->get_contract<opp_inbound_contract_client>(_opp_inbound_addr, abis);
}

sysio::opp::types::ChainKind outpost_ethereum_client::chain_kind() const {
   return sysio::opp::types::CHAIN_KIND_EVM;
}

std::string outpost_ethereum_client::deliver_outbound_envelope(
   uint32_t                 epoch_index,
   const std::vector<char>& envelope_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;

   throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);

   std::string envelope_hex = fc::to_hex(envelope_bytes);
   auto        result       = _opp_inbound_client->epoch_in(envelope_hex);

   ilog("outpost_ethereum_client[{}]: epochIn epoch={} bytes={} result={}",
        to_string(), epoch_index, envelope_bytes.size(), result.as_string());
   return result.as_string();
}

std::vector<char> outpost_ethereum_client::read_inbound_envelope(
   uint32_t         epoch_index,
   fc::microseconds deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   throw_if_past_deadline(deadline_abs, OP_READ_INBOUND);

   // Single view call against the OPP contract's `latestOutboundEnvelope`
   // storage slot, populated by `emitOutboundEnvelope`. The OPP cycle is
   // atomic across actors so only the most-recent emitted envelope is in
   // flight at any moment — historical reads are out of scope and live
   // in the `OPPEnvelope` event archive for off-chain auditors.
   // The typed view's `fc::variant` return is the raw hex `eth_call`
   // result — `create_call<fc::variant>` does NOT auto-decode. Pull the
   // ABI entry for this view and decode through `contract_decode_data`
   // so we get the structured outputs `(uint32 epoch_, bytes data_)`
   // back as a `mutable_variant_object`.
   const auto& abi = _opp_client->get_abi("getLatestOutboundEnvelope");
   const auto raw_hex_var = _opp_client->get_latest_outbound_envelope(
      std::string(eth::block_tag_latest));
   if (!raw_hex_var.is_string()) {
      wlog("outpost_ethereum_client[{}]: getLatestOutboundEnvelope returned non-string variant",
           to_string());
      return {};
   }
   const std::string raw_hex = raw_hex_var.as_string();
   dlog("outpost_ethereum_client[{}]: getLatestOutboundEnvelope raw_hex={}",
        to_string(), raw_hex);
   if (raw_hex.empty() || raw_hex == "0x") {
      // Empty result → contract returned nothing. Either eth_call hit a
      // non-existent slot (unexpected on a deployed contract) or the
      // chain rolled back. Surface as a warning either way.
      wlog("outpost_ethereum_client[{}]: getLatestOutboundEnvelope returned empty hex",
           to_string());
      return {};
   }

   const auto decoded = eth::contract_decode_data(abi, raw_hex);
   dlog("outpost_ethereum_client[{}]: getLatestOutboundEnvelope decoded={}",
        to_string(), fc::json::to_string(decoded, fc::json::yield_function_t{}));
   if (!decoded.is_object()) {
      wlog("outpost_ethereum_client[{}]: decoded view result was not a variant object",
           to_string());
      return {};
   }
   const auto& obj = decoded.get_object();
   if (!obj.contains("epoch_") || !obj.contains("data_")) {
      wlog("outpost_ethereum_client[{}]: decoded view result missing epoch_/data_ keys",
           to_string());
      return {};
   }

   // The libfc ABI decoder returns integer outputs as decimal strings
   // (the encoder normalises every numeric type to a string). Parse
   // accordingly; fall back to uint64 form if a future decoder change
   // emits raw numbers.
   uint32_t stored_epoch = 0;
   {
      const auto& ev = obj["epoch_"];
      if (ev.is_string()) {
         try { stored_epoch = static_cast<uint32_t>(std::stoul(ev.as_string())); }
         catch (...) {
            wlog("outpost_ethereum_client[{}]: failed to parse epoch_ string '{}'",
                 to_string(), ev.as_string());
            return {};
         }
      } else if (ev.is_uint64() || ev.is_int64()) {
         stored_epoch = static_cast<uint32_t>(ev.as_uint64());
      } else {
         wlog("outpost_ethereum_client[{}]: epoch_ has unexpected variant type",
              to_string());
         return {};
      }
   }
   if (stored_epoch == 0 || stored_epoch != epoch_index) {
      // Timing-only: outpost hasn't emitted yet (epoch=0) or the WIRE
      // batch op is querying a slightly stale tip. Both resolve on the
      // next poll. Keep at dlog so steady-state operation isn't noisy.
      dlog("outpost_ethereum_client[{}]: latestOutboundEnvelope epoch mismatch stored={} requested={}",
           to_string(), stored_epoch, epoch_index);
      return {};
   }

   const auto& data_var = obj["data_"];
   if (!data_var.is_string()) {
      wlog("outpost_ethereum_client[{}]: latestOutboundEnvelope data_ not a string",
           to_string());
      return {};
   }
   const std::string hex_data = data_var.as_string();
   const auto raw = fc::crypto::ethereum::hex_to_bytes(hex_data);
   if (raw.empty()) return {};

   sysio::opp::Envelope envelope;
   if (!envelope.ParseFromArray(raw.data(), static_cast<int>(raw.size()))) {
      wlog("outpost_ethereum_client[{}]: latestOutboundEnvelope did not "
           "decode as a protobuf Envelope ({} bytes)",
           to_string(), raw.size());
      return {};
   }
   if (static_cast<uint32_t>(envelope.epoch_index()) != epoch_index) {
      wlog("outpost_ethereum_client[{}]: latestOutboundEnvelope inner "
           "epoch={} != requested {}",
           to_string(), envelope.epoch_index(), epoch_index);
      return {};
   }

   std::vector<char> out(reinterpret_cast<const char*>(raw.data()),
                         reinterpret_cast<const char*>(raw.data() + raw.size()));
   ilog("outpost_ethereum_client[{}]: read inbound envelope epoch={} bytes={}",
        to_string(), epoch_index, out.size());
   return out;
}

} // namespace sysio
