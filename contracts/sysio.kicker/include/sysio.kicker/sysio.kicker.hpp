#pragma once
/**
 * @file sysio.kicker.hpp
 * @brief Governance-budgeted T5 gifts to LIQ holders, triggered by any keeper.
 *
 * Deploy privileged with roa::setsyscode. Budget is an earmark outside emissions;
 * no draw alters sysio.system::t5state.total_distributed. Spot price and current
 * supply value the entire unpaid interval, including an Andon hold or shortfall.
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

   /// Governance budget in WIRE subunits and minimum elapsed payment interval.
   struct [[sysio::table("kickcfg")]] kick_config {
      uint64_t budget_remaining = 0; ///< Absolute remaining earmark; setconfig replaces it.
      /// Minimum unpaid accrual interval before an attempt.
      uint32_t min_interval_sec = kicker_math::default_min_interval_sec;
      SYSLIB_SERIALIZE(kick_config, (budget_remaining)(min_interval_sec))
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
      uint64_t max_gift_per_day = 0; ///< Zero disables the UTC-day spend ceiling.
      uint64_t day = 0; ///< UTC day number of the most recent payment.
      uint128_t spent_today = 0;
      SYSLIB_SERIALIZE(kick_pool, (sym)(rate_bps)(min_gift)(last_kick)(gifted_total)
                                (shortfall_amount)(shortfall_time)(gift_overflow)
                                (max_gift_per_day)(day)(spent_today))
   };
   /// Governance-owned singleton and per-symbol payment clocks.
   using config_table = kv::global<"kickcfg"_n, kick_config>;
   using pools_table = kv::table<"kickpools"_n, pool_key, kick_pool>;

   /// Replace the remaining budget and interval. Requires sysio; never settles open intervals.
   [[sysio::action]] void setconfig(kick_config cfg);
   /// Start accrual now for a registered LIQ/WIRE yield pair. Requires sysio; defaults to 200 bps and 1 WIRE.
   [[sysio::action]] void addpool(symbol_code sym, uint16_t rate_bps = kicker_math::default_rate_bps,
                                uint64_t min_gift = kicker_math::default_min_gift, uint64_t max_gift_per_day = 0);
   /// Change a token's rate/minimum/daily ceiling; the new rate prices the whole open interval. Requires sysio.
   [[sysio::action]] void setpool(symbol_code sym, uint16_t rate_bps, uint64_t min_gift, uint64_t max_gift_per_day);
   /// Stop accrual and erase the row; re-adding starts a new interval. Requires sysio.
   [[sysio::action]] void rmpool(symbol_code sym);
   /// Permissionless gift; partial payments advance a floored pro-rata clock.
   /// Skips and overflow retain the unpaid interval.
   [[sysio::action]] void kick(symbol_code sym);
private:
   /// Validate governance settings at both pool configuration boundaries.
   static void validate_pool(uint16_t rate_bps, uint64_t min_gift, uint64_t max_gift_per_day);
};
} // namespace sysio
