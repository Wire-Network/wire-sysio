#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
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
#include <fc/task/retry.hpp>
#include <fc/time.hpp>

#include <sysio/chain/exceptions.hpp>
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

/// Retry label for the pinned progress read, see `PINNED_READ_RETRY`.
constexpr std::string_view OP_READ_PROGRESS    = "ethereum:read_progress";

/// Execution APIs code for a call the node executed and that reverted, as distinct from a
/// protocol error such as a parse failure, where the node never ran the call at all. Not an
/// EIP-1474 code — that spec only defines the negative range; 3 is geth's convention, which
/// `ethereum/execution-apis` standardised.
constexpr int ethereum_execution_reverted_code = 3;

/// Solidity signatures of the `OPPInbound.epochIn` refusals the relay tells apart
/// (wire-ethereum `OPPErrors.sol`). Hashed rather than written out as selectors so a reader
/// can check them against the ABI directly; `epoch_in_revert_selectors_are_pinned` holds
/// each to the four bytes the contract actually emits. `OPP_HandlerGasExhausted` carries the
/// attestation type as the `uint16` the enum's ABI encoding is.
constexpr auto dispatch_underfunded_signature        = "OPP_DispatchUnderfunded(uint256,uint256)";
constexpr auto handler_gas_exhausted_signature       = "OPP_HandlerGasExhausted(address,uint16,uint256)";
constexpr auto non_sequential_epoch_signature        = "OPP_NonSequentialEpoch(uint32,uint32)";
constexpr auto operator_already_delivered_signature  = "OPP_OperatorAlreadyDelivered(uint32,address)";
constexpr auto not_active_operator_signature         = "OPP_NotActiveOperator(address)";
constexpr auto digest_mismatch_signature             = "OPP_DigestMismatch(bytes32,bytes32)";

/// Solidity signatures of the `OPPInbound` events a delivery's receipt carries, hashed to
/// the topic the node reports; `delivery_event_topics_are_pinned` holds each to the bytes.
constexpr auto epoch_delivery_event_signature            = "EpochDelivery(uint32,address,bytes32)";
constexpr auto epoch_consensus_event_signature           = "EpochConsensus(uint32,bytes32,uint32)";
constexpr auto epoch_dispatch_progressed_event_signature = "EpochDispatchProgressed(uint32,uint16)";
constexpr auto epoch_complete_event_signature            = "EpochComplete(uint32)";

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
/// registers to drop an attestation type on purpose. A compiled-in mirror of the contract
/// constant, never configured.
constexpr auto attestation_blackhole_address = "0x000000000000000000000000000000000000dead";

/// ABI entry names and decoded-output field keys of the outpost contracts this
/// client drives. Grouped per contract so a Solidity rename is one edit here
/// rather than a scatter of string literals. The OPPInbound entry names live
/// with the wrapper (`opp_inbound_abi_name`); only its decoded field keys are
/// here.
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
namespace field {
constexpr auto tipped                  = "tipped";
constexpr auto dispatched              = "dispatched";
constexpr auto complete                = "complete";
constexpr auto finalized               = "finalized";
constexpr auto next_epoch              = "nextEpoch";
constexpr auto agreeing                = "agreeing";
constexpr auto group_size              = "groupSize";
constexpr auto current_epoch_started_at = "currentEpochStartedAtTs";
constexpr auto epoch_duration_sec      = "epochDurationSec_";
}
} // namespace opp_inbound_abi

/// `eth_getTransactionReceipt` log entry field names.
namespace receipt_log_field {
constexpr auto address = "address";
constexpr auto topics  = "topics";
constexpr auto data    = "data";
} // namespace receipt_log_field

/// Most `epochIn` calls one tick sends for one epoch before handing the rest
/// to the next tick. A bound, not a budget: a full-cap envelope spills a
/// handful of times at most, and a cursor still advancing past this many is
/// worth a log line and a fresh tick rather than an unbounded loop inside one.
constexpr uint32_t MAX_CONTINUATIONS_PER_TICK = 32;

/// Most times one tick re-reads the outpost after the node refused a call for
/// a reason that says the state moved under the read (`OPP_NonSequentialEpoch`
/// and its siblings). A refusal that survives this many fresh reads is not a
/// race, and the tick reports it instead of reading again.
constexpr uint32_t MAX_STALE_REFUSALS_PER_TICK = 2;

/// How the pinned progress read retries a backend that does not know the
/// block yet (a load-balanced endpoint behind the one that confirmed the
/// transaction). Short, because the block exists: the backend is behind by
/// seconds, not minutes, and the tick's own deadline bounds it anyway.
constexpr fc::task::retry_options PINNED_READ_RETRY{
   .initial_backoff = fc::milliseconds(250),
   .max_backoff     = fc::seconds(1),
   .total_timeout   = fc::seconds(10),
};

constexpr size_t EVM_ABI_WORD_BYTES            = 32;
constexpr size_t EVM_ADDRESS_BYTES             = 20;
constexpr size_t EVM_SELECTOR_BYTES            = 4;
constexpr size_t HEX_PREFIX_CHARS              = 2;
constexpr size_t HEX_CHARS_PER_BYTE            = 2;
constexpr size_t EVM_ABI_WORD_HEX_CHARS        = EVM_ABI_WORD_BYTES * HEX_CHARS_PER_BYTE;
constexpr size_t EVM_SELECTOR_HEX_CHARS        = EVM_SELECTOR_BYTES * HEX_CHARS_PER_BYTE;
/// Hex digits a `uint64` occupies at the low end of a word; a word with a
/// non-zero digit above them does not fit a `uint64`.
constexpr size_t UINT64_HEX_CHARS              = sizeof(uint64_t) * HEX_CHARS_PER_BYTE;
constexpr int    HEX_RADIX                     = 16;
constexpr size_t MAX_ENVELOPE_HEX_CHARS =
   HEX_PREFIX_CHARS + OPP_MAX_ENVELOPE_BYTES * HEX_CHARS_PER_BYTE;
