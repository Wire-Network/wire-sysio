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
constexpr std::string_view OP_UW_COMMIT        = "uw_commit";

} // namespace

outpost_ethereum_client::outpost_ethereum_client(
   ethereum_client_entry_ptr                         entry,
   std::string                                       opp_addr,
   std::string                                       opp_inbound_addr,
   std::string                                       operator_registry_addr,
   std::vector<fc::network::ethereum::abi::contract> abis,
   uint64_t                                          chain_code,
   uint32_t                                          chain_id)
   : _entry(std::move(entry))
   , _opp_addr(std::move(opp_addr))
   , _opp_inbound_addr(std::move(opp_inbound_addr))
   , _operator_registry_addr(std::move(operator_registry_addr))
   , _outpost_id(chain_code)
   , _chain_id(chain_id) {
   FC_ASSERT(_entry && _entry->client, "ethereum_client_entry must carry a client");

   // Each contract wrapper is materialized only if its address was
   // supplied. A caller that only consumes one outpost capability (e.g.
   // the underwriter calling `uw_commit` against OperatorRegistry) can
   // pass empty strings for the addresses it doesn't use; the methods
   // covering an unprovisioned wrapper assert on entry with a clear
   // diagnostic. Per `outpost-client-spi.md`: address configuration is
   // a per-caller concern; the SPI shape stays uniform.
   if (!_opp_addr.empty()) {
      _opp_client = _entry->client->get_contract<opp_contract_client>(_opp_addr, abis);
   }
   if (!_opp_inbound_addr.empty()) {
      _opp_inbound_client =
         _entry->client->get_contract<opp_inbound_contract_client>(_opp_inbound_addr, abis);
   }
   if (!_operator_registry_addr.empty()) {
      _operator_registry_client =
         _entry->client->get_contract<operator_registry_contract_client>(_operator_registry_addr, abis);
   }
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
   FC_ASSERT(_opp_inbound_client,
             "outpost_ethereum_client[{}]: deliver_outbound_envelope requires an "
             "OPPInbound address — pass opp_inbound_addr to create_outpost_client",
             to_string());

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
   FC_ASSERT(_opp_client,
             "outpost_ethereum_client[{}]: read_inbound_envelope requires an OPP "
             "address — pass opp_addr to create_outpost_client",
             to_string());

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
   // Read at `finalized`, not `latest`. WIRE consensus on inbound is committed forward against this
   // read: an operator that reads a slot at `latest` can achieve WIRE-side consensus on it and queue
   // attestations off it, then watch that slot reorg out of Ethereum's canonical chain seconds later,
   // leaving WIRE committed to history that no longer exists. `finalized` is the only tag with
   // cryptoeconomic finality. This is deliberately not operator-configurable: the read commitment is a
   // consensus parameter, and operators reading at different commitments would deliver divergent
   // envelopes for the same epoch, manufacturing disputes among honest operators.
   const auto raw_hex_var = _opp_client->get_latest_outbound_envelope(
      std::string(eth::block_tag_finalized));
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

std::string outpost_ethereum_client::uw_commit(
   uint64_t                 uw_request_id,
   const std::vector<char>& uic_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   throw_if_past_deadline(deadline_abs, OP_UW_COMMIT);

   FC_ASSERT(_operator_registry_client,
             "outpost_ethereum_client[{}]: uw_commit requires an OperatorRegistry "
             "address — pass operator_registry_addr to create_outpost_client",
             to_string());

   // Solidity `commit(bytes uicBytes)` takes a `bytes` parameter; the
   // libfc ABI encoder for `dt::bytes` expects a hex-encoded string
   // (see ethereum_abi.cpp::encode_dynamic_data). Building the variant
   // around the raw `std::vector<uint8_t>` triggers an `fc::bad_cast`
   // inside the encoder — the typed wrapper takes the hex form directly.
   //
   // The `ethereum_contract_tx_fn<fc::variant, std::string>` signature
   // binds the argument as a non-const `std::string&`, so the local
   // must be a non-const lvalue (mirroring the `epoch_in(envelope_hex)`
   // pattern in `deliver_outbound_envelope`).
   std::string uic_hex = std::string("0x") +
      fc::to_hex(uic_bytes.data(), uic_bytes.size());

   const auto result  = _operator_registry_client->commit(uic_hex);
   const auto tx_hash = result.as_string();
   ilog("outpost_ethereum_client[{}]: uw_commit confirmed uwreq={} tx_hash={} bytes={}",
        to_string(), uw_request_id, tx_hash, uic_bytes.size());
   return tx_hash;
}

} // namespace sysio
