#include <sysio.kicker/sysio.kicker.hpp>
#include <sysio.liq/sysio.liq.hpp>
#include <sysio.token/sysio.token.hpp>
#include <sysio.system/emissions.hpp>
#include <sysio.andon/sysio.andon.hpp>
#include <sysio.opp.common/swap_state.hpp>
#include <sysio.opp.common/wire_asset.hpp>
#include <algorithm>

namespace sysio {
/// Keep the dependency-free arithmetic bounds in lock-step with the protocol asset range.
static_assert(kicker_math::max_amount == asset::max_amount);
namespace {
/// Stable governance and keeper diagnostics.
namespace errors {
constexpr auto no_yield_pool = "LIQ token has no yield pool";
constexpr auto missing_swap_pool = "swap pool does not exist";
constexpr auto invalid_pair = "pool must be LIQ/WIRE with a LIQ yield leg";
constexpr auto negative_reserve = "negative swap reserve";
constexpr auto missing_config = "kicker not configured";
constexpr auto negative_pending = "negative pending emissions";
constexpr auto invalid_balance = "invalid treasury WIRE balance";
constexpr auto invalid_rate = "rate exceeds 10000 bps";
constexpr auto invalid_minimum = "invalid minimum gift";
constexpr auto invalid_cap = "invalid daily cap";
constexpr auto invalid_budget = "budget exceeds asset range";
constexpr auto invalid_interval = "minimum interval must be positive";
constexpr auto invalid_symbol = "invalid LIQ symbol";
constexpr auto missing_shadow = "LIQ token does not exist";
constexpr auto duplicate_pool = "pool already configured";
constexpr auto unknown_pool = "unknown kicker pool";
constexpr auto negative_supply = "negative LIQ supply";
} // namespace errors

/// Inline treasury draw memo, preserved on the token transfer trace.
constexpr auto gift_memo = "T5 LIQ kicker";
/// Active permission for privileged inline transfers and addyield.
constexpr name active_permission = "active"_n;
/// Validate the shared LIQ stat and its live swap pair together, never price a mismatched leg.
opp::swap_state::currency_stats pair_for(symbol_code sym, const liq::currency_stats& shadow) {
   check(shadow.pair_symbol.raw() != 0, errors::no_yield_pool);
   const auto pair = opp::swap_state::stats(opp::swap_state::account).get(
      {shadow.pair_symbol.raw()}, errors::missing_swap_pool);
   check(pair.pool1.contract == kicker::liq_account && pair.pool1.quantity.symbol == shadow.supply.symbol &&
         pair.pool1.quantity.symbol.code() == sym && pair.pool2.contract == kicker::token_account &&
         pair.pool2.quantity.symbol == opp::wire::asset_symbol && pair.yield_leg &&
         pair.yield_leg->get_contract() == kicker::liq_account &&
         pair.yield_leg->get_symbol() == shadow.supply.symbol, errors::invalid_pair);
   check(pair.pool1.quantity.amount >= 0 && pair.pool2.quantity.amount >= 0, errors::negative_reserve);
   return pair;
}
/// Reserve pending emissions, outstanding claims and the remaining emission ceiling; missing state fails closed.
uint64_t treasury_available() {
   using namespace sysiosystem::emissions;
   const t5state_t treasury(kicker::system_account);
   const emitcfg_t emission_config(kicker::system_account);
   if (!treasury.exists() || !emission_config.exists()) return 0;
   const auto state = treasury.get();
   check(state.pending_emission_amount >= 0, errors::negative_pending);
   const auto claims = payclaimtot_t(kicker::system_account).get_or_default(pay_claim_total{});
   const token::accounts balances(kicker::token_account, kicker::system_account.value);
   const token::acct_key key{opp::wire::asset_symbol.code().raw()};
   if (!balances.contains(key)) return 0;
   const auto balance = balances.get(key).balance;
   check(balance.symbol == opp::wire::asset_symbol && balance.amount >= 0, errors::invalid_balance);
   const auto emission = emission_config.get();
   // Widen before subtraction so malformed or exhausted state cannot wrap a reserve.
   const auto remaining = std::max<__int128>(0, static_cast<__int128>(emission.t5_distributable) -
                                               emission.t5_floor - state.total_distributed);
   const auto reserved = static_cast<uint128_t>(state.pending_emission_amount) + claims.outstanding +
                         static_cast<uint128_t>(remaining);
   if (reserved >= static_cast<uint128_t>(balance.amount)) return 0;
   return static_cast<uint64_t>(static_cast<uint128_t>(balance.amount) - reserved);
}
} // namespace

void kicker::validate_pool(uint16_t rate_bps, uint64_t min_gift, uint64_t max_gift_per_day) {
   check(rate_bps <= kicker_math::bps_scale, errors::invalid_rate);
   check(min_gift > 0 && min_gift <= kicker_math::max_amount, errors::invalid_minimum);
   check(max_gift_per_day <= kicker_math::max_amount &&
         (max_gift_per_day == 0 || max_gift_per_day >= min_gift), errors::invalid_cap);
}
void kicker::setconfig(kick_config cfg) {
   require_auth(system_account);
   check(cfg.budget_remaining <= kicker_math::max_amount, errors::invalid_budget);
   check(cfg.min_interval_sec > 0, errors::invalid_interval);
   config_table(get_self()).set(cfg, system_account);
}
void kicker::addpool(symbol_code sym, uint16_t rate_bps, uint64_t min_gift, uint64_t max_gift_per_day) {
   require_auth(system_account);
   validate_pool(rate_bps, min_gift, max_gift_per_day);
   check(sym.is_valid(), errors::invalid_symbol);
   const auto shadow = liq::stats(liq_account).get({sym.raw()}, errors::missing_shadow);
   pair_for(sym, shadow);
   pools_table rows(get_self());
   check(rows.find({sym.raw()}) == rows.end(), errors::duplicate_pool);
   kick_pool pool;
   pool.sym = sym;
   pool.rate_bps = rate_bps;
   pool.min_gift = min_gift;
   pool.max_gift_per_day = max_gift_per_day;
   pool.last_kick = current_time_point();
   rows.emplace(system_account, {sym.raw()}, [&](auto& row) { row = pool; });
}
void kicker::setpool(symbol_code sym, uint16_t rate_bps, uint64_t min_gift, uint64_t max_gift_per_day) {
   require_auth(system_account);
   validate_pool(rate_bps, min_gift, max_gift_per_day);
   pools_table rows(get_self());
   const auto it = rows.find({sym.raw()});
   check(it != rows.end(), errors::unknown_pool);
   rows.modify(system_account, pool_key{sym.raw()}, [&](auto& row) {
      row.rate_bps = rate_bps;
      row.min_gift = min_gift;
      row.max_gift_per_day = max_gift_per_day;
   });
}
void kicker::rmpool(symbol_code sym) {
   require_auth(system_account);
   pools_table rows(get_self());
   const auto it = rows.find({sym.raw()});
   check(it != rows.end(), errors::unknown_pool);
   rows.erase(it);
}
void kicker::kick(symbol_code sym) {
   config_table config(get_self());
   auto cfg = config.get(errors::missing_config);
   pools_table rows(get_self());
   auto pool = rows.get({sym.raw()}, errors::unknown_pool);
   const auto now = current_time_point();
   if (now <= pool.last_kick) return;
   const uint64_t elapsed = (now - pool.last_kick).count();
   if (elapsed < static_cast<uint64_t>(cfg.min_interval_sec) * kicker_math::micros_per_sec ||
       andon::pulled(andon::ANDON_ACCOUNT)) return;
   const auto shadow = liq::stats(liq_account).get({sym.raw()}, errors::missing_shadow);
   check(shadow.supply.amount >= 0, errors::negative_supply);
   if (shadow.supply.amount == 0) {
      pool.last_kick = now;
      pool.shortfall_amount = 0;
      pool.shortfall_time = time_point{};
      pool.gift_overflow = false;
      rows.set(system_account, {sym.raw()}, pool);
      return;
   }
   const auto pair = pair_for(sym, shadow);
   const auto gift = kicker_math::gift(shadow.supply.amount, pool.rate_bps, elapsed,
                                      pair.pool2.quantity.amount, pair.pool1.quantity.amount);
   if (gift && *gift < pool.min_gift) return;
   const uint64_t day = static_cast<uint64_t>(now.time_since_epoch().count()) /
                        (kicker_math::day_sec * kicker_math::micros_per_sec);
   const uint128_t spent = day == pool.day ? pool.spent_today : 0;
   uint64_t available = std::min(cfg.budget_remaining, treasury_available());
   if (pool.max_gift_per_day != 0)
      available = std::min(available, pool.max_gift_per_day > spent
                                         ? pool.max_gift_per_day - static_cast<uint64_t>(spent) : 0);
   // Overflow has no exact representable denominator: keep the entire interval open.
   const uint64_t paid = gift && available >= pool.min_gift ? std::min(*gift, available) : 0;
   pool.shortfall_amount = gift ? *gift - paid : 0;
   pool.gift_overflow = !gift;
   if (!gift || paid < pool.min_gift) {
      pool.shortfall_time = now;
      rows.set(system_account, {sym.raw()}, pool);
      return;
   }
   // Both operands fit uint64; the widened product is exact, floored per the owner's ruling; the sub-microsecond
   // remainder stays in the open interval (holder-favoured, < 1 subunit at realistic supply).
   pool.last_kick += microseconds(static_cast<int64_t>(static_cast<uint128_t>(elapsed) * paid / *gift));
   pool.gifted_total += paid;
   pool.shortfall_time = pool.shortfall_amount ? now : time_point{};
   pool.day = day;
   pool.spent_today = spent + paid;
   cfg.budget_remaining -= paid;
   rows.set(system_account, {sym.raw()}, pool);
   config.set(cfg, system_account);
   token::transfer_action(token_account, {system_account, active_permission}).send(
      system_account, get_self(), asset(paid, opp::wire::asset_symbol), gift_memo);
   action(permission_level{get_self(), active_permission}, liq_account, opp::shadow::ADDYIELD_ACTION,
          std::make_tuple(get_self(), asset(paid, opp::wire::asset_symbol), sym)).send();
}
} // namespace sysio