constexpr size_t MAX_LATEST_OUTBOUND_RPC_BYTES =
   EVM_ABI_WORD_BYTES * 3 +
   ((OPP_MAX_ENVELOPE_BYTES + EVM_ABI_WORD_BYTES - 1) / EVM_ABI_WORD_BYTES) * EVM_ABI_WORD_BYTES;
constexpr size_t MAX_LATEST_OUTBOUND_RPC_HEX_CHARS =
   HEX_PREFIX_CHARS + MAX_LATEST_OUTBOUND_RPC_BYTES * HEX_CHARS_PER_BYTE;

/// Topic index of the first indexed argument in a receipt log (`topics[0]` is
/// the event signature).
constexpr size_t FIRST_INDEXED_TOPIC = 1;
constexpr size_t SECOND_INDEXED_TOPIC = 2;

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

/// The keccak256 of a Solidity signature as prefix-free lower-case hex: the
/// event topic, or (its first four bytes) the function / error selector.
std::string signature_hash_hex(const char* signature) {
   return fc::crypto::keccak256::hash(std::string(signature)).str();
}

/// The four-byte selector of a Solidity function or error `signature`, as hex.
std::string selector_hex(const char* signature) {
   return signature_hash_hex(signature).substr(0, EVM_SELECTOR_HEX_CHARS);
}

/// True when `abis` declares a function named `name`.
bool has_function_abi(const std::vector<eth::abi::contract>& abis, std::string_view name) {
   return std::ranges::any_of(abis, [&](const eth::abi::contract& contract) {
      return contract.type == eth::abi::invoke_target_type::function && contract.name == name;
   });
}

/// Hex normalised for comparison: lower-case with no `0x` prefix.
std::string normalized_hex(std::string_view value) {
   std::string hex{strip_hex_prefix(value)};
   std::ranges::transform(hex, hex.begin(), [](unsigned char c) { return std::tolower(c); });
   return hex;
}

/// A 32-byte word as the decoder renders it, normalised for comparison:
/// lower-case hex with no `0x` prefix. Empty when the value is not a string.
std::string normalized_word(const fc::variant& value) {
   if (!value.is_string()) return {};
   return normalized_hex(value.as_string());
}

/// Whether a normalised word is the zero word (or empty — nothing recorded).
bool is_zero_word(std::string_view word) {
   return std::ranges::all_of(word, [](char c) { return c == '0'; });
}

/// Whether `word` is exactly one ABI word of hex digits.
bool is_hex_word(std::string_view word) {
   return word.size() == EVM_ABI_WORD_HEX_CHARS &&
          std::ranges::all_of(word, [](unsigned char c) { return std::isxdigit(c) != 0; });
}

/// The unsigned integer one normalised ABI word holds, or `std::nullopt` when
/// the word is malformed or does not fit a `uint64`.
std::optional<uint64_t> word_to_uint(std::string_view word) {
   if (!is_hex_word(word)) return std::nullopt;
   const auto high = word.substr(0, EVM_ABI_WORD_HEX_CHARS - UINT64_HEX_CHARS);
   if (!is_zero_word(high)) return std::nullopt;
   try {
      return std::stoull(std::string(word.substr(high.size())), nullptr, HEX_RADIX);
   } catch (const std::exception&) {
      return std::nullopt;
   }
}

/// The `n`-th ABI word of a receipt log's normalised `data`, or `std::nullopt`
/// when the data is shorter than that.
std::optional<std::string_view> data_word(std::string_view data, size_t n) {
   const size_t offset = n * EVM_ABI_WORD_HEX_CHARS;
   if (data.size() < offset + EVM_ABI_WORD_HEX_CHARS) return std::nullopt;
   return data.substr(offset, EVM_ABI_WORD_HEX_CHARS);
}

/// The raw `eth_call` hex of one OPPInbound view, asserted to be a string.
std::string view_hex(const fc::variant& raw, std::string_view client_label, const char* view) {
   FC_ASSERT(raw.is_string(), "outpost_ethereum_client[{}]: {} returned a non-string variant", client_label, view);
   return raw.as_string();
}

/// `OPPInbound.nextEpochIndex()` at `block`.
uint32_t read_next_epoch_index(opp_inbound_contract_client& inbound, const eth::block_number_or_tag_t& block,
                               std::string_view client_label) {
   const auto raw = view_hex(inbound.next_epoch_index(block), client_label, opp_inbound_abi_name::next_epoch_index);
   // A single-output view decodes to the bare value.
   const auto value =
      abi_uint_output(eth::contract_decode_data(inbound.get_abi(opp_inbound_abi_name::next_epoch_index), raw));
   FC_ASSERT(value.has_value(), "outpost_ethereum_client[{}]: nextEpochIndex was not a parsable numeric output",
             client_label);
   return static_cast<uint32_t>(*value);
}

