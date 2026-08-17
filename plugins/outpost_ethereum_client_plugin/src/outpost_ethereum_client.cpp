#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

#include <algorithm>
#include <cctype>
#include <optional>

#include <magic_enum/magic_enum.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/task/deadline.hpp>

#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>

namespace sysio {

namespace {

namespace eth = fc::network::ethereum;
namespace detail = outpost_ethereum_client_detail;

// ── Op labels used for deadline-exceeded error messages ──────────────────
constexpr std::string_view OP_DELIVER_OUTBOUND = "deliver_outbound_envelope";
constexpr std::string_view OP_READ_INBOUND     = "read_inbound_envelope";
constexpr std::string_view OP_UW_COMMIT        = "uw_commit";

/// ABI entry names and decoded-output field keys of the outpost contracts this
/// client drives. Grouped per contract so a Solidity rename is one edit here
/// rather than a scatter of string literals.
namespace opp_abi {
constexpr auto view_latest_outbound_envelope = "getLatestOutboundEnvelope";
namespace field {
constexpr auto epoch = "epoch_";
constexpr auto data  = "data_";
}
} // namespace opp_abi

namespace opp_inbound_abi {
constexpr auto view_envelope_chunk_state = "envelopeChunkState";
constexpr auto view_next_epoch_index     = "nextEpochIndex";
namespace field {
constexpr auto epoch_index     = "epochIndex";
constexpr auto owner           = "owner";
constexpr auto total_chunks    = "totalChunks";
constexpr auto received_chunks = "receivedChunks";
constexpr auto total_bytes     = "totalBytes";
constexpr auto stored_bytes    = "storedBytes";
}
} // namespace opp_inbound_abi

constexpr size_t EVM_ABI_WORD_BYTES            = 32;
constexpr size_t HEX_PREFIX_CHARS              = 2;
constexpr size_t HEX_CHARS_PER_BYTE            = 2;
constexpr size_t MAX_ENVELOPE_HEX_CHARS =
   HEX_PREFIX_CHARS + OPP_MAX_ENVELOPE_BYTES * HEX_CHARS_PER_BYTE;
constexpr size_t MAX_LATEST_OUTBOUND_RPC_BYTES =
   EVM_ABI_WORD_BYTES * 3 +
   ((OPP_MAX_ENVELOPE_BYTES + EVM_ABI_WORD_BYTES - 1) / EVM_ABI_WORD_BYTES) * EVM_ABI_WORD_BYTES;
constexpr size_t MAX_LATEST_OUTBOUND_RPC_HEX_CHARS =
   HEX_PREFIX_CHARS + MAX_LATEST_OUTBOUND_RPC_BYTES * HEX_CHARS_PER_BYTE;

/// Interpret one decoded ABI output word as an unsigned integer.
///
/// The libfc ABI decoder normalises every numeric output to a decimal string;
/// the integral fallback keeps this working if a future decoder emits raw
/// numbers. Returns `std::nullopt` when the variant is neither, so callers can
/// log the offending field and fail closed instead of silently reading 0.
std::optional<uint64_t> abi_uint_output(const fc::variant& value) {
   if (value.is_string()) {
      try {
         return std::stoull(value.as_string());
      } catch (const std::exception&) {
         return std::nullopt;
      }
   }
   if (value.is_uint64() || value.is_int64()) {
      return value.as_uint64();
   }
   return std::nullopt;
}

} // namespace

namespace outpost_ethereum_client_detail {

bool same_evm_address(std::string_view lhs, std::string_view rhs) {
   const auto strip_prefix = [](std::string_view value) {
      if (value.size() >= HEX_PREFIX_CHARS && value[0] == '0' &&
          (value[1] == 'x' || value[1] == 'X')) {
         value.remove_prefix(HEX_PREFIX_CHARS);
      }
      return value;
   };
   const auto left  = strip_prefix(lhs);
   const auto right = strip_prefix(rhs);
   if (left.empty() || left.size() != right.size()) return false;
   return std::ranges::equal(left, right, [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
   });
}

chunk_resume_decision decide_chunk_resume(const envelope_chunk_state& staged,
                                          std::string_view            self_address,
                                          uint32_t                    epoch_index,
                                          uint16_t                    total_chunks,
                                          uint32_t                    total_bytes) {
   // An empty header (`totalChunks == SLOT_EMPTY`) is the reclaimable state the
   // contract hands back for "never staged" and "already reset". Nothing to
   // adopt; chunk 0 stages it.
   if (staged.total_chunks == 0) {
      return {chunk_resume_action::start_fresh, 0};
   }

   // A header owned by someone else is not ours to continue or discard: the
   // view is owner-bound, so this can only mean the read was answered for a
   // different signer. Start from 0 and let the contract's ownership guard
   // arbitrate.
   if (!same_evm_address(staged.owner, self_address)) {
      return {chunk_resume_action::start_fresh, 0};
   }

   // Our own header, but for a different epoch. On a path-2 consensus retry
   // this is the signature of an epoch that advanced underneath us, so the
   // caller confirms against `nextEpochIndex()` before spending gas on late
   // no-op transactions.
   if (staged.epoch_index != epoch_index) {
      return {chunk_resume_action::confirm_epoch_advanced, 0};
   }

   // Our own CURRENT-epoch header whose declared shape belongs to a different
   // envelope, or one claiming more stored chunks than a well-formed upload can
   // hold (the final chunk is never stored, so `receivedChunks` tops out at
   // `totalChunks - 1`). Either way the staged cells are unusable: discard,
   // then upload from 0.
   if (staged.total_chunks != total_chunks || staged.total_bytes != total_bytes ||
       staged.received_chunks >= total_chunks) {
      return {chunk_resume_action::discard_and_restart, 0};
   }

   return {chunk_resume_action::resume, staged.received_chunks};
}

} // namespace outpost_ethereum_client_detail

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

