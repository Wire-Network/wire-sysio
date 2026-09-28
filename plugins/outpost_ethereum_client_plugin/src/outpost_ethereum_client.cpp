#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

#include <algorithm>
#include <cctype>
#include <optional>

#include <magic_enum/magic_enum.hpp>

#include <fc/crypto/keccak256.hpp>
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

constexpr std::string_view OP_REALIZE_YIELD    = "crank_outpost:realizeYield";

/// Execution APIs code for a call the node executed and that reverted, as distinct from a
/// protocol error such as a parse failure, where the node never ran the call at all. Not an
/// EIP-1474 code — that spec only defines the negative range; 3 is geth's convention, which
/// `ethereum/execution-apis` standardised.
constexpr int ethereum_execution_reverted_code = 3;

/// Solidity signature of the revert `discardEnvelopeChunks()` raises when it has nothing of
/// ours to clear. Hashed rather than written out as its selector so a reader can check this
/// against the ABI directly; `chunk_buffer_missing_selector_is_pinned` holds the hash to the
/// four bytes the contract actually emits.

/// The three refusals `SyndicationPool.realizeYield()` raises for its own reasons
/// (Wire-Network/wire-ethereum#207), hashed the same way;
/// `realize_yield_refusal_selectors_are_pinned` holds each to the bytes the contract emits.
constexpr auto no_yield_signature             = "WIRE_NoYield()";
constexpr auto yield_below_deadband_signature = "WIRE_YieldBelowDeadband(uint64,uint64)";
constexpr auto pool_underbacked_signature     = "WIRE_PoolUnderbacked(uint64,uint64)";
/// OpenZeppelin `Pausable`'s refusal, raised by `realizeYield()`'s `whenNotPaused` while the
/// pool's panic role has frozen it (wire-ethereum `SyndicationPool.sol`).
constexpr auto enforced_pause_signature       = "EnforcedPause()";

/// `ATTESTATION_BLACKHOLE` in wire-ethereum's `OPPCommon.sol`: the handler governance
/// registers to drop an attestation type on purpose. Mirror duty, like
/// `ETHEREUM_MAX_CHUNK_BYTES`.
constexpr auto attestation_blackhole_address = "0x000000000000000000000000000000000000dead";

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

namespace syndication_pool_abi {
constexpr auto tx_realize_yield = "realizeYield";
} // namespace syndication_pool_abi

namespace opp_inbound_abi {
constexpr auto view_next_epoch_index   = "nextEpochIndex";
constexpr auto view_dispatch_spill     = "dispatchSpill";
constexpr auto view_epoch_deliveries   = "epochDeliveries";
constexpr auto view_pending_epoch_hash = "pendingEpochHash";
constexpr auto view_attestation_handlers = "attestationHandlers";
namespace field {
constexpr auto tipped     = "tipped";
constexpr auto dispatched = "dispatched";
constexpr auto complete   = "complete";
constexpr auto finalized  = "finalized";
}
} // namespace opp_inbound_abi

/// Most `epochIn` continuations one tick sends for one epoch before handing
/// the rest to the next tick. A bound, not a budget: a full-cap envelope
/// spills a handful of times at most, and a cursor still advancing past this
/// many is worth a log line and a fresh tick rather than an unbounded loop
/// inside one.
constexpr uint32_t MAX_CONTINUATIONS_PER_TICK = 32;

/// Block tag for reading this outpost's own delivery bookkeeping
/// (`nextEpochIndex`, `dispatchSpill`, `epochDeliveries`, `pendingEpochHash`).
/// `latest`, deliberately, where the inbound-envelope read uses `finalized`:
/// see `outpost_ethereum_client::read_dispatch_spill`.
constexpr auto PROGRESS_READ_TAG = eth::block_tag_t::latest;