/// `OPPInbound.dispatchSpill(epoch_index)` at `block`, decoded.
///
/// Fails closed on every field: an unparsable one must not be silently read
/// as "not tipped", which would send a fresh delivery into an epoch that has
/// already settled and be refused as a non-deliverer.
detail::dispatch_spill read_dispatch_spill(opp_inbound_contract_client& inbound, uint32_t epoch_index,
                                           const eth::block_number_or_tag_t& block, std::string_view client_label) {
   // `ethereum_contract_call_fn` binds its arguments as non-const lvalue
   // references, so the epoch needs a named local.
   uint32_t   epoch_arg = epoch_index;
   const auto raw = view_hex(inbound.dispatch_spill(block, epoch_arg), client_label, opp_inbound_abi_name::dispatch_spill);
   const auto decoded = eth::contract_decode_data(inbound.get_abi(opp_inbound_abi_name::dispatch_spill), raw);
   dlog("outpost_ethereum_client[{}]: dispatchSpill({}) decoded={}", client_label, epoch_index,
        fc::json::to_string(decoded, fc::json::yield_function_t{}));
   FC_ASSERT(decoded.is_object(), "outpost_ethereum_client[{}]: dispatchSpill result was not a variant object",
             client_label);

   const auto& object    = decoded.get_object();
   // The decoder hands a Solidity `bool` back as a native bool; anything else is a malformed read.
   const auto  read_bool = [&](const char* key) {
      FC_ASSERT(object.contains(key), "outpost_ethereum_client[{}]: dispatchSpill result missing '{}'", client_label,
                key);
      FC_ASSERT(object[key].is_bool(), "outpost_ethereum_client[{}]: dispatchSpill '{}' was not a boolean output",
                client_label, key);
      return object[key].as_bool();
   };
   FC_ASSERT(object.contains(opp_inbound_abi::field::dispatched),
             "outpost_ethereum_client[{}]: dispatchSpill result missing '{}'", client_label,
             opp_inbound_abi::field::dispatched);
   const auto dispatched = abi_uint_output(object[opp_inbound_abi::field::dispatched]);
   FC_ASSERT(dispatched.has_value(), "outpost_ethereum_client[{}]: dispatchSpill '{}' was not a numeric output",
             client_label, opp_inbound_abi::field::dispatched);

   detail::dispatch_spill spill;
   spill.tipped     = read_bool(opp_inbound_abi::field::tipped);
   spill.dispatched = static_cast<uint16_t>(*dispatched);
   spill.complete   = read_bool(opp_inbound_abi::field::complete);
   spill.finalized  = read_bool(opp_inbound_abi::field::finalized);
   return spill;
}

/// `OPPInbound.epochDeliveries(epoch_index, operator)` at `block`, normalised.
std::string read_own_digest(opp_inbound_contract_client& inbound, uint32_t epoch_index,
                            const std::string& operator_address, const eth::block_number_or_tag_t& block,
                            std::string_view client_label) {
   uint32_t    epoch_arg    = epoch_index;
   std::string operator_arg = operator_address;
   const auto  raw = view_hex(inbound.epoch_deliveries(block, epoch_arg, operator_arg), client_label,
                              opp_inbound_abi_name::epoch_deliveries);
   return normalized_word(eth::contract_decode_data(inbound.get_abi(opp_inbound_abi_name::epoch_deliveries), raw));
}

/// `OPPInbound.pendingEpochHash()` at `block`, normalised.
std::string read_settled_digest(opp_inbound_contract_client& inbound, const eth::block_number_or_tag_t& block,
                                std::string_view client_label) {
   const auto raw = view_hex(inbound.pending_epoch_hash(block), client_label, opp_inbound_abi_name::pending_epoch_hash);
   return normalized_word(eth::contract_decode_data(inbound.get_abi(opp_inbound_abi_name::pending_epoch_hash), raw));
}

/// The block parameter for a read pinned to `block`.
eth::block_number_or_tag_t pinned(const fc::uint256& block) {
   return eth::format_rpc_quantity(block);
}

/// `json_rpc_error::data` as the revert hex it carries, or empty when the
/// node attached none.
std::string revert_hex(const fc::network::json_rpc::json_rpc_error& refusal) {
   return refusal.data.is_string() ? refusal.data.as_string() : std::string{};
}

} // namespace