   // Every OPPInbound staging header is bound to the delivering signer, so the
   // chunk-resume read needs this relay's own address on every multi-chunk
   // delivery. Derive it once here rather than per tick.
   _signer_address_hex = fc::to_hex(_entry->client->get_signer_address(), /*add_prefix=*/true);
}

sysio::opp::types::ChainKind outpost_ethereum_client::chain_kind() const {
   return sysio::opp::types::CHAIN_KIND_EVM;
}

std::vector<uint8_t>
outpost_ethereum_client::authenticated_caller_address() const {
   const auto address = _entry->client->get_signer_address();
   return {address.begin(), address.end()};
}

detail::envelope_chunk_state outpost_ethereum_client::read_envelope_chunk_state() {
   detail::envelope_chunk_state state;

   const auto& abi = _opp_inbound_client->get_abi(opp_inbound_abi::view_envelope_chunk_state);

   // Read at `latest`. This is our OWN staging high-water mark, not delivered
   // content WIRE commits consensus against, so the `finalized`-tag reasoning
   // that governs `read_inbound_envelope` does not apply — reading it at
   // `finalized` would re-send every chunk staged in an unfinalized block.
   std::string operator_address = _signer_address_hex;
   const auto  raw_hex_var =
      _opp_inbound_client->envelope_chunk_state(eth::block_tag_t::latest, operator_address);
   if (!raw_hex_var.is_string()) {
      wlog("outpost_ethereum_client[{}]: envelopeChunkState returned non-string variant",
           to_string());
      return state;
   }
   const std::string raw_hex = raw_hex_var.as_string();
   if (raw_hex.empty() || raw_hex == "0x") {
      dlog("outpost_ethereum_client[{}]: envelopeChunkState returned empty hex", to_string());
      return state;
   }

   const auto decoded = eth::contract_decode_data(abi, raw_hex);
   dlog("outpost_ethereum_client[{}]: envelopeChunkState decoded={}",
        to_string(), fc::json::to_string(decoded, fc::json::yield_function_t{}));
   if (!decoded.is_object()) {
      wlog("outpost_ethereum_client[{}]: envelopeChunkState result was not a variant object",
           to_string());
      return state;
   }

   const auto& obj = decoded.get_object();
   for (const auto* key : {opp_inbound_abi::field::epoch_index,
                           opp_inbound_abi::field::owner,
                           opp_inbound_abi::field::total_chunks,
                           opp_inbound_abi::field::received_chunks,
                           opp_inbound_abi::field::total_bytes,
                           opp_inbound_abi::field::stored_bytes}) {
      if (!obj.contains(key)) {
         wlog("outpost_ethereum_client[{}]: envelopeChunkState result missing '{}' key",
              to_string(), key);
         return state;
      }
   }

   const auto& owner_var = obj[opp_inbound_abi::field::owner];
   if (!owner_var.is_string()) {
      wlog("outpost_ethereum_client[{}]: envelopeChunkState owner was not a string", to_string());
      return state;
   }

   // Fail closed on the fields the resume DECISION reads: an unparsable one
   // must not be silently read as 0, which would fabricate an empty header.
   const auto epoch_index     = abi_uint_output(obj[opp_inbound_abi::field::epoch_index]);
   const auto total_chunks    = abi_uint_output(obj[opp_inbound_abi::field::total_chunks]);
   const auto received_chunks = abi_uint_output(obj[opp_inbound_abi::field::received_chunks]);
   const auto total_bytes     = abi_uint_output(obj[opp_inbound_abi::field::total_bytes]);
   if (!epoch_index || !total_chunks || !received_chunks || !total_bytes) {
      wlog("outpost_ethereum_client[{}]: envelopeChunkState carried an unparsable numeric field",
           to_string());
      return state;
   }

   state.owner           = owner_var.as_string();
   state.epoch_index     = static_cast<uint32_t>(*epoch_index);
   state.total_chunks    = static_cast<uint16_t>(*total_chunks);
   state.received_chunks = static_cast<uint16_t>(*received_chunks);
   state.total_bytes     = static_cast<uint32_t>(*total_bytes);
   // `storedBytes` is a uint256 on chain and feeds diagnostics ONLY — it is not
   // an input to `decide_chunk_resume`. Parse it separately with a 0 default so
   // a value above UINT64_MAX cannot discard an otherwise-valid header and
   // force a needless re-upload of chunks the outpost already holds.
   state.stored_bytes = abi_uint_output(obj[opp_inbound_abi::field::stored_bytes]).value_or(0);
   return state;
}

