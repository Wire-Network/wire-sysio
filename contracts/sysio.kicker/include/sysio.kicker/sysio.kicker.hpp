#pragma once
/**
 * @file sysio.kicker.hpp
 * @brief Governance-budgeted T5 gifts to LIQ holders, triggered by any keeper.
 *
 * Deploy privileged with roa::setsyscode. Budget is an earmark outside emissions;
 * no draw alters sysio.system::t5state.total_distributed. A configurable TWAP and
 * current supply value at most 72 hours of unpaid accrual per kick.
 */
#include <sysio/sysio.hpp>
#include <sysio/symbol.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/time.hpp>
#include <sysio.kicker/kicker_math.hpp>

namespace sysio {
/// Privileged treasury-to-shadow-yield contract; no donation-triggered payment path.
class [[sysio::contract("sysio.kicker")]] kicker : public contract {
public:
   using contract::contract;
   /// Governance authority and treasury account.
   static constexpr name system_account = "sysio"_n;
   /// Depot shadow token contract.
   static constexpr name liq_account = "sysio.liq"_n;
   /// Native WIRE token contract.
   static constexpr name token_account = "sysio.token"_n;

   /// Governance budget in WIRE subunits, payment spacing, minimum TWAP observation span and the spot guard.
   struct [[sysio::table("kickcfg")]] kick_config {
      uint64_t budget_remaining = 0; ///< Absolute remaining earmark; setconfig replaces it.
      /// Minimum unpaid accrual interval before an attempt.
      uint32_t min_interval_sec = kicker_math::default_min_interval_sec;
      /// Minimum observation span; the next kick uses this value even for an open observation.
      uint32_t twap_window_sec = kicker_math::default_twap_window_sec;
      /// Most spot may stray from the window TWAP, in basis points of the TWAP, for a kick to pay; 1..10000.
      uint16_t max_spot_deviation_bps = kicker_math::default_max_spot_deviation_bps;
      SYSLIB_SERIALIZE(kick_config, (budget_remaining)(min_interval_sec)(twap_window_sec)(max_spot_deviation_bps))
   };
   /// Symbol-code primary key for independently configured LIQ tokens.
   struct pool_key {
      uint64_t symbol_code;
      SYSLIB_SERIALIZE(pool_key, (symbol_code))
   };
   /// Per-token accrual and spend state; shortfalls are latest deficits, not duplicated debt.
   struct [[sysio::table("kickpools")]] kick_pool {
      symbol_code sym;
      uint16_t rate_bps = kicker_math::default_rate_bps;
      uint64_t min_gift = kicker_math::default_min_gift;
      time_point last_kick{};
      uint128_t gifted_total = 0; ///< Lifetime paid WIRE, outside emissions accounting.
      uint64_t shortfall_amount = 0; ///< Latest gift minus paid amount; cleared by full payment or zero supply.
      time_point shortfall_time{}; ///< Last unpaid shortfall, for governance visibility.
      bool gift_overflow = false; ///< Gift exceeded asset range; clock remains open.
      /// Spot strayed past max_spot_deviation_bps from the TWAP at the latest priced attempt; clock and observation held.
      bool spot_deviation = false;
      uint64_t max_gift_per_day = kicker_math::max_daily_gift; ///< Positive ceiling, at most 1,000 WIRE per UTC day.
      uint64_t day = 0; ///< UTC day number of the most recent payment.
      uint128_t spent_today = 0;
      opp::twap::cumulative_price price_snapshot; ///< Swap price1 extended to price_time.
      time_point price_time{}; ///< Observation starts on addpool or the last payment, full or partial.
      SYSLIB_SERIALIZE(kick_pool, (sym)(rate_bps)(min_gift)(last_kick)(gifted_total)
                                (shortfall_amount)(shortfall_time)(gift_overflow)(spot_deviation)
                                (max_gift_per_day)(day)(spent_today)(price_snapshot)(price_time))
   };
   /// Governance-owned singleton and per-symbol payment clocks.
   using config_table = kv::global<"kickcfg"_n, kick_config>;
   using pools_table = kv::table<"kickpools"_n, pool_key, kick_pool>;

   /// Replace budget, payment interval, TWAP window and spot guard. Requires sysio; never settles open intervals.
   /// The next kick applies the window to the existing price_time/snapshot without restarting observation.
   [[sysio::action]] void setconfig(kick_config cfg);
   /// Start accrual now for a registered LIQ/WIRE yield pair. Requires sysio; defaults to 200 bps and 1 WIRE.
   [[sysio::action]] void addpool(symbol_code sym, uint16_t rate_bps = kicker_math::default_rate_bps,
                                uint64_t min_gift = kicker_math::default_min_gift,
                                uint64_t max_gift_per_day = kicker_math::max_daily_gift);
   /// Change rate/minimum/daily ceiling without resetting spending or observation; prices bounded accrual. Requires sysio.
   [[sysio::action]] void setpool(symbol_code sym, uint16_t rate_bps, uint64_t min_gift, uint64_t max_gift_per_day);
   /// Stop accrual and erase the row; re-adding starts a new interval. Requires sysio.
   [[sysio::action]] void rmpool(symbol_code sym);
   /// Permissionless TWAP gift after the configured observation window; payment forfeits accrual older than 72 hours.
   /// Pays nothing while spot strays from the window TWAP by more than max_spot_deviation_bps, holding the clock.
   /// Partial payments advance a floored pro-rata clock within the bounded 72-hour accrual interval; every payment
   /// restarts the observation, so a partial payment's remainder waits for, and is priced by, a fresh window.
   /// Skips retain state.
   [[sysio::action]] void kick(symbol_code sym);
private:
   /// Validate governance settings at both pool configuration boundaries.
   static void validate_pool(uint16_t rate_bps, uint64_t min_gift, uint64_t max_gift_per_day);
};
} // namespace sysio
