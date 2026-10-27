#pragma once
/**
 * @file yield_cranks.hpp
 * @brief The decisions behind the depot's yield cranks -- which `sysio.swap`
 *        yield pools are worth a `tickyield`, which `sysio.liq` shadows are worth a
 *        `queueyield`, which `sysio.kicker` pools are worth a `kick` -- and the
 *        per-key spacing that keeps one operator from cranking the same pool every
 *        poll. Pure functions over decoded rows, so the plugin's tests drive them
 *        without a chain.
 */

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/symbol.hpp>

namespace sysio::batch_operator_detail {

/// `sysio.swap` identifiers the yield-tick crank touches.
namespace swap {
   constexpr auto account          = "sysio.swap";
   constexpr auto table_stat       = "stat";
   constexpr auto table_reservoirs = "reservoirs";
   constexpr auto action_tickyield = "tickyield";
   namespace field {
      constexpr auto supply                 = "supply";
      constexpr auto yield_leg              = "yield_leg";
      constexpr auto conversion_horizon_sec = "conversion_horizon_sec";
      constexpr auto depth_cap_bps          = "depth_cap_bps";
      constexpr auto clip_floor             = "clip_floor";
      constexpr auto balance                = "balance";
      constexpr auto quantity               = "quantity";
      constexpr auto symbol_code            = "symbol_code";   ///< the kv key of `stat` / `reservoirs`
      constexpr auto pair_token             = "pair_token";    ///< `tickyield`'s argument
   }
}

/// `sysio.liq` identifiers the pending-yield crank and the underwriter touch.
namespace liq {
   constexpr auto account           = "sysio.liq";
   constexpr auto table_liqpending  = "liqpending";
   constexpr auto table_accounts    = "accounts";
   constexpr auto table_stat        = "stat";
   constexpr auto action_queueyield = "queueyield";
   namespace field {
      constexpr auto quantity    = "quantity";
      constexpr auto symbol_code = "symbol_code";   ///< the kv key of `liqpending`
      constexpr auto sym         = "sym";           ///< `queueyield`'s argument
      constexpr auto balance     = "balance";       ///< an `accounts` row's holding
      constexpr auto supply      = "supply";        ///< a `stat` row's supply, in the shadow's symbol
      constexpr auto token_code  = "token_code";    ///< a `stat` row's registry token code
      constexpr auto chain_code  = "chain_code";    ///< a `stat` row's outpost
   }
}

/// `sysio.kicker` identifiers the kick crank touches.
namespace kicker {
   /// Bound the pool page, its supply lookups and the serial transaction attempts in one poll.
   inline constexpr uint32_t pools_per_poll = 10;
   /// Longest wait between one operator's unpaid kick attempts on one pool, and the most
   /// `--batch-kick-interval-ms` may ask for as the first.
   inline constexpr fc::microseconds max_retry_wait = fc::hours(1);
   constexpr auto account         = "sysio.kicker";
   constexpr auto table_kickcfg   = "kickcfg";     ///< the `kv::global` config singleton
   constexpr auto table_kickpools = "kickpools";   ///< one row per LIQ token
   constexpr auto action_kick     = "kick";
   /// One UTC day, the period of a pool's `max_gift_per_day` ceiling.
   inline constexpr int64_t day_us = int64_t{86'400} * 1'000'000;
   namespace field {
      constexpr auto budget_remaining = "budget_remaining";
      constexpr auto min_interval_sec = "min_interval_sec";
      constexpr auto twap_window_sec  = "twap_window_sec";
      constexpr auto last_kick        = "last_kick";
      constexpr auto min_gift         = "min_gift";
      constexpr auto max_gift_per_day = "max_gift_per_day";
      constexpr auto day              = "day";
      constexpr auto spent_today      = "spent_today";
      constexpr auto price_time       = "price_time";
      constexpr auto sym              = "sym";   ///< the pool row's token and `kick`'s argument
   }
}

/// The kv row wrapper `get_table_rows` returns when `values_only` is off.
namespace kv_row {
   constexpr auto key   = "key";
   constexpr auto value = "value";
}

/// The amount of the ABI-rendered asset (`"1.000000000 SYM"`) under `field`; 0
/// when the field is absent or does not parse -- a crank decides from what it can
/// read and pushes nothing on a row it cannot.
inline int64_t asset_amount(const fc::variant_object& row, const char* field) {
   auto it = row.find(field);
   if (it == row.end() || !it->value().is_string()) return 0;
   try {
      return chain::asset::from_string(it->value().as_string()).get_amount();
   } catch (const fc::exception&) {
      return 0;
   }
}

/// The kv key of a `stat` / `reservoirs` / `liqpending` row (the `{key, value}`
/// wrapper): the pool or shadow symbol code, or nullopt when the row carries none.
inline std::optional<chain::symbol_code> row_symbol_code(const fc::variant_object& row) {
   auto key = row.find(kv_row::key);
   if (key == row.end() || !key->value().is_object()) return std::nullopt;
   const auto& key_obj = key->value().get_object();
   auto code = key_obj.find(swap::field::symbol_code);
   if (code == key_obj.end()) return std::nullopt;
   return chain::symbol_code{ code->value().as_uint64() };
}

/// The name of a symbol code -- the spelling `tickyield` / `queueyield` take and
/// the logs use.
inline std::string symbol_code_name(chain::symbol_code code) {
   return chain::symbol(code.value << 8).name();
}

/// The `value` of a `{key, value}` row, or nullopt when the row carries none.
inline std::optional<fc::variant_object> row_value(const fc::variant_object& row) {
   auto value = row.find(kv_row::value);
   if (value == row.end() || !value->value().is_object()) return std::nullopt;
   return value->value().get_object();
}

/// True iff a `sysio.swap::stat` row is a yield pool `tickyield` would accept: a
/// shadow `yield_leg` plus all three tick parameters set -- the exact preconditions
/// the action `check()`s, so a crank never pays for a push the contract is
/// guaranteed to reject.
inline bool is_tickable_pool(const fc::variant_object& stat_row) {
   auto positive = [&](const char* field) {
      auto it = stat_row.find(field);
      return it != stat_row.end() && it->value().as_int64() > 0;
   };
   auto leg = stat_row.find(swap::field::yield_leg);
   return leg != stat_row.end() && !leg->value().is_null() &&
          positive(swap::field::conversion_horizon_sec) &&
          positive(swap::field::depth_cap_bps) &&
          positive(swap::field::clip_floor);
}

/// What the kick crank reads from `sysio.kicker::kickcfg`.
struct kick_settings {
   fc::microseconds min_interval;       ///< `min_interval_sec`: unpaid accrual before an attempt
   fc::microseconds twap_window;        ///< `twap_window_sec`: the price observation a payment needs
   uint64_t         budget_remaining = 0;
};

namespace kick_detail {
   /// `field` of `row` as an unsigned integer that fits 64 bits, or nullopt when absent, unreadable or wider.
   /// Read through `as_uint128`: a uint128 column (`spent_today`) reaches an in-process reader as a uint128
   /// variant, on which `as_uint64` throws, and the HTTP API renders the same column as a decimal string; both read
   /// here, as do the uint32/uint64 columns.
   inline std::optional<uint64_t> unsigned_field(const fc::variant_object& row, const char* field) {
      auto it = row.find(field);
      if (it == row.end()) return std::nullopt;
      try {
         const fc::uint128 value = it->value().as_uint128();
         if (value > std::numeric_limits<uint64_t>::max()) return std::nullopt;
         return static_cast<uint64_t>(value);
      } catch (const fc::exception&) {
         return std::nullopt;
      }
   }
   /// `field` of `row` as a time point, or nullopt when absent or unreadable.
   inline std::optional<fc::time_point> time_field(const fc::variant_object& row, const char* field) {
      auto it = row.find(field);
      if (it == row.end()) return std::nullopt;
      try {
         return it->value().as<fc::time_point>();
      } catch (const fc::exception&) {
         return std::nullopt;
      }
   }
}

/// The settings of a `sysio.kicker::kickcfg` value, or nullopt when a field is absent, unreadable, or a value
/// `setconfig` refuses (a zero interval, a window under a minute): a row the crank cannot reason about, so it
/// pushes nothing.
inline std::optional<kick_settings> read_kick_settings(const fc::variant_object& config) {
   constexpr uint64_t min_twap_window_sec = 60;
   const auto interval = kick_detail::unsigned_field(config, kicker::field::min_interval_sec);
   const auto window   = kick_detail::unsigned_field(config, kicker::field::twap_window_sec);
   const auto budget   = kick_detail::unsigned_field(config, kicker::field::budget_remaining);
   if (!interval || *interval == 0 || !window || *window < min_twap_window_sec || !budget) return std::nullopt;
   return kick_settings{.min_interval     = fc::seconds(static_cast<int64_t>(*interval)),
                        .twap_window      = fc::seconds(static_cast<int64_t>(*window)),
                        .budget_remaining = *budget};
}

/// The `last_kick` of a `sysio.kicker::kickpools` value, or nullopt when the field
/// is absent or does not parse.
inline std::optional<fc::time_point> kick_last_kick(const fc::variant_object& pool) {
   return kick_detail::time_field(pool, kicker::field::last_kick);
}

/// Each LIQ token's current `sysio.liq::stat` supply, in subunits, keyed by the symbol name `kick` takes.
using liq_supplies = std::map<std::string, int64_t>;

/// The supplies of the `sysio.liq::stat` values in `rows`; a row whose supply does not parse is left out.
inline liq_supplies read_liq_supplies(const fc::variants& rows) {
   liq_supplies out;
   for (const fc::variant& row : rows) {
      if (!row.is_object()) continue;
      auto it = row.get_object().find(liq::field::supply);
      if (it == row.get_object().end() || !it->value().is_string()) continue;
      try {
         const auto supply = chain::asset::from_string(it->value().as_string());
         out[supply.get_symbol().name()] = supply.get_amount();
      } catch (const fc::exception&) {
      }
   }
   return out;
}

/// Whether `--batch-kick-interval-ms` is a usable first retry wait: positive, since `kick_backoff` doubles it and a
/// zero would retry an unpaid kick every poll forever, and no longer than `kicker::max_retry_wait`, the ceiling the
/// backoff doubles up to.
inline bool kick_interval_valid(uint32_t interval_ms) {
   return interval_ms > 0 && fc::milliseconds(interval_ms) <= kicker::max_retry_wait;
}

/// The symbol codes of the `kickpools` rows on a page, in page order.
inline std::vector<chain::symbol_code> page_symbol_codes(const fc::variants& rows) {
   std::vector<chain::symbol_code> out;
   for (const fc::variant& r : rows) {
      if (!r.is_object()) continue;
      if (const auto code = row_symbol_code(r.get_object())) out.push_back(*code);
   }
   return out;
}

/// The supplies of the LIQ tokens on a page, each read with `read_stat(code)`: the matching `sysio.liq::stat`
/// values (empty when the token has no row), or nullopt when the read failed. Any failed read fails the whole
/// lookup: the crank does not act on part of the view.
template <typename ReadStat>
std::optional<liq_supplies> read_page_supplies(const fc::variants& rows, ReadStat&& read_stat) {
   fc::variants stats;
   for (const chain::symbol_code code : page_symbol_codes(rows)) {
      const std::optional<fc::variants> found = read_stat(code);
      if (!found) return std::nullopt;
      stats.insert(stats.end(), found->begin(), found->end());
   }
   return read_liq_supplies(stats);
}

/// True iff `kick` at `now` could pay: every early return and every nonpaying outcome the crank can see from the
/// rows is ruled out. The action returns without paying while
///  - less than `min_interval` has passed since `last_kick` (its clock check),
///  - the token's LIQ supply is zero (it then only restarts the clock, a write that pays nothing),
///  - the price observation since `price_time` is shorter than `twap_window` (TWAP warm-up; every payment restarts
///    the observation, so this alone keeps a paid pool quiet for a window),
///  - the remaining budget is below `min_gift`, or
///  - this UTC day's spending has left less than `min_gift` under `max_gift_per_day`.
/// A row with a zero `supply_snapshot` under a nonzero supply stays due: that kick only checkpoints the supply,
/// but accrual starts at that checkpoint, so it has to be sent. The contract sizes a gift on the smaller of
/// `supply_snapshot` and the current supply; the crank does not price gifts, so neither supply narrows it further.
/// A row missing any of these fields is never due, nor is a token without a supply. What the rows cannot show -- an Andon hold, a gift below
/// `min_gift`, a treasury reserve, spot past the contract's spot-versus-TWAP guard, a broken swap pair -- is bounded
/// by the caller's `kick_backoff`.
inline bool kick_due(const fc::variant_object& pool, const kick_settings& settings, const liq_supplies& supplies,
                     fc::time_point now) {
   const auto last_kick  = kick_last_kick(pool);
   const auto price_time = kick_detail::time_field(pool, kicker::field::price_time);
   const auto minimum    = kick_detail::unsigned_field(pool, kicker::field::min_gift);
   const auto cap        = kick_detail::unsigned_field(pool, kicker::field::max_gift_per_day);
   const auto day        = kick_detail::unsigned_field(pool, kicker::field::day);
   const auto spent      = kick_detail::unsigned_field(pool, kicker::field::spent_today);
   auto       sym        = pool.find(kicker::field::sym);
   if (!last_kick || !price_time || !minimum || !cap || !day || !spent) return false;
   if (sym == pool.end() || !sym->value().is_string()) return false;
   const auto supply = supplies.find(sym->value().as_string());
   if (supply == supplies.end() || supply->second <= 0) return false;
   if (now <= *last_kick || now - *last_kick < settings.min_interval) return false;
   if (now <= *price_time || now - *price_time < settings.twap_window) return false;
   if (settings.budget_remaining < *minimum) return false;
   const auto today = static_cast<uint64_t>(now.time_since_epoch().count() / kicker::day_us);
   const uint64_t spent_now = *day == today ? *spent : 0;
   const uint64_t allowance = *cap > spent_now ? *cap - spent_now : 0;
   return allowance >= *minimum;
}

/// Retry spacing for kicks that may not pay. The first attempt on a pool waits `base` before the next; each further
/// attempt that found `last_kick` where the previous one left it -- nothing was paid -- doubles the wait, up to
/// `ceiling`. A moved `last_kick` (a payment, by this operator or another) restarts at `base`. Andon holds, gifts
/// below `min_gift` and reserve shortfalls are not visible in the rows the crank reads, and every ACTIVE operator
/// cranks every pool: without the backoff each would push an accepted no-op kick once per `base` for as long as the
/// condition lasts.
class kick_backoff {
public:
   bool due(const std::string& key, fc::time_point last_kick, fc::time_point now) const {
      auto it = _attempts.find(key);
      return it == _attempts.end() || it->second.last_kick != last_kick || now - it->second.at >= it->second.wait;
   }
   void mark(const std::string& key, fc::time_point last_kick, fc::time_point now, fc::microseconds base,
             fc::microseconds ceiling) {
      auto [it, added] = _attempts.try_emplace(key, attempt{now, last_kick, base});
      if (added) return;
      auto& prior = it->second;
      prior.wait      = prior.last_kick == last_kick ? std::min(prior.wait + prior.wait, ceiling) : base;
      prior.at        = now;
      prior.last_kick = last_kick;
   }
   /// The wait before `key` may be retried, for tests and logs; zero when it has no attempt.
   fc::microseconds wait(const std::string& key) const {
      auto it = _attempts.find(key);
      return it == _attempts.end() ? fc::microseconds() : it->second.wait;
   }
   /// Drop retired symbols only after a complete, unfiltered table sweep.
   void retain(const std::set<std::string>& live) {
      std::erase_if(_attempts, [&](const auto& entry) { return !live.contains(entry.first); });
   }

private:
   struct attempt {
      fc::time_point   at;
      fc::time_point   last_kick;
      fc::microseconds wait;
   };
   std::map<std::string, attempt> _attempts;
};

/// One pass of the kick crank over `rows` (`kickpools` rows with their kv keys): for each pool `kick_due` passes
/// and `backoff` allows, mark the attempt and `push(code)`. Stops, returning false, as soon as `stopping()`.
template <typename Push, typename Now, typename Stopping>
bool kick_rows(const fc::variants& rows, const kick_settings& settings, const liq_supplies& supplies,
               kick_backoff& backoff, fc::microseconds base, fc::microseconds ceiling, Push&& push, Now&& now,
               Stopping&& stopping) {
   for (const fc::variant& r : rows) {
      if (stopping()) return false;
      if (!r.is_object()) continue;
      const auto code  = row_symbol_code(r.get_object());
      const auto value = row_value(r.get_object());
      if (!code || !value) continue;
      const auto at = now();
      if (!kick_due(*value, settings, supplies, at)) continue;
      const auto pool      = symbol_code_name(*code);
      const auto last_kick = *kick_last_kick(*value);   // kick_due parsed it
      if (!backoff.due(pool, last_kick, at)) continue;
      backoff.mark(pool, last_kick, at, base, ceiling);
      push(*code);
   }
   return true;
}

/// One push per `interval` per key from this operator. `tickyield` is harmless to
/// crank every block, but every ACTIVE operator polls every few seconds and a
/// reservoir drains over a horizon of hours, so without spacing the whole group
/// would push a tick per pool per poll for the length of the sale.
class crank_spacing {
public:
   bool due(const std::string& key, fc::time_point now, fc::microseconds interval) const {
      auto it = _last_push.find(key);
      return it == _last_push.end() || now - it->second >= interval;
   }
   void mark(const std::string& key, fc::time_point now) { _last_push[key] = now; }

private:
   std::map<std::string, fc::time_point> _last_push;
};

/// Retain the chain's inclusive next-key cursor and prune spacing at the end of each sweep.
class kick_pool_scan {
public:
   /// Feed directly to get_table_rows.lower_bound; empty starts the next sweep.
   const std::string& lower_bound() const { return _next; }
   /// Record every decoded symbol, including pools that are not due.
   void observe(const std::string& symbol) { _live.insert(symbol); }
   /// Call only after a successful page read. A failed read is not the end of a sweep: the caller skips this, so
   /// the cursor stays on the unread page and no retry marker is pruned for a pool the sweep has not reached.
   void finish_page(bool more, const std::string& next, kick_backoff& backoff) {
      _next = more ? next : std::string{};
      if (!more) {
         backoff.retain(_live);
         _live.clear();
      }
   }
private:
   std::string _next;
   std::set<std::string> _live;
};

/// One poll of the paged kick crank: read the page at `scan`'s cursor with `read_page(lower_bound)` (nullopt when
/// the read failed), read the supplies of just that page's tokens with `read_stat` (see `read_page_supplies`), run
/// `kick_rows` over the page, then finish it. A failed read and a pass cut short by `stopping()` finish nothing:
/// the cursor stays on the unread page and no retry marker is pruned. Returns whether the page was finished.
template <typename ReadPage, typename ReadStat, typename Push, typename Now, typename Stopping>
bool kick_page(kick_pool_scan& scan, kick_backoff& backoff, const kick_settings& settings, fc::microseconds base,
               fc::microseconds ceiling, ReadPage&& read_page, ReadStat&& read_stat, Push&& push, Now&& now,
               Stopping&& stopping) {
   const auto page = read_page(scan.lower_bound());
   if (!page) return false;
   const std::optional<liq_supplies> supplies = read_page_supplies(page->rows, read_stat);
   if (!supplies) return false;
   if (!kick_rows(page->rows, settings, *supplies, backoff, base, ceiling, push, now, stopping)) return false;
   // Only a finished page's symbols join the sweep's live set: an unfinished one is read again.
   for (const chain::symbol_code code : page_symbol_codes(page->rows)) scan.observe(symbol_code_name(code));
   scan.finish_page(page->more, page->next_key, backoff);
   return true;
}

} // namespace sysio::batch_operator_detail