std::optional<uint16_t> outpost_ethereum_client::resume_chunk_index(
   uint32_t       epoch_index,
   uint16_t       total_chunks,
   uint32_t       total_bytes,
   fc::time_point deadline_abs) {
   throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);
   const auto staged   = read_envelope_chunk_state();
   const auto decision = detail::decide_chunk_resume(
      staged, _signer_address_hex, epoch_index, total_chunks, total_bytes);

   dlog("outpost_ethereum_client[{}]: chunk resume epoch={} total_chunks={} action={} "
        "staged(epoch={} owner={} chunks={} received={} bytes={})",
        to_string(), epoch_index, total_chunks,
        magic_enum::enum_name(decision.action),
        staged.epoch_index, staged.owner, staged.total_chunks,
        staged.received_chunks, staged.total_bytes);

   switch (decision.action) {
   case detail::chunk_resume_action::start_fresh:
      return uint16_t{0};

   case detail::chunk_resume_action::resume:
      ilog("outpost_ethereum_client[{}]: resuming epoch={} delivery at chunk {}/{} "
           "from the on-chain staging high-water mark",
           to_string(), epoch_index, decision.start_chunk, total_chunks);
      return decision.start_chunk;

   case detail::chunk_resume_action::discard_and_restart: {
      throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);
      try {
         const auto result = _opp_inbound_client->discard_envelope_chunks();
         ilog("outpost_ethereum_client[{}]: discarded a superseded epoch={} staging header "
              "(staged chunks={} bytes={}) tx={}",
              to_string(), epoch_index, staged.total_chunks, staged.total_bytes,
              result.as_string());
      } catch (const fc::network::json_rpc::json_rpc_error& e) {
         // A revert the node catches during `eth_estimateGas` arrives as a
         // JSON-RPC error, and for this call it means `OPP_ChunkBufferMissing`
         // — the header is already clear, which is exactly the state we wanted.
         // Tolerate it and upload from chunk 0.
         //
         // Deliberately NOT widened: a revert first observed at RECEIPT time
         // (status=0) is raised by `wait_for_confirmation` as a generic
         // `fc::exception` (`ethereum_client.cpp` "Ethereum tx {} reverted
         // (status=0)"), and so are transport failures and deadline expiry.
         // All three propagate and abandon the tick, which is benign: the next
         // cron tick re-reads the staging header and resumes from whatever the
         // chain actually holds.
         dlog("outpost_ethereum_client[{}]: discardEnvelopeChunks reverted at estimate time, "
              "treating the staging header as already clear: code={} message='{}'",
              to_string(), e.code, e.top_message());
      }
      return uint16_t{0};
   }

   case detail::chunk_resume_action::confirm_epoch_advanced: {
      throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);
      const auto next_var = _opp_inbound_client->next_epoch_index(eth::block_tag_t::latest);
      // `nextEpochIndex()` declares a single output, and `contract_decode_data`
      // returns a lone component directly rather than wrapping it.
      const auto next_epoch =
         next_var.is_string()
            ? abi_uint_output(eth::contract_decode_data(
                 _opp_inbound_client->get_abi(opp_inbound_abi::view_next_epoch_index),
                 next_var.as_string()))
            : std::optional<uint64_t>{};
      if (!next_epoch) {
         wlog("outpost_ethereum_client[{}]: could not read nextEpochIndex while resolving a "
              "stale staging header; delivering epoch={} from chunk 0",
              to_string(), epoch_index);
         return uint16_t{0};
      }
      if (*next_epoch > epoch_index) {
         ilog("outpost_ethereum_client[{}]: skipping epoch={} delivery — the outpost has "
              "already advanced to epoch {}",
              to_string(), epoch_index, *next_epoch);
         return std::nullopt;
      }
      return uint16_t{0};
   }
   }

   // Unreachable: the switch above covers every `chunk_resume_action`. An enum
   // can still hold an out-of-range value, so the compiler requires a fallback;
   // uploading from chunk 0 is the conservative one (the contract absorbs any
   // replayed chunk as an idempotent no-op).
   return uint16_t{0};
}