constexpr size_t EVM_ABI_WORD_BYTES            = 32;
constexpr size_t EVM_ADDRESS_BYTES             = 20;
constexpr size_t EVM_SELECTOR_BYTES            = 4;
constexpr size_t HEX_PREFIX_CHARS              = 2;
constexpr size_t HEX_CHARS_PER_BYTE            = 2;
constexpr size_t MAX_ENVELOPE_HEX_CHARS =
   HEX_PREFIX_CHARS + OPP_MAX_ENVELOPE_BYTES * HEX_CHARS_PER_BYTE;
constexpr size_t MAX_LATEST_OUTBOUND_RPC_BYTES =
   EVM_ABI_WORD_BYTES * 3 +
   ((OPP_MAX_ENVELOPE_BYTES + EVM_ABI_WORD_BYTES - 1) / EVM_ABI_WORD_BYTES) * EVM_ABI_WORD_BYTES;
constexpr size_t MAX_LATEST_OUTBOUND_RPC_HEX_CHARS =
   HEX_PREFIX_CHARS + MAX_LATEST_OUTBOUND_RPC_BYTES * HEX_CHARS_PER_BYTE;

/// Drop a leading `0x` / `0X` if present. Hex is not self-delimiting, so every
/// comparison below works on the prefix-free form.
std::string_view strip_hex_prefix(std::string_view value) {
   if (value.size() >= HEX_PREFIX_CHARS && value[0] == '0' &&
       (value[1] == 'x' || value[1] == 'X')) {
      value.remove_prefix(HEX_PREFIX_CHARS);
   }
   return value;
}

/// Equal-length hex comparison, insensitive to case (EIP-55 checksum casing).
bool same_hex(std::string_view lhs, std::string_view rhs) {
   return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
   });
}

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

/// The four-byte selector of a Solidity function or error `signature`, as hex.
std::string selector_hex(const char* signature) {
   return fc::crypto::keccak256::hash(std::string(signature))
      .str()
      .substr(0, EVM_SELECTOR_BYTES * HEX_CHARS_PER_BYTE);
}

/// True when `abis` declares a function named `name`.
bool has_function_abi(const std::vector<eth::abi::contract>& abis, std::string_view name) {
   return std::ranges::any_of(abis, [&](const eth::abi::contract& contract) {
      return contract.type == eth::abi::invoke_target_type::function && contract.name == name;
   });
}

/// Interpret one decoded ABI output word as a boolean.
///
/// Tolerates every shape the decoder may hand back for a Solidity `bool` — a
/// native bool, the decimal strings the numeric path normalises to, or a raw
/// number. Returns `std::nullopt` for anything else so callers fail closed.
std::optional<bool> abi_bool_output(const fc::variant& value) {
   if (value.is_bool()) return value.as_bool();
   if (value.is_string()) {
      const auto text = value.as_string();
      if (text == "true") return true;
      if (text == "false") return false;
   }
   const auto numeric = abi_uint_output(value);
   if (numeric) return *numeric != 0;
   return std::nullopt;
}

/// Pull the single value out of a decoded view result.
///
/// libfc picks one of three shapes: a BARE value when the function has one
/// output, a named object when every output is named, and a positional array
/// otherwise. A public state-variable getter takes the first path because its
/// output carries no name; a struct getter takes the second. Reading a scalar
/// through this keeps a caller indifferent to which one the decoder chose.
fc::variant decoded_scalar(const fc::variant& decoded) {
   if (decoded.is_array()) {
      const auto& items = decoded.get_array();
      return items.empty() ? fc::variant() : items[0];
   }
   if (decoded.is_object()) {
      const auto& object = decoded.get_object();
      return object.size() == 0 ? fc::variant() : object.begin()->value();
   }
   return decoded;
}

/// A 32-byte word as the decoder renders it, normalised for comparison:
/// lower-case hex with no `0x` prefix. Empty when the value is not a string.
std::string normalized_word(const fc::variant& value) {
   if (!value.is_string()) return {};
   std::string word = value.as_string();
   if (word.size() >= HEX_PREFIX_CHARS && word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) {
      word.erase(0, HEX_PREFIX_CHARS);
   }
   std::ranges::transform(word, word.begin(), [](unsigned char c) { return std::tolower(c); });
   return word;
}

