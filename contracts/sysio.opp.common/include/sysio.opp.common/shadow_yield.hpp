#pragma once
/**
 * @file shadow_yield.hpp
 * @brief The shadow token's yield-distribution state, as its holders read it.
 *
 * A shadow token (shadow-liqETH, shadow-liqSOL) pays WIRE yield to its holders
 * through one cumulative index per symbol: WIRE earned per shadow unit, scaled
 * by YIELD_INDEX_SCALE. Every holder row carries the index value at its last
 * settle and the WIRE banked so far; what a holder is owed at any moment is
 *
 *     owed_wire + balance * (index - index_checkpoint) / YIELD_INDEX_SCALE
 *
 * with the product taken in 128 bits and the division floored, exactly as the
 * token's own settle computes it. A contract that holds shadow (sysio.swap for
 * its pools) can therefore compute its payout from public state BEFORE calling
 * `claim`, and assert the exact amount when the transfer lands, instead of
 * inferring it from a balance change.
 *
 * The index and the checkpoints are 128-bit: an add moves the index by
 * yield * YIELD_INDEX_SCALE / supply, which passes 2^64 on one large add to a
 * thinly held symbol. The ABI carries the fields as the builtin `uint128`.
 *
 * This header is the contract between the token and its holders: the table
 * names, the row layouts, the scale, and the two typed actions a holder calls.
 * Both sides compile against it.
 */

#include <sysio/asset.hpp>
#include <sysio/check.hpp>
#include <sysio/name.hpp>
#include <sysio/symbol.hpp>

namespace sysio::opp::shadow {

/// The index's width. `uint128_t` is the CDT builtin the ABI generator emits as `uint128`.
using u128 = uint128_t;

/// Fixed-point scale of the cumulative index (WIRE per shadow unit).
inline constexpr uint64_t YIELD_INDEX_SCALE = 1'000'000'000'000;

/// The holder balance table: scope = holder, key = symbol code. sysio.token's
/// layout plus the two accrual fields.
inline constexpr name ACCOUNTS_TABLE = "accounts"_n;
/// The per-symbol index: scope = the token contract, key = symbol code.
inline constexpr name YIELD_INDEX_TABLE = "yieldidx"_n;

/// `claim(name holder, symbol_code sym)`: settle `holder`'s row for `sym`, pay it
/// what `owed` says from the token's own WIRE, and zero the row. Holder's authority.
inline constexpr name CLAIM_ACTION = "claim"_n;
/// `addyield(name from, asset quantity, symbol_code target)`: move `quantity` WIRE
/// from `from` into `target`'s pot by inline transfer under `from`'s own authority
/// (a token contract that is not privileged therefore needs `sysio.code` on
/// `from`'s active), and advance the index by quantity / supply, carrying the
/// remainder.
inline constexpr name ADDYIELD_ACTION = "addyield"_n;

/// Key of an `accounts` or `yieldidx` row.
struct symbol_key {
   uint64_t symbol_code;
   SYSLIB_SERIALIZE(symbol_key, (symbol_code))
};

/// A holder's row for one shadow symbol.
struct account {
   asset     balance;
   uint128_t index_checkpoint = 0;   ///< index value at the last settle of this row
   uint64_t  owed_wire        = 0;   ///< WIRE banked by earlier settles, not yet claimed
   SYSLIB_SERIALIZE(account, (balance)(index_checkpoint)(owed_wire))
};

/// One shadow symbol's distribution state.
struct yield_index {
   uint128_t index = 0;   ///< cumulative WIRE per shadow unit, scaled by YIELD_INDEX_SCALE
   uint64_t  pot   = 0;   ///< WIRE held for holders and not yet claimed
   uint64_t  carry = 0;   ///< WIRE received but below one index unit, carried to the next add
   SYSLIB_SERIALIZE(yield_index, (index)(pot)(carry))
};

/// The WIRE a holder row is owed now, at index `index`: the banked amount plus
/// the accrual since its checkpoint, floored. Pure, so the token's settle and a
/// holder's pre-claim computation are one function. The index only grows and a
/// row is never owed more than its pot holds, so either bound failing is corrupt state.
inline uint64_t owed(const account& row, u128 index) {
   check( index >= row.index_checkpoint, "index precedes the row checkpoint" );
   if (row.balance.amount <= 0) return row.owed_wire;
   const u128 accrued = static_cast<u128>(row.balance.amount) * (index - row.index_checkpoint) / YIELD_INDEX_SCALE;
   const u128 total   = static_cast<u128>(row.owed_wire) + accrued;
   check( total <= static_cast<u128>(asset::max_amount), "owed yield exceeds the asset range" );
   return static_cast<uint64_t>(total);
}

} // namespace sysio::opp::shadow