std::string outpost_ethereum_client::deliver_outbound_envelope(
   uint32_t                 epoch_index,
   const std::vector<char>& envelope_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);

   throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);
   FC_ASSERT(_opp_inbound_client,
             "outpost_ethereum_client[{}]: deliver_outbound_envelope requires an "
             "OPPInbound address — pass opp_inbound_addr to create_outpost_client",
             to_string());

   const size_t total = envelope_bytes.size();
   FC_ASSERT(total > 0,
             "outpost_ethereum_client[{}]: refusing to deliver an empty envelope",
             to_string());
   FC_ASSERT(total <= OPP_MAX_ENVELOPE_BYTES,
             "outpost_ethereum_client[{}]: envelope ({} bytes) exceeds the platform cap "
             "of {} bytes; OPPInbound will reject it",
             to_string(), total, OPP_MAX_ENVELOPE_BYTES);

   const uint16_t total_chunks = detail::chunk_count_for(total);

   // RESUME — multi-chunk deliveries only. A single-chunk envelope stages
   // nothing on chain (OPPInbound's fast path assembles straight from
   // calldata), so the dominant case pays no extra round trip.
   uint16_t start_chunk = 0;
   if (total_chunks > 1) {
      const auto resume =
         resume_chunk_index(epoch_index, total_chunks, static_cast<uint32_t>(total), deadline_abs);
      if (!resume) {
         // The outpost advanced past this epoch; every chunk would be a paid
         // late no-op. Returning empty marks the epoch handled for this tick.
         return {};
      }
      start_chunk = *resume;
   }

   // Sequential, receipt-confirmed submission — one transaction in flight at a
   // time, so the signer's nonce advances in lock-step and a mid-sequence
   // failure simply abandons the tick. The next cron tick restarts from the
   // on-chain high-water mark; the contract absorbs any replayed chunk as an
   // idempotent no-op.
   std::string last_tx;
   for (uint16_t chunk = start_chunk; chunk < total_chunks; ++chunk) {
      throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);

      const size_t offset = static_cast<size_t>(chunk) * ETHEREUM_MAX_CHUNK_BYTES;
      const size_t length = std::min(ETHEREUM_MAX_CHUNK_BYTES, total - offset);

      // `ethereum_contract_tx_fn` binds every argument as a non-const lvalue
      // reference, so each one needs a named local (the same constraint that
      // shapes `uw_commit`'s hex local).
      uint32_t    epoch_arg  = epoch_index;
      uint16_t    chunk_arg  = chunk;
      uint16_t    chunks_arg = total_chunks;
      uint32_t    bytes_arg  = static_cast<uint32_t>(total);
      std::string chunk_hex =
         fc::to_hex(envelope_bytes.data() + offset, static_cast<uint32_t>(length));

      const auto result =
         _opp_inbound_client->epoch_in(epoch_arg, chunk_arg, chunks_arg, bytes_arg, chunk_hex);
      last_tx = result.as_string();
      ilog("outpost_ethereum_client[{}]: epochIn chunk sent epoch={} chunk={}/{} bytes={} tx={}",
           to_string(), epoch_index, chunk, total_chunks, length, last_tx);
   }

   return last_tx;
}