/// Whether a normalised word is the zero word (or empty — nothing recorded).
bool is_zero_word(std::string_view word) {
   return std::ranges::all_of(word, [](char c) { return c == '0'; });
}

} // namespace

namespace outpost_ethereum_client_detail {

bool same_evm_address(std::string_view lhs, std::string_view rhs) {
   const auto left = strip_hex_prefix(lhs);
   if (left.empty()) return false;
   return same_hex(left, strip_hex_prefix(rhs));
}

delivery_action decide_delivery(uint32_t              next_epoch_index,
                                uint32_t              epoch_index,
                                const dispatch_spill& spill,
                                bool                  own_delivery_settled) {
   // The outpost is past this epoch, or has closed it: every transaction
   // would be a paid late no-op. `finalized` is checked as well as the epoch
   // cursor because the two are written in one call and reads that race a
   // just-mined block may see one before the other.
   if (next_epoch_index > epoch_index || spill.finalized) {
      return delivery_action::already_finalized;
   }
   // Tipped but unfinished. The contract admits a continuation only from a
   // relay whose recorded delivery IS the settled digest, so a relay that has
   // not delivered, or delivered a minority envelope, has nothing to send.
   if (spill.tipped) {
      return own_delivery_settled ? delivery_action::continue_dispatch
                                  : delivery_action::wait_for_deliverer;
   }
   return delivery_action::deliver;
}

std::optional<realize_yield_refusal> classify_realize_yield_revert(std::string_view revert_data) {
   constexpr size_t selector_chars = EVM_SELECTOR_BYTES * HEX_CHARS_PER_BYTE;
   constexpr size_t word_chars     = EVM_ABI_WORD_BYTES * HEX_CHARS_PER_BYTE;

   const auto data = strip_hex_prefix(revert_data);
   if (data.size() < selector_chars) return std::nullopt;
   const auto selector       = data.substr(0, selector_chars);
   const auto argument_chars = data.size() - selector_chars;
   // Exact shape, as for `OPP_ChunkBufferMissing`: an error with other arguments hashes
   // differently, and a payload that does not fit the error is not that error.
   const auto is = [&](const char* signature, size_t words) {
      return argument_chars == words * word_chars && same_hex(selector, selector_hex(signature));
   };
   if (is(no_yield_signature, 0)) return realize_yield_refusal::no_yield;
   if (is(yield_below_deadband_signature, 2)) return realize_yield_refusal::below_deadband;
   if (is(pool_underbacked_signature, 2)) return realize_yield_refusal::underbacked;
   if (is(enforced_pause_signature, 0)) return realize_yield_refusal::paused;
   return std::nullopt;
}

std::optional<std::string> address_from_word(std::string_view raw_hex) {
   constexpr size_t word_chars    = EVM_ABI_WORD_BYTES * HEX_CHARS_PER_BYTE;
   constexpr size_t address_chars = EVM_ADDRESS_BYTES * HEX_CHARS_PER_BYTE;

   const auto word = strip_hex_prefix(raw_hex);
   if (word.size() != word_chars) return std::nullopt;
   if (!std::ranges::all_of(word, [](unsigned char c) { return std::isxdigit(c) != 0; })) return std::nullopt;
   // An address word is left-padded with zeros; anything else in the pad is not an address.
   if (word.find_first_not_of('0') < word_chars - address_chars) return std::nullopt;
   return "0x" + std::string(word.substr(word_chars - address_chars));
}

bool is_routable_handler(std::string_view handler_address) {
   const auto address = strip_hex_prefix(handler_address);
   if (address.empty() || address.find_first_not_of('0') == std::string_view::npos) return false;
   return !same_evm_address(handler_address, attestation_blackhole_address);
}

} // namespace outpost_ethereum_client_detail

