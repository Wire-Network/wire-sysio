#pragma once
/**
 * @file safe_ops.hpp
 * @brief Non-throwing primitives for the OPP inbound-consensus dispatch path.
 *
 * Every OPP contract handler reachable from `sysio.msgch::deliver -> evalcons ->
 * dispatch` (createuwreq, rcrdcommit, onreward, the swap-settlement tail, ...)
 * MUST NOT abort on operator-supplied data: a `check()`/abort there rolls back
 * the consensus-tipping delivery and stalls epoch advancement chain-wide
 * (`feedback_opp_handlers_never_throw.md`). The two recurring foot-guns are
 *
 *   1. constructing a `name` from a cross-chain account string — CDT's `name`
 *      constructor `check(false, ...)`-aborts on an invalid string, and
 *   2. unchecked `uint64_t +=` on operator-relayed external-chain amounts —
 *      a wrap corrupts `>=` sufficiency guards and can let a later `asset()` /
 *      `subtract_balance` abort slip through into settlement.
 *
 * The helpers below were previously copied into individual contracts
 * (`sysio.dclaim`'s `is_valid_name_string`, `sysio.reserv`'s `add_capped_u64`).
 * They live here so every OPP contract validates names and saturates the same
 * way — one place to change, one place to audit.
 */

#include <cstdint>
#include <optional>
#include <string_view>

namespace sysio::opp::safe {

/// The Antelope `asset` magnitude limit, `2^62 - 1`. Equal to
/// `sysio::asset::max_amount` in both the CDT (contract/WASM) and chain (host)
/// `asset` types, but defined locally so this header stays dependency-free: it
/// is included by WASM contracts AND by the native-tester host build
/// (`safe_ops_tests.cpp`), where `<sysio/asset.hpp>` is not on the include path.
/// `safe_ops_tests.cpp` static_asserts this against the real `asset::max_amount`
/// so the two can never silently drift.
inline constexpr int64_t depot_amount_max = (int64_t{1} << 62) - 1;

/// Fail-closed parse of an inbound OPP `TokenAmount.amount` into a non-negative
/// depot amount.
///
/// `TokenAmount.amount` is signed on the wire (`int64`) and in the contract
/// binding (`vint64_t`). The WSA-028 foot-gun was
/// `static_cast<uint64_t>(static_cast<int64_t>(amount))`: a negative value such
/// as `-1` wraps to `18446744073709551615`, an impossible "balance" that sails
/// through every zero-only guard and inflates collateral, reserve, reward, and
/// settlement accounting.
///
/// This gate returns the amount as a `uint64_t` iff it is strictly positive AND
/// within the asset magnitude range (`<= depot_amount_max`, i.e. `2^62 - 1`);
/// otherwise it returns `std::nullopt`. The upper bound guarantees the validated
/// value can later be carried by a WIRE-frame `asset` (e.g. `sysio.dclaim`)
/// without tripping `asset()`'s range `check()`, so the never-throw dispatch
/// contract is preserved end to end.
///
/// Callers MUST treat `std::nullopt` as "drop / refund-revert this inbound
/// message" — never as a reason to `check()`-abort inside the consensus
/// dispatch chain, which would stall epoch advancement chain-wide
/// (`feedback_opp_handlers_never_throw.md`).
///
/// @param amount the decoded `TokenAmount.amount` (cast from its `vint64_t`).
/// @return the amount as a `uint64_t` in `(0, depot_amount_max]`, or
///         `std::nullopt` when it is non-positive or out of asset range.
inline std::optional<uint64_t> to_depot_amount(int64_t amount) {
   if (amount <= 0)               return std::nullopt;
   if (amount > depot_amount_max) return std::nullopt;
   return static_cast<uint64_t>(amount);
}

/// Non-throwing validation of a string destined for `name(std::string_view)`.
///
/// CDT's `name` constructor `check(false, ...)`-aborts on a string longer than
/// 13 characters, one containing a character outside ".12345abcdefghijklmnopqrstuvwxyz",
/// or one whose 13th character exceeds the 4-bit final symbol (value > 15). A
/// cross-chain-supplied account string must therefore be validated here and
/// soft-skipped, never fed blindly into `name()` from inside the dispatch
/// chain. Mirrors CDT `basic_name`'s `char_to_value` + length rules exactly so
/// the *full* CDT name domain is accepted — in particular a legitimate 13-byte
/// name (final symbol in `.`/`1`-`5`/`a`-`j`) passes, where a naive
/// `size() > 12` length cap would wrongly reject it.
///
/// @param s the candidate account string (no leading/trailing trimming).
/// @return true iff `name(s)` would construct without aborting.
inline bool is_valid_name_string(std::string_view s) {
   if (s.size() > 13) return false;
   for (std::size_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      uint8_t v;
      if      (c == '.')               v = 0;
      else if (c >= '1' && c <= '5')   v = static_cast<uint8_t>(c - '1' + 1);
      else if (c >= 'a' && c <= 'z')   v = static_cast<uint8_t>(c - 'a' + 6);
      else return false;                       // character outside the name alphabet
      if (i == 12 && v > 15) return false;     // 13th character encodes only 4 bits
   }
   return true;
}

/// Saturating unsigned 64-bit addition. Returns `a + b`, clamped to
/// `UINT64_MAX` on overflow instead of wrapping.
///
/// OPP amount accumulators (reserve balances, rewards-bucket counters) and
/// settlement pre-checks add operator-relayed external-chain amounts that have
/// no on-chain supply cap. A raw `+` could wrap past `UINT64_MAX` and corrupt
/// the weighted-AMM curve or a `>=` sufficiency guard — letting a downstream
/// `asset()` / `subtract_balance` abort reach settlement and stall consensus.
/// Saturating keeps the guards monotonic and never throws; the cap is
/// unreachable for any real token amount.
inline uint64_t add_sat_u64(uint64_t a, uint64_t b) {
   return (b > UINT64_MAX - a) ? UINT64_MAX : a + b;
}

/// Saturating signed 64-bit addition. Returns `a + b`, clamped to `INT64_MAX`
/// on positive overflow and `INT64_MIN` on negative overflow, instead of
/// invoking signed-overflow undefined behaviour.
///
/// `asset::amount` is an `int64_t`, so an unchecked `amount += amount` on a
/// reslimit/policy accumulator is UB the instant the sum exceeds `INT64_MAX`.
/// Several of those accumulators sit on the OPP inbound-consensus dispatch path
/// (e.g. `sysio.roa::increase_reslimit`, reached from `nodeownreg` ->
/// `regnodeowner` and from `newnameduser`), where a `check()`-abort would roll
/// back the consensus-tipping delivery and stall epoch advancement. Saturating
/// keeps the arithmetic well-defined and never throws; the cap is unreachable
/// for any weight bounded by the SYS supply.
inline int64_t add_sat_i64(int64_t a, int64_t b) {
   if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
   if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
   return a + b;
}

} // namespace sysio::opp::safe