std::vector<char> outpost_ethereum_client::read_inbound_envelope(
   uint32_t         epoch_index,
   fc::microseconds deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);

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
   const auto& abi = _opp_client->get_abi(opp_abi::view_latest_outbound_envelope);
   // Read at `finalized`, not `latest`. WIRE consensus on inbound is committed forward against this
   // read: an operator that reads a slot at `latest` can achieve WIRE-side consensus on it and queue
   // attestations off it, then watch that slot reorg out of Ethereum's canonical chain seconds later,
   // leaving WIRE committed to history that no longer exists. `finalized` is the only tag with
   // cryptoeconomic finality. This is deliberately not operator-configurable: the read commitment is a
   // consensus parameter, and operators reading at different commitments would deliver divergent
   // envelopes for the same epoch, manufacturing disputes among honest operators.
   const auto raw_hex_var = _opp_client->get_latest_outbound_envelope(eth::block_tag_t::finalized);
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
   if (raw_hex.size() > MAX_LATEST_OUTBOUND_RPC_HEX_CHARS) {
      wlog("outpost_ethereum_client[{}]: getLatestOutboundEnvelope raw hex "
           "({} chars) exceeds ABI envelope cap of {} chars",
           to_string(), raw_hex.size(), MAX_LATEST_OUTBOUND_RPC_HEX_CHARS);
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
   if (!obj.contains(opp_abi::field::epoch) || !obj.contains(opp_abi::field::data)) {
      wlog("outpost_ethereum_client[{}]: decoded view result missing epoch_/data_ keys",
           to_string());
      return {};
   }

   const auto stored_epoch_value = abi_uint_output(obj[opp_abi::field::epoch]);
   if (!stored_epoch_value) {
      wlog("outpost_ethereum_client[{}]: latestOutboundEnvelope epoch_ was not a parsable "
           "numeric output",
           to_string());
      return {};
   }
   const auto stored_epoch = static_cast<uint32_t>(*stored_epoch_value);
   if (stored_epoch == 0 || stored_epoch != epoch_index) {
      // Timing-only: outpost hasn't emitted yet (epoch=0) or the WIRE
      // batch op is querying a slightly stale tip. Both resolve on the
      // next poll. Keep at dlog so steady-state operation isn't noisy.
      dlog("outpost_ethereum_client[{}]: latestOutboundEnvelope epoch mismatch stored={} requested={}",
           to_string(), stored_epoch, epoch_index);
      return {};
   }

   const auto& data_var = obj[opp_abi::field::data];
   if (!data_var.is_string()) {
      wlog("outpost_ethereum_client[{}]: latestOutboundEnvelope data_ not a string",
           to_string());
      return {};
   }
   const std::string hex_data = data_var.as_string();
   if (hex_data.size() > MAX_ENVELOPE_HEX_CHARS) {
      wlog("outpost_ethereum_client[{}]: latestOutboundEnvelope data_ "
           "({} chars) exceeds envelope cap of {} chars",
           to_string(), hex_data.size(), MAX_ENVELOPE_HEX_CHARS);
      return {};
   }
   const auto raw = fc::crypto::ethereum::hex_to_bytes(hex_data);
   if (raw.empty()) return {};
   if (raw.size() > OPP_MAX_ENVELOPE_BYTES) {
      wlog("outpost_ethereum_client[{}]: latestOutboundEnvelope raw "
           "envelope ({} bytes) exceeds cap of {} bytes",
           to_string(), raw.size(), OPP_MAX_ENVELOPE_BYTES);
      return {};
   }

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
   fc::task::deadline_scope rpc_deadline(deadline_abs);

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