outpost_ethereum_client::outpost_ethereum_client(
   ethereum_client_entry_ptr                         entry,
   std::string                                       opp_addr,
   std::string                                       opp_inbound_addr,
   std::vector<fc::network::ethereum::abi::contract> abis,
   uint64_t                                          chain_code,
   uint32_t                                          chain_id)
   : _entry(std::move(entry))
   , _opp_addr(std::move(opp_addr))
   , _opp_inbound_addr(std::move(opp_inbound_addr))
   , _abis(std::move(abis))
   , _outpost_id(chain_code)
   , _chain_id(chain_id) {
   FC_ASSERT(_entry && _entry->client, "ethereum_client_entry must carry a client");

   // Each contract wrapper is materialized only if its address was
   // supplied. A caller that only consumes one outpost capability (e.g.
   // the underwriter calling `uw_commit` against OperatorRegistry) can
   // pass empty strings for the addresses it doesn't use; the methods
   // covering an unprovisioned wrapper assert on entry with a clear
   // diagnostic. Per `outpost-client-spi.md`: address configuration is
   // a per-caller concern; the SPI shape stays uniform. The syndication
   // pool's wrapper is the exception: the outpost names the pool, so the
   // crank binds it when it discovers the address.
   if (!_opp_addr.empty()) {
      _opp_client = _entry->client->get_contract<opp_contract_client>(_opp_addr, _abis);
   }
   if (!_opp_inbound_addr.empty()) {
      _opp_inbound_client =
         _entry->client->get_contract<opp_inbound_contract_client>(_opp_inbound_addr, _abis);
   }

   // OPPInbound records every delivery under the delivering signer, and a
   // continuation is admitted only from a signer whose recorded digest is the
   // settled one, so the continuation check needs this relay's own address.
   // Derive it once here rather than per tick.
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

uint32_t outpost_ethereum_client::read_next_epoch_index() {
   const auto raw = _opp_inbound_client->next_epoch_index(PROGRESS_READ_TAG);
   FC_ASSERT(raw.is_string(),
             "outpost_ethereum_client[{}]: nextEpochIndex returned a non-string variant",
             to_string());
   const auto decoded = decoded_scalar(eth::contract_decode_data(
      _opp_inbound_client->get_abi(opp_inbound_abi::view_next_epoch_index), raw.as_string()));
   const auto value = abi_uint_output(decoded);
   FC_ASSERT(value.has_value(),
             "outpost_ethereum_client[{}]: nextEpochIndex was not a parsable numeric output",
             to_string());
   return static_cast<uint32_t>(*value);
}

detail::dispatch_spill outpost_ethereum_client::read_dispatch_spill(uint32_t epoch_index) {
   // `ethereum_contract_call_fn` binds its arguments as non-const lvalue
   // references, so the epoch needs a named local.
   uint32_t   epoch_arg = epoch_index;
   const auto raw       = _opp_inbound_client->dispatch_spill(PROGRESS_READ_TAG, epoch_arg);
   FC_ASSERT(raw.is_string(),
             "outpost_ethereum_client[{}]: dispatchSpill returned a non-string variant",
             to_string());
   const auto decoded = eth::contract_decode_data(
      _opp_inbound_client->get_abi(opp_inbound_abi::view_dispatch_spill), raw.as_string());
   dlog("outpost_ethereum_client[{}]: dispatchSpill({}) decoded={}",
        to_string(), epoch_index, fc::json::to_string(decoded, fc::json::yield_function_t{}));
   FC_ASSERT(decoded.is_object(),
             "outpost_ethereum_client[{}]: dispatchSpill result was not a variant object",
             to_string());

   // Fail closed on every field: an unparsable one must not be silently read
   // as "not tipped", which would send a fresh delivery into an epoch that
   // has already settled and be refused as a non-deliverer.
   const auto& object     = decoded.get_object();
   const auto  read_bool  = [&](const char* key) {
      FC_ASSERT(object.contains(key),
                "outpost_ethereum_client[{}]: dispatchSpill result missing '{}'", to_string(), key);
      const auto value = abi_bool_output(object[key]);
      FC_ASSERT(value.has_value(),
                "outpost_ethereum_client[{}]: dispatchSpill '{}' was not a boolean output",
                to_string(), key);
      return *value;
   };
   FC_ASSERT(object.contains(opp_inbound_abi::field::dispatched),
             "outpost_ethereum_client[{}]: dispatchSpill result missing '{}'",
             to_string(), opp_inbound_abi::field::dispatched);
   const auto dispatched = abi_uint_output(object[opp_inbound_abi::field::dispatched]);
   FC_ASSERT(dispatched.has_value(),
             "outpost_ethereum_client[{}]: dispatchSpill '{}' was not a numeric output",
             to_string(), opp_inbound_abi::field::dispatched);

   detail::dispatch_spill spill;
   spill.tipped     = read_bool(opp_inbound_abi::field::tipped);
   spill.dispatched = static_cast<uint16_t>(*dispatched);
   spill.complete   = read_bool(opp_inbound_abi::field::complete);
   spill.finalized  = read_bool(opp_inbound_abi::field::finalized);
   return spill;
}

bool outpost_ethereum_client::own_delivery_settled(uint32_t epoch_index) {
   uint32_t    epoch_arg    = epoch_index;
   std::string operator_arg = _signer_address_hex;
   const auto  delivered_raw =
      _opp_inbound_client->epoch_deliveries(PROGRESS_READ_TAG, epoch_arg, operator_arg);
   const auto settled_raw = _opp_inbound_client->pending_epoch_hash(PROGRESS_READ_TAG);
   FC_ASSERT(delivered_raw.is_string() && settled_raw.is_string(),
             "outpost_ethereum_client[{}]: epochDeliveries / pendingEpochHash returned a "
             "non-string variant",
             to_string());

   const auto delivered = normalized_word(decoded_scalar(eth::contract_decode_data(
      _opp_inbound_client->get_abi(opp_inbound_abi::view_epoch_deliveries), delivered_raw.as_string())));
   const auto settled = normalized_word(decoded_scalar(eth::contract_decode_data(
      _opp_inbound_client->get_abi(opp_inbound_abi::view_pending_epoch_hash), settled_raw.as_string())));
   dlog("outpost_ethereum_client[{}]: epoch={} own delivery={} settled={}",
        to_string(), epoch_index, delivered, settled);

   // A zero digest is "nothing recorded" on either side and never a match: an
   // epoch cannot settle on the zero digest, and a relay that has not
   // delivered has no claim on the continuation.
   if (is_zero_word(delivered) || is_zero_word(settled)) return false;
   return delivered == settled;
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

   // ONE call carries the whole envelope. Ethereum bounds a transaction by
   // gas, not size, and a full-cap envelope is ~1.3 M gas of calldata against
   // EIP-7825's 16.7 M cap; what may not fit is DISPATCH, which the contract
   // spills across continuations of this same call. The envelope is
   // re-supplied on every continuation — the outpost never stores its bytes.
   const std::string envelope_hex = fc::to_hex(envelope_bytes.data(), static_cast<uint32_t>(total));

   // Sequential, receipt-confirmed submission — one transaction in flight at a
   // time, so the signer's nonce advances in lock-step and a mid-sequence
   // failure simply abandons the tick. The next cron tick re-reads the
   // outpost's cursor and picks up wherever the chain actually is.
   std::string last_tx;
   bool        delivered_this_tick = false;
   for (uint32_t call = 0; call <= MAX_CONTINUATIONS_PER_TICK; ++call) {
      throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);

      const auto next_epoch = read_next_epoch_index();
      const auto spill      = read_dispatch_spill(epoch_index);
      // The two settlement reads are only meaningful — and only paid for —
      // when the epoch is mid-flight.
      const bool settled = spill.tipped && !spill.finalized && own_delivery_settled(epoch_index);
      const auto action  = detail::decide_delivery(next_epoch, epoch_index, spill, settled);
      dlog("outpost_ethereum_client[{}]: epoch={} action={} next_epoch={} "
           "spill(tipped={} dispatched={} complete={} finalized={}) settled={}",
           to_string(), epoch_index, magic_enum::enum_name(action), next_epoch,
           spill.tipped, spill.dispatched, spill.complete, spill.finalized, settled);

      switch (action) {
      case detail::delivery_action::already_finalized:
         if (last_tx.empty()) {
            // Nothing sent: the outpost was already past this epoch when the
            // tick began. An EMPTY tx id tells the job the epoch is handled.
            ilog("outpost_ethereum_client[{}]: skipping epoch={} delivery — the outpost has "
                 "already finalized it (nextEpochIndex={})",
                 to_string(), epoch_index, next_epoch);
         }
         return last_tx;

      case detail::delivery_action::wait_for_deliverer:
         ilog("outpost_ethereum_client[{}]: epoch={} tipped on a digest this relay did not "
              "deliver (dispatched={} complete={}); its deliverers carry the continuation",
              to_string(), epoch_index, spill.dispatched, spill.complete);
         return last_tx;

      case detail::delivery_action::deliver:
         if (delivered_this_tick) {
            // Our delivery is recorded and consensus has not tipped: the
            // remaining group members have yet to deliver. Re-sending would
            // be a paid no-op; the job's boundary-gated retry covers path 2.
            dlog("outpost_ethereum_client[{}]: epoch={} delivery recorded; consensus not yet tipped",
                 to_string(), epoch_index);
            return last_tx;
         }
         break;

      case detail::delivery_action::continue_dispatch:
         break;
      }

      throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);
      // `ethereum_contract_tx_fn` binds every argument as a non-const lvalue
      // reference, so each one needs a named local.
      uint32_t    epoch_arg   = epoch_index;
      std::string payload_hex = envelope_hex;
      const auto  result      = _opp_inbound_client->epoch_in(epoch_arg, payload_hex);
      last_tx                 = result.as_string();

      if (action == detail::delivery_action::deliver) {
         delivered_this_tick = true;
         ilog("outpost_ethereum_client[{}]: epochIn delivered epoch={} bytes={} tx={}",
              to_string(), epoch_index, total, last_tx);
      } else {
         ilog("outpost_ethereum_client[{}]: epochIn continued epoch={} from attestation {} "
              "(complete={}) tx={}",
              to_string(), epoch_index, spill.dispatched, spill.complete, last_tx);
      }
   }

   wlog("outpost_ethereum_client[{}]: epoch={} still not finalized after {} continuations in "
        "one tick; the next tick resumes from the outpost's cursor",
        to_string(), epoch_index, MAX_CONTINUATIONS_PER_TICK);
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

