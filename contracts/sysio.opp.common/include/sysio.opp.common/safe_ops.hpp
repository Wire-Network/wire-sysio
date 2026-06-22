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
#include <string_view>

namespace sysio::opp::safe {

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

} // namespace sysio::opp::safe