namespace outpost_ethereum_client_detail {

bool same_evm_address(std::string_view lhs, std::string_view rhs) {
   const auto left = strip_hex_prefix(lhs);
   if (left.empty()) return false;
   return same_hex(left, strip_hex_prefix(rhs));
}

delivery_settlement classify_settlement(std::string_view own_digest, std::string_view settled_digest,
                                        bool tipped) {
   FC_ASSERT(is_hex_word(own_digest), "epochDeliveries did not return one 32-byte word: '{}'", own_digest);
   FC_ASSERT(is_hex_word(settled_digest), "pendingEpochHash did not return one 32-byte word: '{}'", settled_digest);
   if (is_zero_word(own_digest)) return delivery_settlement::never_delivered;
   // Until the tip, `pendingEpochHash` is an earlier epoch's digest (or the
   // zero word on a fresh outpost): nothing to compare this epoch's record to.
   if (!tipped) return delivery_settlement::recorded;
   return same_hex(own_digest, settled_digest) ? delivery_settlement::settled : delivery_settlement::divergent;
}

bool majority_tip_reachable(const pending_consensus& consensus, uint64_t now_sec) {
   if (consensus.group_size == 0) return false;
   const uint64_t boundary = consensus.epoch_started_at + consensus.epoch_duration_sec;
   // STRICT majority, as the contract computes it (`groupSize / 2 + 1`).
   const uint32_t majority = consensus.group_size / 2 + 1;
   return now_sec >= boundary && consensus.agreeing >= majority;
}

delivery_action decide_delivery(uint32_t              next_epoch_index,
                                uint32_t              epoch_index,
                                const dispatch_spill& spill,
                                delivery_settlement   settlement,
                                bool                  majority_reachable) {
   // The outpost is past this epoch, or has closed it: every transaction
   // would be a paid late no-op. `finalized` is checked as well as the epoch
   // cursor because the two are written in one call and reads that race a
   // just-mined block may see one before the other.
   if (next_epoch_index > epoch_index || spill.finalized) {
      return delivery_action::already_finalized;
   }
   // The outpost has not reached this epoch: `epochIn` refuses it as
   // non-sequential until the earlier one finalizes.
   if (next_epoch_index < epoch_index) {
      return delivery_action::outpost_behind;
   }
   // Tipped but unfinished. The contract admits a continuation only from a
   // relay whose recorded delivery IS the settled digest, so a relay that has
   // not delivered, or delivered a minority envelope, has nothing to send.
   if (spill.tipped) {
      return settlement == delivery_settlement::settled ? delivery_action::continue_dispatch
                                                        : delivery_action::wait_for_deliverer;
   }
   if (settlement == delivery_settlement::never_delivered) return delivery_action::deliver;
   // Recorded and untipped: the rest of the group has yet to deliver. A
   // re-delivery re-runs the tip, which only settles anything once the
   // contract's path-2 predicate holds.
   return majority_reachable ? delivery_action::retry_consensus : delivery_action::await_peers;
}

bool advanced(const delivery_progress& before, const delivery_progress& after) {
   if (after.next_epoch_index > before.next_epoch_index) return true;
   if (after.spill.finalized && !before.spill.finalized) return true;
   if (after.spill.complete && !before.spill.complete) return true;
   if (after.spill.tipped && !before.spill.tipped) return true;
   if (after.spill.dispatched > before.spill.dispatched) return true;
   return is_zero_word(before.own_digest) && !is_zero_word(after.own_digest);
}

std::optional<epoch_in_revert> classify_epoch_in_revert(int rpc_code, std::string_view revert_data) {
   if (rpc_code != ethereum_execution_reverted_code) return std::nullopt;
   const auto data = strip_hex_prefix(revert_data);
   if (data.size() < EVM_SELECTOR_HEX_CHARS) return std::nullopt;
   const auto selector       = data.substr(0, EVM_SELECTOR_HEX_CHARS);
   const auto argument_chars = data.size() - EVM_SELECTOR_HEX_CHARS;
   // Exact shape: an error with other arguments hashes differently, and a
   // payload that does not fit the error is not that error.
   const auto is = [&](const char* signature, size_t words) {
      return argument_chars == words * EVM_ABI_WORD_HEX_CHARS && same_hex(selector, selector_hex(signature));
   };
   if (is(dispatch_underfunded_signature, 2)) return epoch_in_revert::dispatch_underfunded;
   if (is(handler_gas_exhausted_signature, 3)) return epoch_in_revert::handler_gas_exhausted;
   if (is(non_sequential_epoch_signature, 2)) return epoch_in_revert::non_sequential_epoch;
   if (is(operator_already_delivered_signature, 2)) return epoch_in_revert::operator_already_delivered;
   if (is(not_active_operator_signature, 1)) return epoch_in_revert::not_active_operator;
   if (is(digest_mismatch_signature, 2)) return epoch_in_revert::digest_mismatch;
   return std::nullopt;
}

delivery_receipt_summary summarize_delivery_receipt(const fc::variants& logs, std::string_view inbound_address,
                                                    uint32_t epoch_index, std::string_view operator_address) {
   const auto delivery_topic   = signature_hash_hex(epoch_delivery_event_signature);
   const auto consensus_topic  = signature_hash_hex(epoch_consensus_event_signature);
   const auto progressed_topic = signature_hash_hex(epoch_dispatch_progressed_event_signature);
   const auto complete_topic   = signature_hash_hex(epoch_complete_event_signature);

   delivery_receipt_summary summary;
   for (const auto& entry : logs) {
      if (!entry.is_object()) continue;
      const auto& log = entry.get_object();
      if (!log.contains(receipt_log_field::address) || !log[receipt_log_field::address].is_string()) continue;
      if (!same_evm_address(log[receipt_log_field::address].as_string(), inbound_address)) continue;
      if (!log.contains(receipt_log_field::topics) || !log[receipt_log_field::topics].is_array()) continue;
      const auto& topics = log[receipt_log_field::topics].get_array();
      if (topics.empty() || !topics.front().is_string()) continue;
      const auto topic = normalized_word(topics.front());
      const auto data  = log.contains(receipt_log_field::data) ? normalized_word(log[receipt_log_field::data])
                                                               : std::string{};
      // The epoch of an event that indexes it; `EpochComplete` carries its
      // epoch in the data instead.
      const auto indexed_epoch = [&]() -> std::optional<uint64_t> {
         if (topics.size() <= FIRST_INDEXED_TOPIC) return std::nullopt;
         return word_to_uint(normalized_word(topics[FIRST_INDEXED_TOPIC]));
      };

      if (same_hex(topic, delivery_topic)) {
         if (topics.size() <= SECOND_INDEXED_TOPIC) continue;
         const auto operator_word = address_from_word(normalized_word(topics[SECOND_INDEXED_TOPIC]));
         if (indexed_epoch() == epoch_index && operator_word && same_evm_address(*operator_word, operator_address)) {
            summary.recorded = true;
         }
      } else if (same_hex(topic, consensus_topic)) {
         if (indexed_epoch() == epoch_index) summary.tipped = true;
      } else if (same_hex(topic, progressed_topic)) {
         const auto dispatched = data_word(data, 0).and_then(word_to_uint);
         if (indexed_epoch() == epoch_index && dispatched && *dispatched <= std::numeric_limits<uint16_t>::max()) {
            summary.dispatched = static_cast<uint16_t>(*dispatched);
         }
      } else if (same_hex(topic, complete_topic)) {
         if (data_word(data, 0).and_then(word_to_uint) == epoch_index) summary.finalized = true;
      }
   }
   return summary;
}

std::string envelope_digest_word(const std::vector<char>& envelope_bytes) {
   const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(envelope_bytes.data()),
                                        envelope_bytes.size()};
   return normalized_hex(fc::crypto::keccak256::hash(bytes).str());
}