void outpost_ethereum_client::bind_syndication_pool(
   std::string address, std::shared_ptr<syndication_pool_contract_client> client) {
   FC_ASSERT(client, "outpost_ethereum_client[{}]: bind_syndication_pool needs a wrapper", to_string());
   _syndication_pool_addr   = std::move(address);
   _syndication_pool_client = std::move(client);
}

std::optional<std::string> outpost_ethereum_client::discover_syndication_pool() {
   // The routing table is configuration, not delivered content, so `latest` is right for the
   // same reason it is for the staging-header read.
   uint16_t attestation_type = static_cast<uint16_t>(
      magic_enum::enum_integer(sysio::opp::types::ATTESTATION_TYPE_DESYNDICATE_LIQ));
   const auto raw = _opp_inbound_client->attestation_handlers(eth::block_tag_t::latest, attestation_type);
   if (!raw.is_string()) {
      wlog("outpost_ethereum_client[{}]: attestationHandlers returned non-string variant", to_string());
      return std::nullopt;
   }
   const auto handler = detail::address_from_word(raw.as_string());
   if (!handler) {
      wlog("outpost_ethereum_client[{}]: attestationHandlers returned an unparsable word: {}",
           to_string(), raw.as_string());
      return std::nullopt;
   }
   if (!detail::is_routable_handler(*handler)) {
      dlog("outpost_ethereum_client[{}]: no DESYNDICATE_LIQ handler is registered -- no syndication "
           "pool to crank",
           to_string());
      return std::nullopt;
   }
   return handler;
}

