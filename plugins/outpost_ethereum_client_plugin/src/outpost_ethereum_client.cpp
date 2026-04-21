#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>

#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>

namespace sysio {

namespace {

namespace eth = fc::network::ethereum;

// ── Wire-side identifiers ────────────────────────────────────────────────
/// Event emitted by `OPP.sol` carrying a serialized `opp::Envelope` as its
/// sole `bytes` argument. Mirrored from `wire-ethereum/contracts/OPP.sol`.
constexpr std::string_view OPP_ENVELOPE_EVENT_NAME = "OPPEnvelope";

/// Key under which the ABI decoder parks the raw envelope payload inside
/// the decoded event variant when the event has a single named `bytes`
/// parameter.
constexpr std::string_view EVENT_DATA_FIELD = "data";

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

   // Build the typed contract clients once at construction — same lazy-cached
   // behavior the old `ensure_eth_clients` guarded, just eagerly now that this
   // object exists.
   _opp_client         = _entry->client->get_contract<opp_contract_client>(_opp_addr, abis);
   _opp_inbound_client = _entry->client->get_contract<opp_inbound_contract_client>(_opp_inbound_addr, abis);
}

sysio::opp::types::ChainKind outpost_ethereum_client::chain_kind() const {
   return sysio::opp::types::CHAIN_KIND_ETHEREUM;
}

std::string outpost_ethereum_client::deliver_outbound_envelope(
   uint32_t                 epoch_index,
   const std::vector<char>& envelope_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;

   throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);

   // epoch_in takes its std::string argument by non-const reference, so the
   // hex payload has to be mutable here.
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

   const auto events = _opp_client->query_events(
      {std::string(OPP_ENVELOPE_EVENT_NAME)},
      eth::block_tag{eth::block_tag_latest},
      eth::block_tag{eth::block_tag_latest});

   ilog("outpost_ethereum_client[{}]: {} events fetched = {}",
        to_string(), OPP_ENVELOPE_EVENT_NAME, events.size());

   std::vector<char> combined;
   uint32_t          msg_count = 0;

   for (auto& evt : events) {
      if (evt.event_name != OPP_ENVELOPE_EVENT_NAME || evt.data.empty()) continue;

      // `evt.data` is raw ABI-encoded event data; decode through the ABI to
      // extract the single `bytes` parameter that holds the raw protobuf.
      auto decoded = evt.decode<fc::variant>();
      if (!decoded.has_value()) {
         elog("outpost_ethereum_client[{}]: failed to ABI-decode {} event: {}",
              to_string(), OPP_ENVELOPE_EVENT_NAME, decoded.error().what());
         continue;
      }

      auto&        v = decoded.value();
      std::string  hex_data;
      if (v.is_object() && v.get_object().contains(EVENT_DATA_FIELD.data())) {
         hex_data = v[EVENT_DATA_FIELD.data()].as_string();
      } else if (v.is_string()) {
         hex_data = v.as_string();
      } else {
         elog("outpost_ethereum_client[{}]: unexpected ABI-decoded variant type for {}",
              to_string(), OPP_ENVELOPE_EVENT_NAME);
         continue;
      }

      auto proto_bytes = fc::crypto::ethereum::hex_to_bytes(hex_data);

      // Validate the payload is a well-formed opp::Envelope and that its
      // epoch_index matches. ETH retains prior-epoch emissions in its event
      // log; a block-range query for `latest` can surface stale envelopes
      // that would otherwise trip `sysio.msgch::deliver`'s epoch_index
      // mismatch assertion.
      sysio::opp::Envelope envelope;
      if (!envelope.ParseFromArray(proto_bytes.data(),
                                   static_cast<int>(proto_bytes.size()))) {
         wlog("outpost_ethereum_client[{}]: skipping non-Envelope ETH event payload ({} bytes)",
              to_string(), proto_bytes.size());
         continue;
      }

      if (static_cast<uint32_t>(envelope.epoch_index()) != epoch_index) {
         dlog("outpost_ethereum_client[{}]: skipping envelope for epoch {} (current {})",
              to_string(), envelope.epoch_index(), epoch_index);
         continue;
      }

      combined.insert(combined.end(),
                      reinterpret_cast<const char*>(proto_bytes.data()),
                      reinterpret_cast<const char*>(proto_bytes.data() + proto_bytes.size()));
      ++msg_count;
   }

   if (msg_count > 0) {
      ilog("outpost_ethereum_client[{}]: concatenated {} inbound envelopes ({} bytes)",
           to_string(), msg_count, combined.size());
   }
   return combined;
}

} // namespace sysio