std::optional<realize_yield_refusal> classify_realize_yield_revert(std::string_view revert_data) {
   const auto data = strip_hex_prefix(revert_data);
   if (data.size() < EVM_SELECTOR_HEX_CHARS) return std::nullopt;
   const auto selector       = data.substr(0, EVM_SELECTOR_HEX_CHARS);
   const auto argument_chars = data.size() - EVM_SELECTOR_HEX_CHARS;
   // Exact shape, as for the `epochIn` refusals: an error with other arguments hashes
   // differently, and a payload that does not fit the error is not that error.
   const auto is = [&](const char* signature, size_t words) {
      return argument_chars == words * EVM_ABI_WORD_HEX_CHARS && same_hex(selector, selector_hex(signature));
   };
   if (is(no_yield_signature, 0)) return realize_yield_refusal::no_yield;
   if (is(yield_below_deadband_signature, 2)) return realize_yield_refusal::below_deadband;
   if (is(pool_underbacked_signature, 2)) return realize_yield_refusal::underbacked;
   if (is(enforced_pause_signature, 0)) return realize_yield_refusal::paused;
   return std::nullopt;
}

std::optional<uint32_t> count_envelope_attestations(const std::vector<char>& envelope_bytes) {
   sysio::opp::Envelope envelope;
   if (!envelope.ParseFromArray(envelope_bytes.data(), static_cast<int>(envelope_bytes.size()))) {
      return std::nullopt;
   }
   uint64_t count = 0;
   for (const auto& message : envelope.messages()) count += message.payload().attestations_size();
   if (count > std::numeric_limits<uint32_t>::max()) return std::nullopt;
   return static_cast<uint32_t>(count);
}