void outpost_ethereum_client::crank_outpost(uint32_t epoch_index, fc::microseconds deadline) {
   // The pool's ABI ships with wire-ethereum #207; an ABI set without `realizeYield` is an
   // outpost deployment that predates the pool, and there is nothing to crank on it. Without
   // the OPPInbound wrapper there is no routing table to discover the pool from.
   if (!_opp_inbound_client || !has_function_abi(_abis, syndication_pool_abi::tx_realize_yield)) {
      dlog("outpost_ethereum_client[{}]: no syndication pool ABI -- nothing to crank", to_string());
      return;
   }

   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);
   throw_if_past_deadline(deadline_abs, OP_REALIZE_YIELD);

   const auto pool = discover_syndication_pool();
   if (!pool) return;
   if (!_syndication_pool_client || !detail::same_evm_address(_syndication_pool_addr, *pool)) {
      ilog("outpost_ethereum_client[{}]: syndication pool {} is the outpost's DESYNDICATE_LIQ handler",
           to_string(), *pool);
      bind_syndication_pool(*pool,
                            _entry->client->get_contract<syndication_pool_contract_client>(*pool, _abis));
   }

   throw_if_past_deadline(deadline_abs, OP_REALIZE_YIELD);
   try {
      const auto result = _syndication_pool_client->realize_yield();
      ilog("outpost_ethereum_client[{}]: realizeYield sent for epoch {} tx={}",
           to_string(), epoch_index, result.as_string());
   } catch (const fc::network::json_rpc::json_rpc_error& e) {
      // A revert at estimate time costs no gas. Only the pool's own refusals -- its three
      // yield outcomes and `EnforcedPause()`, which comes only from SyndicationPool's own pause
      // (the OPP endpoint has none) -- are outcomes of the crank rather than failures of it;
      // anything else -- a signer without the `yield_operator` role, a foreign implementation --
      // is the job's to log as a failed crank, exactly like a transport failure.
      const auto refusal =
         e.code == ethereum_execution_reverted_code
            ? detail::classify_realize_yield_revert(e.data.is_string() ? e.data.as_string() : std::string{})
            : std::nullopt;
      if (!refusal) throw;
      switch (*refusal) {
      case detail::realize_yield_refusal::no_yield:
      case detail::realize_yield_refusal::below_deadband:
         dlog("outpost_ethereum_client[{}]: realizeYield has nothing to report for epoch {} ({})",
              to_string(), epoch_index, magic_enum::enum_name(*refusal));
         return;
      case detail::realize_yield_refusal::underbacked:
         wlog("outpost_ethereum_client[{}]: syndication pool {} is below its principal; realizeYield "
              "refused for epoch {} (the loss path is not in that contract)",
              to_string(), _syndication_pool_addr, epoch_index);
         return;
      case detail::realize_yield_refusal::paused:
         // The emergency stop: an expected state the andon playbook clears, reported every
         // epoch at info so it stays visible without reading as a failed crank.
         ilog("outpost_ethereum_client[{}]: syndication pool {} is paused; realizeYield not reported "
              "for epoch {}",
              to_string(), _syndication_pool_addr, epoch_index);
         return;
      }
   }
}

} // namespace sysio
