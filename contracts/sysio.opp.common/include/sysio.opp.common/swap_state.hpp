#pragma once
/**
 * @file swap_state.hpp
 * @brief Shared AMM state for sysio.swap and read-only contract consumers.
 *
 * LAYOUT IS SHARED STATE: field order and table keys are the serialized contract
 * between the AMM and readers. Pool1/pool2 are the live balances at action time;
 * reading their ratio needs no sync and does not settle unclaimed shadow yield.
 */
#include <sysio/asset.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/time.hpp>
#include <sysio.opp.common/twap.hpp>
#include <optional>

namespace sysio::opp::swap_state {

/// The AMM account owning these tables.
inline constexpr name account = "sysio.swap"_n;
/// Current pair balances and LP supply.
inline constexpr name stat_table = "stat"_n;
/// Cumulative prices, for readers that need a TWAP.
inline constexpr name price_table = "priceaccum"_n;

/// A pair's rows in `stat` and `priceaccum`: keyed by the LP token's symbol code.
struct pair_key {
   uint64_t symbol_code;
   SYSLIB_SERIALIZE(pair_key, (symbol_code))
};

/// Complete on-chain pair layout, unchanged by extraction.
struct currency_stats {
   asset          supply;
   asset          max_supply;
   name           issuer;
   extended_asset pool1;
   extended_asset pool2;
   int            fee;
   name           fee_authority;   ///< whose signature changefee requires for this pair
   asset          locked_shares;   ///< part of `supply` held by no account, never redeemable
   std::optional<extended_symbol> yield_leg;   ///< the shadow leg of a yield pool; empty for a plain pool
   uint32_t       conversion_horizon_sec = 0;  ///< H: the reservoir is meant to sell over this long
   uint32_t       depth_cap_bps          = 0;  ///< hard ceiling on one clip, bps of the pool's shadow side
   int64_t        clip_floor             = 0;  ///< least a clip may be, in units of the shadow
   int64_t        last_tick_depth        = 0;  ///< the shadow side as of the last setyield or selling tick.
                                               ///< The depth cap is taken against the SMALLER of this and
                                               ///< the current side, so a shadow side inflated inside one
                                               ///< transaction cannot widen the cap that bounds it.
   time_point     last_tick{};                 ///< elapsed-time base of the clip formula: the last tick that
                                               ///< sold, the last setyield, or when the reservoir last
                                               ///< went from empty to funded, whichever is latest. A tick
                                               ///< that sells nothing deliberately leaves it alone.
   SYSLIB_SERIALIZE(currency_stats, (supply)(max_supply)(issuer)(pool1)(pool2)(fee)(fee_authority)
                                    (locked_shares)(yield_leg)(conversion_horizon_sec)(depth_cap_bps)
                                    (clip_floor)(last_tick_depth)(last_tick))
};

/// Cumulative-price accumulators for a pair (sysio.opp.common/twap.hpp).
/// `price1` sums the Q64.64 price of one unit of pool1 in units of pool2,
/// times elapsed microseconds; `price2` the reverse. Both advance, at the
/// spot price that held since `last_update`, immediately before the pools
/// change and on `sync`. A reader snapshots the row at t0 and computes
/// `twap::average_price(twap::difference(now, snapshot), t - t0)`.
struct price_accumulator {
   sysio::opp::twap::cumulative_price price1;
   sysio::opp::twap::cumulative_price price2;
   time_point                         last_update;
   SYSLIB_SERIALIZE(price_accumulator, (price1)(price2)(last_update))
};

/// Public read-only stat view; ownership is declared only by sysio.swap.
using stats = kv::table<stat_table, pair_key, currency_stats>;
/// Public read-only accumulator view.
using priceaccums = kv::table<price_table, pair_key, price_accumulator>;

} // namespace sysio::opp::swap_state