std::optional<std::string> address_from_word(std::string_view raw_hex) {
   constexpr size_t address_chars = EVM_ADDRESS_BYTES * HEX_CHARS_PER_BYTE;

   const auto word = strip_hex_prefix(raw_hex);
   if (!is_hex_word(word)) return std::nullopt;
   // An address word is left-padded with zeros; anything else in the pad is not an address.
   if (word.find_first_not_of('0') < EVM_ABI_WORD_HEX_CHARS - address_chars) return std::nullopt;
   return "0x" + std::string(word.substr(EVM_ABI_WORD_HEX_CHARS - address_chars));
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

      // A delivery is funded to what it has to carry, up to the policy's
      // ceiling. A ceiling under what the largest envelope the platform
      // allows needs is a configuration that cannot deliver, and it is
      // refused here rather than discovered one stuck epoch at a time. The
      // total-cost term is a warning, not a refusal: it binds at the fee a
      // call is sent at, and only the policy's own maximum is known here; an
      // unbounded total cost (the maximum policy) has nothing to warn about.
      const auto&    policy  = _entry->client->transaction_policy();
      const uint64_t ceiling = delivery_gas_ceiling(policy);
      SYS_ASSERT(ceiling >= DELIVERY_MINIMUM_GAS_CEILING, chain::plugin_config_exception,
                 "outpost_ethereum_client[{}]: transaction policy max_gas_limit {} funds at most {} gas per "
                 "epochIn, below the {} a full-cap envelope needs to deliver, dispatch one attestation and "
                 "emit; raise max_gas_limit for client {}",
                 to_string(), policy.max_gas_limit.str(), ceiling, DELIVERY_MINIMUM_GAS_CEILING,
                 policy.client_id);
      const uint64_t affordable_at_max_fee = delivery_gas_ceiling(policy, policy.max_fee_per_gas);
      const bool     total_cost_bounded =
         policy.max_total_native_cost != eth::maximum_ethereum_transaction_policy_value();
      if (total_cost_bounded && affordable_at_max_fee < DELIVERY_MINIMUM_GAS_CEILING) {
         wlog("outpost_ethereum_client[{}]: transaction policy max_total_native_cost {} wei funds only {} gas "
              "per epochIn at max_fee_per_gas {} wei, below the {} a full-cap envelope needs; deliveries at "
              "fees near the policy maximum will spill into more continuations than the budget sizes for",
              to_string(), policy.max_total_native_cost.str(), affordable_at_max_fee, policy.max_fee_per_gas.str(),
              DELIVERY_MINIMUM_GAS_CEILING);
      }
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

detail::delivery_progress outpost_ethereum_client::read_progress(uint32_t epoch_index, const fc::uint256& block) {
   const auto at = pinned(block);
   // A backend that has not seen `block` yet answers with a JSON-RPC error
   // rather than stale state; that is the one failure worth waiting out.
   // Everything else (a malformed response, the deadline) propagates.
   return fc::task::retry_until<detail::delivery_progress>(
      OP_READ_PROGRESS, PINNED_READ_RETRY, [&]() -> std::optional<detail::delivery_progress> {
         try {
            detail::delivery_progress progress;
            progress.next_epoch_index = read_next_epoch_index(*_opp_inbound_client, at, to_string());
            progress.spill            = read_dispatch_spill(*_opp_inbound_client, epoch_index, at, to_string());
            progress.own_digest = read_own_digest(*_opp_inbound_client, epoch_index, _signer_address_hex, at, to_string());
            progress.settled_digest = read_settled_digest(*_opp_inbound_client, at, to_string());
            dlog("outpost_ethereum_client[{}]: epoch={} at block {}: next_epoch={} spill(tipped={} dispatched={} "
                 "complete={} finalized={}) own={} settled={}",
                 to_string(), epoch_index, block.str(), progress.next_epoch_index, progress.spill.tipped,
                 progress.spill.dispatched, progress.spill.complete, progress.spill.finalized, progress.own_digest,
                 progress.settled_digest);
            return progress;
         } catch (const fc::network::json_rpc::json_rpc_error& refusal) {
            dlog("outpost_ethereum_client[{}]: progress read at block {} refused ({}); retrying", to_string(),
                 block.str(), refusal.to_string());
            return std::nullopt;
         }
      });
}

detail::pending_consensus outpost_ethereum_client::read_pending_consensus(const std::string& digest_word,
                                                                         const fc::uint256& block) {
   std::string digest_arg = "0x" + digest_word;
   const auto  raw = view_hex(_opp_inbound_client->pending_consensus_for_digest(pinned(block), digest_arg), to_string(),
                              opp_inbound_abi_name::pending_consensus_for_digest);
   const auto decoded = eth::contract_decode_data(
      _opp_inbound_client->get_abi(opp_inbound_abi_name::pending_consensus_for_digest), raw);
   FC_ASSERT(decoded.is_object(),
             "outpost_ethereum_client[{}]: pendingConsensusForDigest result was not a variant object", to_string());
   const auto& object = decoded.get_object();
   const auto  read   = [&](const char* key) -> uint64_t {
      FC_ASSERT(object.contains(key), "outpost_ethereum_client[{}]: pendingConsensusForDigest result missing '{}'",
                to_string(), key);
      const auto value = abi_uint_output(object[key]);
      FC_ASSERT(value.has_value(),
                "outpost_ethereum_client[{}]: pendingConsensusForDigest '{}' was not a numeric output", to_string(),
                key);
      return *value;
   };
   detail::pending_consensus consensus;
   consensus.next_epoch         = static_cast<uint32_t>(read(opp_inbound_abi::field::next_epoch));
   consensus.agreeing           = static_cast<uint32_t>(read(opp_inbound_abi::field::agreeing));
   consensus.group_size         = static_cast<uint32_t>(read(opp_inbound_abi::field::group_size));
   consensus.epoch_started_at   = read(opp_inbound_abi::field::current_epoch_started_at);
   consensus.epoch_duration_sec = static_cast<uint32_t>(read(opp_inbound_abi::field::epoch_duration_sec));
   return consensus;
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
   const std::string envelope_hex  = fc::to_hex(envelope_bytes.data(), static_cast<uint32_t>(total));
   // The digest the outpost records for this delivery and settles consensus
   // on: a continuation is only valid with the bytes that hash to the settled
   // digest, which the relay checks before paying to find out.
   const std::string local_digest = detail::envelope_digest_word(envelope_bytes);

   // Each call is funded to what it has left to carry (`delivery_gas_budget`),
   // never to a fixed figure: the attestations the outpost's cursor says are
   // still to dispatch, doubled for every call this tick that fell short. An
   // envelope the relay cannot read is funded to the ceiling. The ceiling is
   // what the policy allows AT THE CURRENT FEE, so a budget it admits is a
   // budget the policy's total-cost term will not refuse at signing.
   const auto     attestation_count = detail::count_envelope_attestations(envelope_bytes);
   const auto     gas_config        = _entry->client->get_gas_config();
   const uint64_t gas_ceiling =
      delivery_gas_ceiling(_entry->client->transaction_policy(), gas_config.max_fee_per_gas);
   if (!attestation_count) {
      wlog("outpost_ethereum_client[{}]: epoch={} envelope ({} bytes) does not decode as an OPP "
           "envelope; funding delivery to the ceiling ({} gas)",
           to_string(), epoch_index, total, gas_ceiling);
   }

   // Every decision is taken from the outpost's state at ONE block: the head
   // when the tick starts, then the block each confirmed call landed in. A
   // read at `latest` could be served by a backend behind the block that just
   // confirmed and report the state from before the call.
   fc::uint256 pinned_block = _entry->client->get_block_number();

   // A call that cannot be sent at its budget, or that was sent and moved
   // nothing, is reported as a stalled epoch: once at error level for each
   // place the cursor stalls, then at debug until the cursor moves. The tick
   // ends in `outpost_delivery_incomplete_exception` so the job retries it.
   const auto report_stall = [&](const detail::dispatch_spill& spill, const std::string& reason) {
      const std::pair<uint32_t, uint16_t> where{epoch_index, spill.dispatched};
      if (_reported_stall != where) {
         _reported_stall = where;
         elog("outpost_ethereum_client[{}]: epoch={} cannot advance past attestation {} at the ceiling ({} gas): "
              "{}; the next tick retries, and the epoch stays open until the outpost accepts the call",
              to_string(), epoch_index, spill.dispatched, gas_ceiling, reason);
      } else {
         dlog("outpost_ethereum_client[{}]: epoch={} still stalled at attestation {}: {}", to_string(), epoch_index,
              spill.dispatched, reason);
      }
      SYS_THROW(chain::outpost_delivery_incomplete_exception,
                "outpost_ethereum_client[{}]: epoch={} stalled at attestation {} at the ceiling ({} gas): {}",
                to_string(), epoch_index, spill.dispatched, gas_ceiling, reason);
   };

   // Sequential, receipt-confirmed submission — one transaction in flight at a
   // time, so the signer's nonce advances in lock-step and a mid-sequence
   // failure simply abandons the tick. The next cron tick re-reads the
   // outpost's cursor and picks up wherever the chain actually is.
   std::string                              last_tx;
   uint32_t                                 sends          = 0;
   uint32_t                                 escalations    = 0;
   uint32_t                                 stale_refusals = 0;
   uint64_t                                 last_budget    = 0;
   std::optional<detail::delivery_progress> before_last_send;
   for (;;) {
      throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);

      const auto progress = read_progress(epoch_index, pinned_block);
      const auto settlement =
         detail::classify_settlement(progress.own_digest, progress.settled_digest, progress.spill.tipped);

      // A recorded, untipped delivery is re-sent only when the outpost's own
      // numbers say the re-send would tip it — and only when this tick has
      // not just sent one, whose tip check ran in that very transaction.
      bool                                    majority_reachable = false;
      std::optional<detail::pending_consensus> consensus;
      if (sends == 0 && settlement == detail::delivery_settlement::recorded &&
          progress.next_epoch_index == epoch_index) {
         consensus = read_pending_consensus(progress.own_digest, pinned_block);
         majority_reachable =
            detail::majority_tip_reachable(*consensus, uint64_t{fc::time_point::now().sec_since_epoch()});
      }
      const auto action = detail::decide_delivery(progress.next_epoch_index, epoch_index, progress.spill,
                                                  settlement, majority_reachable);
      dlog("outpost_ethereum_client[{}]: epoch={} action={} next_epoch={} "
           "spill(tipped={} dispatched={} complete={} finalized={}) settlement={} sends={}",
           to_string(), epoch_index, magic_enum::enum_name(action), progress.next_epoch_index,
           progress.spill.tipped, progress.spill.dispatched, progress.spill.complete, progress.spill.finalized,
           magic_enum::enum_name(settlement), sends);

      switch (action) {
      case detail::delivery_action::already_finalized:
         if (last_tx.empty()) {
            // Nothing sent: the outpost was already past this epoch when the
            // tick began. An EMPTY tx id tells the job the epoch is handled.
            ilog("outpost_ethereum_client[{}]: skipping epoch={} delivery — the outpost has "
                 "already finalized it (nextEpochIndex={})",
                 to_string(), epoch_index, progress.next_epoch_index);
         }
         return last_tx;

      case detail::delivery_action::outpost_behind:
         // Not a handled epoch: the outpost will accept this delivery once it
         // finalizes the earlier one, and the next tick must try again.
         ilog("outpost_ethereum_client[{}]: epoch={} not delivered — the outpost is still on epoch {}; "
              "retrying next tick",
              to_string(), epoch_index, progress.next_epoch_index);
         SYS_THROW(chain::outpost_delivery_incomplete_exception,
                   "outpost_ethereum_client[{}]: epoch={} not delivered — the outpost is still on epoch {}",
                   to_string(), epoch_index, progress.next_epoch_index);

      case detail::delivery_action::wait_for_deliverer:
         if (settlement == detail::delivery_settlement::divergent) {
            // This relay's envelope disagreed with the majority's: worth a
            // warning with both digests, since it means the depot handed this
            // relay different bytes than its peers.
            wlog("outpost_ethereum_client[{}]: epoch={} tipped on digest {} but this relay delivered {}; its "
                 "deliverers carry the continuation (dispatched={} complete={})",
                 to_string(), epoch_index, progress.settled_digest, progress.own_digest, progress.spill.dispatched,
                 progress.spill.complete);
         } else {
            ilog("outpost_ethereum_client[{}]: epoch={} tipped on a digest this relay did not deliver "
                 "(dispatched={} complete={}); its deliverers carry the continuation",
                 to_string(), epoch_index, progress.spill.dispatched, progress.spill.complete);
         }
         return last_tx;

      case detail::delivery_action::await_peers:
         if (sends == 0) {
            // A consensus retry that arrives before the OUTPOST's boundary (the
            // job gates it on the depot's, which leads the outpost's) is not a
            // used-up retry: the epoch stays open so the job tries again next
            // tick, until the boundary passes and the majority view decides.
            const uint64_t boundary = consensus->epoch_started_at + consensus->epoch_duration_sec;
            if (uint64_t{fc::time_point::now().sec_since_epoch()} < boundary) {
               ilog("outpost_ethereum_client[{}]: epoch={} delivery recorded; the outpost's boundary ({}) has not "
                    "passed, so a re-delivery could not tip it yet; retrying next tick",
                    to_string(), epoch_index, boundary);
               SYS_THROW(chain::outpost_delivery_incomplete_exception,
                         "outpost_ethereum_client[{}]: epoch={} recorded; outpost boundary {} not reached",
                         to_string(), epoch_index, boundary);
            }
            ilog("outpost_ethereum_client[{}]: epoch={} delivery already recorded; consensus not tipped "
                 "(agreeing={} group={} boundary={}); nothing to send until more of the group delivers",
                 to_string(), epoch_index, consensus->agreeing, consensus->group_size, boundary);
         } else {
            dlog("outpost_ethereum_client[{}]: epoch={} delivery recorded; consensus not yet tipped", to_string(),
                 epoch_index);
         }
         return last_tx;

      case detail::delivery_action::retry_consensus:
         ilog("outpost_ethereum_client[{}]: epoch={} re-delivering to run the boundary tip (agreeing={} "
              "group={} boundary={})",
              to_string(), epoch_index, consensus->agreeing, consensus->group_size,
              consensus->epoch_started_at + consensus->epoch_duration_sec);
         break;

      case detail::delivery_action::continue_dispatch:
         // The contract resumes only from the bytes that hash to the settled
         // digest. An envelope that does not is not this epoch's — nothing
         // to continue with, and no transaction to pay for finding that out.
         if (!same_hex(local_digest, progress.settled_digest)) {
            wlog("outpost_ethereum_client[{}]: epoch={} is tipped on digest {} but the envelope held hashes to "
                 "{}; this relay cannot continue it",
                 to_string(), epoch_index, progress.settled_digest, local_digest);
            return last_tx;
         }
         break;

      case detail::delivery_action::deliver:
         break;
      }

      // The call this tick last paid for must have moved the outpost's cursor;
      // one that did not was under-funded for the attestation at the cursor
      // (the contract records a spill only when it dispatched at least one),
      // and the next is funded higher. At the ceiling there is nothing higher.
      if (before_last_send) {
         if (!detail::advanced(*before_last_send, progress)) {
            if (last_budget >= gas_ceiling) {
               report_stall(progress.spill, "the last call advanced nothing");
            }
            ++escalations;
         } else if (action == detail::delivery_action::continue_dispatch) {
            // A continuation behind a call this tick already sent means that
            // call spilled: what it was funded with did not carry its work.
            ++escalations;
         }
      }
      if (sends >= MAX_CONTINUATIONS_PER_TICK) {
         wlog("outpost_ethereum_client[{}]: epoch={} still open after {} calls in one tick (dispatched={} "
              "complete={}); the next tick resumes from the outpost's cursor",
              to_string(), epoch_index, sends, progress.spill.dispatched, progress.spill.complete);
         SYS_THROW(chain::outpost_delivery_incomplete_exception,
                   "outpost_ethereum_client[{}]: epoch={} still open after {} calls in one tick", to_string(),
                   epoch_index, sends);
      }

      throw_if_past_deadline(deadline_abs, OP_DELIVER_OUTBOUND);
      // Before the tip nothing is dispatched, so every attestation is still
      // to carry; after it the outpost's cursor says how many remain, and a
      // complete dispatch leaves only the emit.
      std::optional<uint32_t> remaining;
      if (attestation_count) {
         const uint32_t done = progress.spill.tipped ? progress.spill.dispatched : 0;
         remaining = progress.spill.complete ? 0 : (*attestation_count > done ? *attestation_count - done : 0);
      }
      uint64_t gas_budget = delivery_gas_budget(delivery_gas_request{
         .envelope_bytes         = total,
         .remaining_attestations = remaining,
         .escalations            = escalations,
         .ceiling                = gas_ceiling,
      });
      dlog("outpost_ethereum_client[{}]: epoch={} funding epochIn with {} gas (remaining={} "
           "escalations={} ceiling={})",
           to_string(), epoch_index, gas_budget,
           remaining ? std::to_string(*remaining) : "unknown", escalations, gas_ceiling);

      // The ABI arguments are bound as non-const lvalue references, so each
      // one needs a named local.
      uint32_t         epoch_arg   = epoch_index;
      std::string      payload_hex = envelope_hex;
      epoch_in_receipt receipt;
      try {
         receipt = _opp_inbound_client->epoch_in(epoch_arg, payload_hex, gas_budget);
      } catch (const fc::network::json_rpc::json_rpc_error& refusal) {
         // The node refused the call before anything was signed: the
         // pre-flight estimate runs under the budget. Which refusal decides
         // what the relay does about it.
         const auto revert = detail::classify_epoch_in_revert(refusal.code, revert_hex(refusal));
         if (!revert) throw;
         switch (*revert) {
         case detail::epoch_in_revert::dispatch_underfunded:
         case detail::epoch_in_revert::handler_gas_exhausted:
            // The budget's fault below the ceiling: retry once with everything
            // the policy allows. At the ceiling it is the envelope's — an
            // attestation no funding carries — and the tick reports it rather
            // than spend the ceiling on finding out again.
            if (gas_budget >= gas_ceiling) report_stall(progress.spill, refusal.to_string());
            wlog("outpost_ethereum_client[{}]: epoch={} refused at {} gas ({}); retrying at the "
                 "ceiling ({} gas)",
                 to_string(), epoch_index, gas_budget, magic_enum::enum_name(*revert), gas_ceiling);
            gas_budget = gas_ceiling;
            receipt    = _opp_inbound_client->epoch_in(epoch_arg, payload_hex, gas_budget);
            break;

         case detail::epoch_in_revert::non_sequential_epoch:
         case detail::epoch_in_revert::operator_already_delivered:
         case detail::epoch_in_revert::not_active_operator:
         case detail::epoch_in_revert::digest_mismatch:
            // The outpost's state is not what the pinned read said — another
            // relay's call landed since — or this relay has no standing for
            // the call. Read again at the head and decide from that rather
            // than guess; a refusal that survives fresh reads is reported.
            if (++stale_refusals > MAX_STALE_REFUSALS_PER_TICK) throw;
            ilog("outpost_ethereum_client[{}]: epoch={} refused ({}); re-reading the outpost at the head",
                 to_string(), epoch_index, magic_enum::enum_name(*revert));
            pinned_block = _entry->client->get_block_number();
            before_last_send.reset();
            continue;
         }
      }

      ++sends;
      last_tx          = receipt.tx_hash;
      last_budget      = gas_budget;
      pinned_block     = receipt.block_number;
      before_last_send = progress;

      const auto summary =
         detail::summarize_delivery_receipt(receipt.logs, _opp_inbound_addr, epoch_index, _signer_address_hex);
      ilog("outpost_ethereum_client[{}]: epochIn {} epoch={} bytes={} gas={} block={} tx={} — recorded={} "
           "tipped={} dispatched={} finalized={}",
           to_string(), action == detail::delivery_action::continue_dispatch ? "continued" : "delivered",
           epoch_index, total, gas_budget, receipt.block_number.str(), receipt.tx_hash, summary.recorded,
           summary.tipped, summary.dispatched ? std::to_string(*summary.dispatched) : "-", summary.finalized);
   }
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
   // The routing table is configuration, not delivered content, so `latest` is right: it is
   // not state this relay is mid-way through changing, and a stale read costs one idle crank.
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
            ? detail::classify_realize_yield_revert(revert_hex(e))
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
