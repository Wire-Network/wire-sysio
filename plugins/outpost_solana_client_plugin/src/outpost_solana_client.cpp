#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

#include <optional>
#include <string>
#include <string_view>

#include <fc/crypto/base64.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/variant_object.hpp>

#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>

namespace sysio {

namespace {

// ── Solana JSON-RPC method names ─────────────────────────────────────────
constexpr std::string_view RPC_GET_SIGNATURES_FOR_ADDRESS = "getSignaturesForAddress";
constexpr std::string_view RPC_GET_TRANSACTION            = "getTransaction";

// ── Response field names (raw JSON fields, not ABI) ──────────────────────
constexpr std::string_view FIELD_SIGNATURE    = "signature";
constexpr std::string_view FIELD_META         = "meta";
constexpr std::string_view FIELD_ERR          = "err";
constexpr std::string_view FIELD_LOG_MESSAGES = "logMessages";

// ── getSignaturesForAddress params ───────────────────────────────────────
constexpr std::string_view PARAM_LIMIT           = "limit";
/// How many recent program signatures to scan per inbound cycle. 20 covers
/// several epochs' worth of envelopes at the current 60s cadence while
/// keeping the per-tick RPC cost bounded.
constexpr uint32_t         SIGNATURE_FETCH_LIMIT = 20;

// ── getTransaction params ────────────────────────────────────────────────
constexpr std::string_view PARAM_ENCODING         = "encoding";
constexpr std::string_view ENCODING_JSON          = "json";
constexpr std::string_view PARAM_MAX_SUPPORTED_TX = "maxSupportedTransactionVersion";
/// Versioned-tx support level — 0 accepts both legacy and v0 txs.
constexpr uint32_t         MAX_SUPPORTED_TX_VERSION = 0;

// ── Program-data log parsing ─────────────────────────────────────────────
/// Solana prefixes base64-encoded `sol_log_data` payloads with this exact
/// string in the tx's `meta.logMessages` array.
constexpr std::string_view PROGRAM_DATA_LOG_PREFIX = "Program data: ";

// ── Op labels used for deadline-exceeded error messages ──────────────────
constexpr std::string_view OP_EPOCH_IN               = "deliver_outbound_envelope:epoch_in";
constexpr std::string_view OP_EMIT_OUTBOUND_ENVELOPE = "deliver_outbound_envelope:emit_outbound_envelope";
constexpr std::string_view OP_RPC_GET_SIGNATURES     = "read_inbound_envelope:getSignaturesForAddress";
constexpr std::string_view OP_RPC_GET_TRANSACTION    = "read_inbound_envelope:getTransaction";

} // namespace

outpost_solana_client::outpost_solana_client(
   solana_client_entry_ptr                        entry,
   fc::network::solana::solana_public_key         program_id,
   std::vector<fc::network::solana::idl::program> program_idls,
   uint64_t                                       outpost_id,
   uint32_t                                       chain_id)
   : _entry(std::move(entry))
   , _program_id(program_id)
   , _outpost_id(outpost_id)
   , _chain_id(chain_id) {
   FC_ASSERT(_entry && _entry->client,
             "solana_client_entry must carry a client");
   FC_ASSERT(!program_idls.empty(),
             "Solana outpost requires at least one IDL for program '{}'",
             OPP_SOLANA_OUTPOST_PROGRAM_NAME);

   _program_client = std::make_shared<opp_solana_outpost_client>(
      _entry->client, _program_id, program_idls);
}

sysio::opp::types::ChainKind outpost_solana_client::chain_kind() const {
   return sysio::opp::types::CHAIN_KIND_SOLANA;
}

std::string outpost_solana_client::deliver_outbound_envelope(
   uint32_t                 epoch_index,
   const std::vector<char>& envelope_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;

   std::vector<uint8_t> bytes(envelope_bytes.begin(), envelope_bytes.end());

   throw_if_past_deadline(deadline_abs, OP_EPOCH_IN);
   auto epoch_in_sig = _program_client->epoch_in(epoch_index, bytes);
   ilog("outpost_solana_client[{}]: epoch_in sent epoch={} bytes={} sig={}",
        to_string(), epoch_index, bytes.size(), epoch_in_sig);

   // Drain queued outbound attestations. On the ETH side the equivalent is
   // triggered from inside OPPInbound on consensus; Solana has no equivalent
   // cross-program trigger so the batch operator must invoke this second
   // instruction explicitly after `epoch_in`.
   throw_if_past_deadline(deadline_abs, OP_EMIT_OUTBOUND_ENVELOPE);
   auto emit_sig = _program_client->emit_outbound_envelope(epoch_index);
   ilog("outpost_solana_client[{}]: emit_outbound_envelope sig={}", to_string(), emit_sig);

   return emit_sig;
}

std::vector<char> outpost_solana_client::read_inbound_envelope(
   uint32_t         epoch_index,
   fc::microseconds deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;

   throw_if_past_deadline(deadline_abs, OP_RPC_GET_SIGNATURES);

   const auto program_id_b58 = _program_id.to_string(fc::yield_function_t{});
   auto       sigs_result    = _entry->client->execute(
      std::string(RPC_GET_SIGNATURES_FOR_ADDRESS),
      fc::variants{
         fc::variant(program_id_b58),
         fc::variant(fc::mutable_variant_object()(PARAM_LIMIT.data(), SIGNATURE_FETCH_LIMIT))
      });

   if (!sigs_result.is_array()) {
      return {};
   }

   std::vector<char> combined;
   uint32_t          msg_count = 0;

   for (auto& sig_entry : sigs_result.get_array()) {
      if (!sig_entry.is_object()) continue;
      auto sig = sig_entry.get_object()[FIELD_SIGNATURE.data()].as_string();

      throw_if_past_deadline(deadline_abs, OP_RPC_GET_TRANSACTION);

      auto tx_result = _entry->client->execute(
         std::string(RPC_GET_TRANSACTION),
         fc::variants{
            fc::variant(sig),
            fc::variant(fc::mutable_variant_object()
                           (PARAM_ENCODING.data(),         std::string(ENCODING_JSON))
                           (PARAM_MAX_SUPPORTED_TX.data(), MAX_SUPPORTED_TX_VERSION))
         });

      if (!tx_result.is_object()) continue;
      auto& meta = tx_result.get_object()[FIELD_META.data()];
      if (!meta.is_object()) continue;

      // Skip failed transactions — Solana reports failure out-of-band in
      // `meta.err`. A non-null value means the tx reverted and any emitted
      // "Program data:" output is garbage from the partial execution.
      auto& err_field = meta.get_object()[FIELD_ERR.data()];
      if (!err_field.is_null()) continue;

      auto& log_messages = meta.get_object()[FIELD_LOG_MESSAGES.data()];
      if (!log_messages.is_array()) continue;

      // Take only the most recent `PROGRAM_DATA_LOG_PREFIX` line per tx.
      // `emit_outbound_envelope` is a single instruction → at most one
      // `sol_log_data` payload per invocation. Concatenating every match
      // would miscount envelopes on txs that bundle multiple program calls.
      std::optional<std::string> last_b64;
      for (auto& log : log_messages.get_array()) {
         auto log_str = log.as_string();
         auto pos     = log_str.find(PROGRAM_DATA_LOG_PREFIX);
         if (pos != std::string::npos) {
            last_b64 = log_str.substr(pos + PROGRAM_DATA_LOG_PREFIX.size());
         }
      }
      if (!last_b64) continue;

      auto decoded = fc::base64_decode(*last_b64);

      // Validate as a protobuf Envelope before accepting. Bad bytes would
      // otherwise propagate into `sysio.msgch::deliver` and poison the
      // epoch's inbound chain.
      sysio::opp::Envelope envelope;
      if (!envelope.ParseFromArray(decoded.data(), decoded.size())) {
         wlog("outpost_solana_client[{}]: skipping non-Envelope program data ({} bytes)",
              to_string(), decoded.size());
         continue;
      }

      if (static_cast<uint32_t>(envelope.epoch_index()) != epoch_index) {
         dlog("outpost_solana_client[{}]: skipping envelope for epoch {} (current {})",
              to_string(), envelope.epoch_index(), epoch_index);
         continue;
      }

      combined.insert(combined.end(), decoded.begin(), decoded.end());
      ++msg_count;
   }

   if (msg_count > 0) {
      ilog("outpost_solana_client[{}]: concatenated {} inbound envelopes ({} bytes)",
           to_string(), msg_count, combined.size());
   }
   return combined;
}

} // namespace sysio
