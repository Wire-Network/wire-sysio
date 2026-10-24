#include "../src/yield_cranks.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <fc/int128.hpp>
#include <fc/io/json.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/symbol.hpp>

using namespace sysio::batch_operator_detail;
using mvo = fc::mutable_variant_object;

namespace {

/// Independent deployment fixtures: the production TWAP window and the shortest `setconfig` accepts.
constexpr uint32_t short_twap_window_sec = 60;
constexpr uint32_t production_twap_window_sec = 86'400;
constexpr uint32_t configured_min_interval_sec = 3600;
constexpr int64_t  day_seconds = 86'400;

constexpr auto    pool_code    = "POOLB";
constexpr auto    shadow_code  = "LIQSOL";
constexpr int64_t one_token    = 1'000'000'000;   // one whole 9-decimal token, in subunits

uint64_t raw_code(const char* code) { return sysio::chain::symbol(0, code).to_symbol_code().value; }

/// A `sysio.swap::stat` value as the chain renders it for a yield pool: the shadow
/// leg set and the three tick parameters configured by `setyield`.
mvo yield_pool_row() {
   return mvo()
      ("supply",                 std::string("100.000000000 ") + pool_code)
      ("yield_leg",              mvo()("sym", std::string("9,") + shadow_code)("contract", "sysio.liq"))
      ("conversion_horizon_sec", 86400)
      ("depth_cap_bps",          50)
      ("clip_floor",             1000);
}

/// A `{key, value}` kv row with `code` as the symbol-code key.
mvo kv(const char* code, mvo value) {
   return mvo()("key", mvo()("symbol_code", raw_code(code)))("value", std::move(value));
}

/// The committed ABI at `path` (`KICKER_ABI_PATH` / `LIQ_ABI_PATH`, set by the test target).
sysio::chain::abi_serializer load_abi(const char* path) {
   return sysio::chain::abi_serializer(fc::json::from_file(path).as<sysio::chain::abi_def>(),
                                       sysio::chain::abi_serializer::create_depth_yield_function());
}
const sysio::chain::abi_serializer& kicker_abi() {
   static const auto abi = load_abi(KICKER_ABI_PATH);
   return abi;
}
const sysio::chain::abi_serializer& liq_abi() {
   static const auto abi = load_abi(LIQ_ABI_PATH);
   return abi;
}

/// `value` as the node's own table reader hands it to the crank: packed with the contract's ABI and decoded by
/// `abi_serializer`, the path `read_table_rows_checked` takes. Its uint128 columns are uint128 variants there, not
/// the decimal strings the HTTP API prints.
mvo as_chain_renders(const sysio::chain::abi_serializer& abi, const char* type, const fc::variant& value) {
   // Depth-bounded only, like the contract fixtures: a wall-clock budget would make the test host-dependent.
   const auto yield = sysio::chain::abi_serializer::create_depth_yield_function();
   return mvo(abi.binary_to_variant(type, abi.variant_to_binary(type, value, yield), yield).get_object());
}

/// A `sysio.kicker::kickcfg` value as the chain renders it.
mvo kick_config_row(uint64_t budget, uint32_t window = production_twap_window_sec,
                    uint32_t interval = configured_min_interval_sec) {
   return as_chain_renders(kicker_abi(), "kick_config", mvo()
      ("budget_remaining", budget)("min_interval_sec", interval)("twap_window_sec", window)
      ("max_spot_deviation_bps", 5'000));   // the contract's default
}

/// `kick_config_row` read back as the crank reads it.
kick_settings settings(uint64_t budget = 1000 * one_token, uint32_t window = production_twap_window_sec) {
   return read_kick_settings(kick_config_row(budget, window)).value();
}

/// A `sysio.kicker::kickpools` value as the chain renders it: the LIQ token, its rate, minimum and daily ceiling,
/// the accrual clock, the price observation and this UTC day's spending (a uint128 column).
mvo kick_pool_row(fc::time_point last_kick, fc::time_point price_time, uint64_t day = 0, uint64_t spent = 0,
                  uint64_t cap = 1000 * one_token, const char* sym = shadow_code) {
   return as_chain_renders(kicker_abi(), "kick_pool", mvo()
      ("sym",              sym)
      ("rate_bps",         200)
      ("min_gift",         one_token)
      ("last_kick",        last_kick)
      ("gifted_total",     fc::uint128(5))
      ("shortfall_amount", 0)
      ("shortfall_time",   fc::time_point())
      ("gift_overflow",    false)
      ("spot_deviation",   false)
      ("max_gift_per_day", cap)
      ("day",              day)
      ("spent_today",      fc::uint128(spent))
      ("price_snapshot",   mvo()("lo", fc::uint128(0))("hi", fc::uint128(0)))
      ("price_time",       price_time));
}

/// A pool whose observation matured long ago, so only the clock, supply and budget decide.
mvo kick_pool_row(fc::time_point last_kick, const char* sym = shadow_code) {
   return kick_pool_row(last_kick, last_kick - fc::days(2), 0, 0, 1000 * one_token, sym);
}

/// The UTC day number of `t`, as `kick` records it.
uint64_t utc_day(fc::time_point t) { return static_cast<uint64_t>(t.sec_since_epoch() / day_seconds); }

/// A `sysio.liq::stat` value as the chain renders it.
fc::variant liq_stat_row(const std::string& supply) {
   return fc::variant(as_chain_renders(liq_abi(), "currency_stats", mvo()
      ("supply", supply)("chain_code", "SOLANA")("token_code", "SOL")("pair_symbol", pool_code)));
}

/// `amount` subunits of the 9-decimal LIQ token `sym`, as an asset string.
std::string liq_asset(int64_t amount, const char* sym = shadow_code) {
   return sysio::chain::asset(amount, sysio::chain::symbol(9, sym)).to_string();
}

/// The supplies the crank reads for the fixture's token.
liq_supplies supplies(int64_t amount = 100 * one_token) {
   return read_liq_supplies({liq_stat_row(liq_asset(amount))});
}

} // namespace

BOOST_AUTO_TEST_SUITE(yield_cranks_tests)

BOOST_AUTO_TEST_CASE(a_yield_pool_with_its_tick_parameters_is_tickable) {
   BOOST_CHECK(is_tickable_pool(yield_pool_row()));
}

BOOST_AUTO_TEST_CASE(a_plain_pool_is_not_tickable) {
   auto row = yield_pool_row();
   row.set("yield_leg", fc::variant());   // the optional is empty: no shadow leg
   BOOST_CHECK(!is_tickable_pool(row));
}

BOOST_AUTO_TEST_CASE(a_yield_pool_without_tick_parameters_is_not_tickable) {
   for (const char* field : {"conversion_horizon_sec", "depth_cap_bps", "clip_floor"}) {
      auto zeroed = yield_pool_row();
      zeroed.set(field, 0);
      BOOST_CHECK_MESSAGE(!is_tickable_pool(zeroed), field << " = 0 must refuse the tick");
      auto missing = yield_pool_row();
      missing.erase(field);
      BOOST_CHECK_MESSAGE(!is_tickable_pool(missing), "absent " << field << " must refuse the tick");
   }
}

BOOST_AUTO_TEST_CASE(asset_amount_reads_a_rendered_asset_and_nothing_else) {
   BOOST_CHECK_EQUAL(12 * one_token, asset_amount(mvo()("quantity", "12.000000000 WIRE"), "quantity"));
   BOOST_CHECK_EQUAL(0, asset_amount(mvo()("quantity", "12.000000000 WIRE"), "balance"));   // absent
   BOOST_CHECK_EQUAL(0, asset_amount(mvo()("quantity", 12), "quantity"));                  // not rendered
   BOOST_CHECK_EQUAL(0, asset_amount(mvo()("quantity", "not an asset"), "quantity"));
}

BOOST_AUTO_TEST_CASE(row_symbol_code_reads_the_kv_key_and_renders_it_for_an_action) {
   const auto row = kv(shadow_code, mvo()("quantity", "3.000000000 LIQSOL"));
   const auto code = row_symbol_code(row);
   BOOST_REQUIRE(code.has_value());
   BOOST_CHECK_EQUAL(raw_code(shadow_code), code->value);
   // The symbol_code ABI argument of `tickyield` / `queueyield` is the code's name.
   BOOST_CHECK_EQUAL(std::string(shadow_code), symbol_code_name(*code));
   fc::variant rendered;
   fc::to_variant(*code, rendered);
   BOOST_CHECK_EQUAL(std::string(shadow_code), rendered.as_string());

   BOOST_CHECK(!row_symbol_code(mvo()("value", mvo())).has_value());              // no key
   BOOST_CHECK(!row_symbol_code(mvo()("key", "0a0b")("value", mvo())).has_value()); // undecoded key
}

BOOST_AUTO_TEST_CASE(row_value_unwraps_the_kv_wrapper) {
   const auto row   = kv(pool_code, mvo()("balance", mvo()("quantity", "2.000000000 LIQSOL")("contract", "sysio.liq")));
   const auto value = row_value(row);
   BOOST_REQUIRE(value.has_value());
   BOOST_CHECK_EQUAL(2 * one_token, asset_amount((*value)["balance"].get_object(), "quantity"));
   BOOST_CHECK(!row_value(mvo()("key", mvo())).has_value());
}

BOOST_AUTO_TEST_CASE(kick_settings_read_the_config_and_refuse_what_setconfig_would) {
   for (const uint32_t window : {short_twap_window_sec, production_twap_window_sec}) {
      const auto read = read_kick_settings(kick_config_row(7, window));
      BOOST_REQUIRE(read.has_value());
      BOOST_CHECK_EQUAL(fc::seconds(configured_min_interval_sec).count(), read->min_interval.count());
      BOOST_CHECK_EQUAL(fc::seconds(window).count(), read->twap_window.count());
      BOOST_CHECK_EQUAL(7u, read->budget_remaining);
   }

   BOOST_CHECK(!read_kick_settings(mvo()("budget_remaining", 0)).has_value());                       // absent fields
   BOOST_CHECK(!read_kick_settings(kick_config_row(0, production_twap_window_sec, 0)).has_value());   // refused 0
   BOOST_CHECK(!read_kick_settings(kick_config_row(0, short_twap_window_sec - 1)).has_value());       // refused < 60
   BOOST_CHECK(!read_kick_settings(mvo()("budget_remaining", 0)("min_interval_sec", "not a number")
                                      ("twap_window_sec", 60)).has_value());
}

/// The rows reach the crank as `abi_serializer` decodes them in-process: a uint128 column is a uint128 variant, on
/// which `as_uint64` throws. The crank reads it, and the HTTP API's decimal-string rendering, alike.
BOOST_AUTO_TEST_CASE(kick_fields_read_as_the_node_renders_them) {
   const auto pool = kick_pool_row(fc::time_point::from_iso_string("2026-10-10T12:00:00.000"));
   BOOST_REQUIRE(pool["gifted_total"].is_uint128());
   BOOST_CHECK_THROW(pool["gifted_total"].as_uint64(), fc::exception);
   BOOST_CHECK_EQUAL(5u, *kick_detail::unsigned_field(pool, "gifted_total"));
   BOOST_CHECK_EQUAL(5u, *kick_detail::unsigned_field(mvo()("gifted_total", "5"), "gifted_total"));
   // A value past 64 bits is refused, not truncated.
   const fc::uint128 wide = fc::uint128(std::numeric_limits<uint64_t>::max()) + 1;
   BOOST_CHECK(!kick_detail::unsigned_field(mvo()("gifted_total", fc::variant(wide)), "gifted_total").has_value());
   BOOST_CHECK(!kick_detail::unsigned_field(mvo()("gifted_total", -1), "gifted_total").has_value());
   BOOST_CHECK_EQUAL(std::string(shadow_code), pool["sym"].as_string());

   // Review of #675: `spent_today` reaches the crank the same way. Read with `as_uint64` it threw, every pool read
   // as not due, and the crank never pushed a kick on a live node.
   const auto at   = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   const auto paid = kick_pool_row(at - fc::days(2), at - fc::days(2), utc_day(at), one_token);
   BOOST_REQUIRE(paid["spent_today"].is_uint128());
   BOOST_CHECK_THROW(paid["spent_today"].as_uint64(), fc::exception);
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(one_token), *kick_detail::unsigned_field(paid, "spent_today"));
   BOOST_CHECK(kick_due(paid, settings(), supplies(), at));
}

BOOST_AUTO_TEST_CASE(kick_is_due_only_past_the_contract_clock_check) {
   const auto last_kick    = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   const auto min_interval = fc::seconds(configured_min_interval_sec);
   const auto pool = kick_pool_row(last_kick);
   const auto cfg  = settings();

   // `kick` returns without writing while `now - last_kick < min_interval`.
   BOOST_CHECK(!kick_due(pool, cfg, supplies(), last_kick));
   BOOST_CHECK(!kick_due(pool, cfg, supplies(), last_kick + min_interval - fc::microseconds(1)));
   BOOST_CHECK(kick_due(pool, cfg, supplies(), last_kick + min_interval));
   BOOST_CHECK(kick_due(pool, cfg, supplies(), last_kick + fc::days(3)));   // a long-unpaid pool stays due
   // A clock ahead of this node's (read from a later block) is never due.
   BOOST_CHECK(!kick_due(pool, cfg, supplies(), last_kick - fc::seconds(1)));
}

/// Review of #675: one hour after a payment the minimum interval had passed, but the contract could not pay for
/// the rest of the TWAP window -- every ACTIVE operator pushed an accepted no-op kick per pool per retry interval
/// (about 1,380 a day each at the defaults). The observation restarts at every payment, so `price_time` gates it.
BOOST_AUTO_TEST_CASE(kick_waits_for_the_twap_observation_after_a_payment) {
   const auto paid_at = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   const auto pool    = kick_pool_row(paid_at, paid_at, utc_day(paid_at), one_token);
   const auto cfg     = settings();
   const auto window  = fc::seconds(production_twap_window_sec);

   BOOST_CHECK(!kick_due(pool, cfg, supplies(), paid_at + fc::seconds(configured_min_interval_sec)));
   BOOST_CHECK(!kick_due(pool, cfg, supplies(), paid_at + window - fc::microseconds(1)));
   BOOST_CHECK(kick_due(pool, cfg, supplies(), paid_at + window));
   // The window is read from the config, not assumed.
   BOOST_CHECK(kick_due(pool, settings(1000 * one_token, short_twap_window_sec), supplies(),
                        paid_at + fc::seconds(configured_min_interval_sec)));
   // A partial payment advances `last_kick` only pro rata, but restarts the observation all the same.
   const auto partial = kick_pool_row(paid_at - fc::days(2), paid_at, utc_day(paid_at), 1000 * one_token);
   BOOST_CHECK(!kick_due(partial, cfg, supplies(), paid_at + fc::hours(1)));
}

BOOST_AUTO_TEST_CASE(kick_is_not_due_while_the_budget_or_the_day_cannot_pay_the_minimum) {
   // Noon, so a few hours later is the same UTC day.
   const auto start = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   const auto now   = start + fc::hours(3);
   const auto today = utc_day(now);
   const auto old   = start - fc::days(2);

   // The budget: `kick` pays nothing below `min_gift`.
   BOOST_CHECK(kick_due(kick_pool_row(old, old), settings(one_token), supplies(), now));
   BOOST_CHECK(!kick_due(kick_pool_row(old, old), settings(one_token - 1), supplies(), now));
   BOOST_CHECK(!kick_due(kick_pool_row(old, old), settings(0), supplies(), now));

   // This UTC day's ceiling: spent to within less than `min_gift`, nothing more is paid today ...
   const uint64_t cap = 10 * one_token;
   BOOST_CHECK(kick_due(kick_pool_row(old, old, today, cap - one_token, cap), settings(), supplies(), now));
   BOOST_CHECK(!kick_due(kick_pool_row(old, old, today, cap - one_token + 1, cap), settings(), supplies(), now));
   BOOST_CHECK(!kick_due(kick_pool_row(old, old, today, cap, cap), settings(), supplies(), now));
   // ... and spending recorded on an earlier day does not count.
   BOOST_CHECK(kick_due(kick_pool_row(old, old, today - 1, cap, cap), settings(), supplies(), now));
   BOOST_CHECK(kick_due(kick_pool_row(old, old, today, cap, cap), settings(), supplies(),
                        fc::time_point(fc::seconds(static_cast<int64_t>(today + 1) * day_seconds))));
}

/// Review of #675: on a zero LIQ supply `kick` restarts the clock without paying, which looked to the crank like a
/// payment and restarted its backoff -- a write per pool per `min_interval_sec` per operator while nobody holds the
/// token. A zero (or unknown) supply is never due.
BOOST_AUTO_TEST_CASE(kick_is_not_due_while_the_token_has_no_supply) {
   const auto old = fc::time_point::from_iso_string("2026-10-08T12:00:00.000");
   const auto now = old + fc::days(2);
   BOOST_CHECK(kick_due(kick_pool_row(old), settings(), supplies(1), now));
   BOOST_CHECK(!kick_due(kick_pool_row(old), settings(), supplies(0), now));
   BOOST_CHECK(!kick_due(kick_pool_row(old), settings(), liq_supplies{}, now));            // no stat row
   BOOST_CHECK(!kick_due(kick_pool_row(old, "LIQETH"), settings(), supplies(), now));     // another token's

   // The supplies come from `sysio.liq::stat` rows as the node renders them; an unreadable row is left out.
   const auto read = read_liq_supplies({liq_stat_row(liq_asset(2 * one_token)), liq_stat_row(liq_asset(0, "LIQETH")),
                                        fc::variant(mvo()("supply", 7))});
   BOOST_CHECK_EQUAL(2u, read.size());
   BOOST_CHECK_EQUAL(2 * one_token, read.at(shadow_code));
   BOOST_CHECK_EQUAL(0, read.at("LIQETH"));
}

BOOST_AUTO_TEST_CASE(kick_row_fields_parse_as_rendered_and_a_missing_one_is_never_due) {
   const auto last_kick = fc::time_point::from_iso_string("2026-10-10T12:00:00.500");
   const auto parsed    = kick_last_kick(kick_pool_row(last_kick));
   BOOST_REQUIRE(parsed.has_value());
   BOOST_CHECK(*parsed == last_kick);
   const auto later = last_kick + fc::days(3);
   BOOST_REQUIRE(kick_due(kick_pool_row(last_kick), settings(), supplies(), later));

   for (const auto* field : {"last_kick", "price_time", "min_gift", "max_gift_per_day", "day", "spent_today", "sym"}) {
      BOOST_TEST_CONTEXT(field) {
         auto missing = kick_pool_row(last_kick);
         missing.erase(field);
         BOOST_CHECK(!kick_due(missing, settings(), supplies(), later));
         auto garbled = kick_pool_row(last_kick);
         garbled.set(field, mvo()("not", "a value"));
         BOOST_CHECK(!kick_due(garbled, settings(), supplies(), later));
      }
   }
   BOOST_CHECK(!kick_last_kick(mvo()).has_value());
}

BOOST_AUTO_TEST_CASE(a_due_kick_pool_is_keyed_by_its_symbol_code) {
   // The crank reads `kickpools` with its kv keys: the key is the `kick` argument.
   const auto last_kick = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   const auto row       = kv(shadow_code, kick_pool_row(last_kick));
   const auto code      = row_symbol_code(row);
   const auto value     = row_value(row);
   BOOST_REQUIRE(code.has_value());
   BOOST_REQUIRE(value.has_value());
   BOOST_CHECK_EQUAL(std::string(shadow_code), symbol_code_name(*code));
   BOOST_CHECK(kick_due(*value, settings(), supplies(), last_kick + fc::days(1)));
}

/// The crank's pass over the pool rows: due pools are pushed once, a pool that is not due or still backing off is
/// not, and a shutdown stops the pass.
BOOST_AUTO_TEST_CASE(kick_rows_pushes_each_due_pool_once_and_stops_on_shutdown) {
   const auto last_kick = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   const auto now       = last_kick + fc::days(1);
   const fc::variants rows{fc::variant(kv(shadow_code, kick_pool_row(last_kick))),
                           fc::variant(kv("LIQETH", kick_pool_row(last_kick, "LIQETH"))),   // no supply
                           fc::variant(kv(pool_code, kick_pool_row(now, pool_code)))};       // clock not due
   const liq_supplies stocked{{shadow_code, one_token}, {pool_code, one_token}};
   kick_backoff backoff;
   std::vector<std::string> pushed;
   const auto push  = [&](sysio::chain::symbol_code code) { pushed.push_back(symbol_code_name(code)); };
   const auto clock = [&] { return now; };
   const auto base = fc::seconds(60), ceiling = fc::hours(1);

   BOOST_CHECK(kick_rows(rows, settings(), stocked, backoff, base, ceiling, push, clock, [] { return false; }));
   BOOST_REQUIRE_EQUAL(1u, pushed.size());
   BOOST_CHECK_EQUAL(std::string(shadow_code), pushed.front());
   // The same poll again: the attempt is backing off.
   BOOST_CHECK(kick_rows(rows, settings(), stocked, backoff, base, ceiling, push, clock, [] { return false; }));
   BOOST_CHECK_EQUAL(1u, pushed.size());

   kick_backoff fresh;
   BOOST_CHECK(!kick_rows(rows, settings(), stocked, fresh, base, ceiling, push, clock, [] { return true; }));
   BOOST_CHECK_EQUAL(1u, pushed.size());
   BOOST_CHECK(fresh.due(shadow_code, last_kick, now));   // nothing was marked
}

/// What the rows cannot show (an Andon hold, a gift below `min_gift`) leaves `last_kick` alone: each unpaid
/// attempt doubles the wait to the ceiling, and a moved clock -- a payment by anyone -- starts over.
BOOST_AUTO_TEST_CASE(kick_backoff_doubles_while_unpaid_and_resets_when_the_clock_moves) {
   const auto base      = fc::seconds(60);
   const auto ceiling   = fc::hours(1);
   const auto last_kick = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   auto       now       = last_kick + fc::days(1);
   kick_backoff backoff;

   BOOST_CHECK(backoff.due(shadow_code, last_kick, now));
   backoff.mark(shadow_code, last_kick, now, base, ceiling);
   BOOST_CHECK_EQUAL(base.count(), backoff.wait(shadow_code).count());
   BOOST_CHECK(!backoff.due(shadow_code, last_kick, now + base - fc::microseconds(1)));
   BOOST_CHECK(backoff.due(pool_code, last_kick, now));   // keys are independent

   uint32_t attempts = 1;
   auto     expected = base;
   while (expected < ceiling) {
      now += backoff.wait(shadow_code);
      BOOST_REQUIRE(backoff.due(shadow_code, last_kick, now));
      backoff.mark(shadow_code, last_kick, now, base, ceiling);
      ++attempts;
      expected = std::min(expected + expected, ceiling);
      BOOST_CHECK_EQUAL(expected.count(), backoff.wait(shadow_code).count());
   }
   BOOST_CHECK_EQUAL(7u, attempts);   // waits of 1, 2, 4, 8, 16 and 32 minutes, then the hour
   backoff.mark(shadow_code, last_kick, now + ceiling, base, ceiling);
   BOOST_CHECK_EQUAL(ceiling.count(), backoff.wait(shadow_code).count());

   // A payment moved `last_kick`: due at once, and the next unpaid attempt waits only `base` again.
   const auto paid = last_kick + fc::days(1);
   BOOST_CHECK(backoff.due(shadow_code, paid, now + ceiling + fc::seconds(1)));
   backoff.mark(shadow_code, paid, now + ceiling + fc::seconds(1), base, ceiling);
   BOOST_CHECK_EQUAL(base.count(), backoff.wait(shadow_code).count());
}

/// Persistent nonpayment for a day: at the old fixed one-minute spacing an operator pushed 1,440 kicks per pool;
/// the backoff pushes a few dozen.
BOOST_AUTO_TEST_CASE(kick_backoff_bounds_a_day_of_unpaid_attempts) {
   const auto base      = fc::seconds(60);
   const auto ceiling   = fc::hours(1);
   const auto last_kick = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   const auto start     = last_kick + fc::days(1);
   kick_backoff backoff;
   uint32_t     pushes = 0;
   for (auto now = start; now < start + fc::days(1); now += fc::seconds(15)) {   // the default poll
      if (!backoff.due(shadow_code, last_kick, now)) continue;
      backoff.mark(shadow_code, last_kick, now, base, ceiling);
      ++pushes;
   }
   BOOST_CHECK_LE(pushes, 32u);
   BOOST_CHECK_GE(pushes, 24u);
}

BOOST_AUTO_TEST_CASE(crank_spacing_allows_one_push_per_interval_per_key) {
   crank_spacing spacing;
   const auto interval = fc::seconds(60);
   const auto t0       = fc::time_point::now();

   BOOST_CHECK(spacing.due(pool_code, t0, interval));
   spacing.mark(pool_code, t0);
   BOOST_CHECK(!spacing.due(pool_code, t0 + fc::seconds(59), interval));
   BOOST_CHECK(spacing.due(pool_code, t0 + fc::seconds(60), interval));
   // Keys are independent: a tick on one pool does not delay another.
   BOOST_CHECK(spacing.due(shadow_code, t0, interval));
}

/// A multi-page sweep retains all live pools and drops retired retry keys only on completion.
BOOST_AUTO_TEST_CASE(kick_scan_pages_fairly_and_prunes_removed_pools) {
   static_assert(kicker::pools_per_poll == 10);
   constexpr auto next_page = "{\"symbol_code\":123}";
   constexpr auto retired = "RETIRED";
   const auto now       = fc::time_point::now();
   const auto last_kick = now - fc::days(1);
   const auto interval  = fc::seconds(60);
   kick_backoff backoff;
   kick_pool_scan scan;
   backoff.mark(pool_code, last_kick, now, interval, interval);
   backoff.mark(shadow_code, last_kick, now, interval, interval);
   backoff.mark(retired, last_kick, now, interval, interval);
   BOOST_CHECK(scan.lower_bound().empty());
   scan.observe(pool_code);
   scan.finish_page(true, next_page, backoff);
   BOOST_CHECK_EQUAL(next_page, scan.lower_bound());
   BOOST_CHECK(!backoff.due(retired, last_kick, now));
   scan.observe(shadow_code);
   scan.finish_page(false, {}, backoff);
   BOOST_CHECK(scan.lower_bound().empty());
   BOOST_CHECK(!backoff.due(pool_code, last_kick, now));
   BOOST_CHECK(!backoff.due(shadow_code, last_kick, now));
   BOOST_CHECK(backoff.due(retired, last_kick, now));
   // An empty table prunes everything; re-added symbols can be attempted immediately.
   scan.finish_page(false, {}, backoff);
   BOOST_CHECK(backoff.due(pool_code, last_kick, now));
   BOOST_CHECK(backoff.due(shadow_code, last_kick, now));
}

/// Review of #675: `read_table` turned a failed read into an empty last page, so finishing the page after a failure
/// restarted the sweep and pruned the retry markers of every pool past the failed page. The crank now skips
/// `finish_page` when the read fails; the next pass rereads the same page and the sweep completes from there.
BOOST_AUTO_TEST_CASE(kick_scan_survives_a_failed_middle_page) {
   constexpr auto second_page = "{\"symbol_code\":2}";
   constexpr auto tail        = "TAIL";
   const auto now       = fc::time_point::now();
   const auto last_kick = now - fc::days(1);
   const auto interval  = fc::seconds(60);

   // What a failure taken as completion did: the cursor restarts and the tail pool's marker is gone.
   {
      kick_backoff backoff;
      kick_pool_scan scan;
      backoff.mark(tail, last_kick, now, interval, interval);
      scan.observe(pool_code);
      scan.finish_page(true, second_page, backoff);
      scan.finish_page(false, {}, backoff);   // the failed read's default result: no rows, no `more`
      BOOST_CHECK(scan.lower_bound().empty());
      BOOST_CHECK(backoff.due(tail, last_kick, now));
   }

   kick_backoff backoff;
   kick_pool_scan scan;
   backoff.mark(tail, last_kick, now, interval, interval);
   scan.observe(pool_code);
   scan.finish_page(true, second_page, backoff);
   // Page two fails: nothing is finished, so the cursor still names page two and the tail keeps its marker.
   BOOST_CHECK_EQUAL(second_page, scan.lower_bound());
   BOOST_CHECK(!backoff.due(tail, last_kick, now));
   // The next pass reads page two and completes the sweep: pools on both pages survive the prune.
   scan.observe(tail);
   scan.finish_page(false, {}, backoff);
   BOOST_CHECK(scan.lower_bound().empty());
   BOOST_CHECK(!backoff.due(tail, last_kick, now));
}

namespace {
/// A kickpools page as `read_table_checked` returns it.
struct page_t {
   fc::variants rows;
   bool         more = false;
   std::string  next_key;
};

/// A two-page kickpools table (LIQSOL, then POOLB) behind the crank's two readers, with switchable failures. Each
/// stat lookup returns that token's row as the node renders it, and records which token was looked up.
struct paged_table {
   static constexpr auto second_page = "{\"symbol_code\":2}";
   fc::time_point             last_kick = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   bool                       fail_page = false;
   bool                       fail_stat = false;
   std::vector<std::string>   reads, lookups;

   std::optional<page_t> read_page(const std::string& lower_bound) {
      reads.push_back(lower_bound);
      if (fail_page) return std::nullopt;
      if (lower_bound.empty())
         return page_t{{fc::variant(kv(shadow_code, kick_pool_row(last_kick)))}, true, second_page};
      return page_t{{fc::variant(kv(pool_code, kick_pool_row(last_kick, pool_code)))}, false, {}};
   }
   std::optional<fc::variants> read_stat(sysio::chain::symbol_code code) {
      const auto name = symbol_code_name(code);
      lookups.push_back(name);
      if (fail_stat) return std::nullopt;
      return fc::variants{liq_stat_row(liq_asset(one_token, name.c_str()))};
   }
};
} // namespace

/// The crank's paged poll: a failed page read finishes nothing and pushes nothing, and the next poll rereads that
/// page and completes the sweep.
BOOST_AUTO_TEST_CASE(kick_page_finishes_nothing_on_a_failed_read) {
   paged_table table;
   const auto now  = table.last_kick + fc::days(1);
   const auto base = fc::seconds(60), ceiling = fc::hours(1);
   kick_backoff   backoff;
   kick_pool_scan scan;
   backoff.mark("TAIL", table.last_kick, now, base, ceiling);
   std::vector<std::string> pushed;
   const auto run = [&] {
      return kick_page(scan, backoff, settings(), base, ceiling,
                       [&](const std::string& lower_bound) { return table.read_page(lower_bound); },
                       [&](sysio::chain::symbol_code code) { return table.read_stat(code); },
                       [&](sysio::chain::symbol_code code) { pushed.push_back(symbol_code_name(code)); },
                       [&] { return now; }, [] { return false; });
   };

   BOOST_CHECK(run());   // page one
   BOOST_CHECK_EQUAL(paged_table::second_page, scan.lower_bound());
   table.fail_page = true;
   BOOST_CHECK(!run());  // page two fails
   BOOST_CHECK_EQUAL(paged_table::second_page, scan.lower_bound());
   BOOST_CHECK(!backoff.due("TAIL", table.last_kick, now));   // nothing pruned
   BOOST_CHECK_EQUAL(1u, pushed.size());
   table.fail_page = false;
   BOOST_CHECK(run());   // page two again: the sweep completes
   BOOST_CHECK(std::vector<std::string>({"", paged_table::second_page, paged_table::second_page}) == table.reads);
   BOOST_CHECK(scan.lower_bound().empty());
   BOOST_CHECK(std::vector<std::string>({shadow_code, pool_code}) == pushed);
   BOOST_CHECK(backoff.due("TAIL", table.last_kick, now));    // retired only once the sweep was complete
}

/// Review of #675: the supplies came from a full `sysio.liq::stat` scan every poll, unbounded by the ten-pool page.
/// Now only the page's own tokens are looked up, one exact read each, and a failed lookup ends the poll the way a
/// failed page read does: nothing pushed, nothing finished.
BOOST_AUTO_TEST_CASE(kick_page_looks_up_only_its_own_tokens_and_stops_on_a_failed_lookup) {
   paged_table table;
   const auto now  = table.last_kick + fc::days(1);
   const auto base = fc::seconds(60), ceiling = fc::hours(1);
   kick_backoff   backoff;
   kick_pool_scan scan;
   std::vector<std::string> pushed;
   const auto run = [&] {
      return kick_page(scan, backoff, settings(), base, ceiling,
                       [&](const std::string& lower_bound) { return table.read_page(lower_bound); },
                       [&](sysio::chain::symbol_code code) { return table.read_stat(code); },
                       [&](sysio::chain::symbol_code code) { pushed.push_back(symbol_code_name(code)); },
                       [&] { return now; }, [] { return false; });
   };

   table.fail_stat = true;
   BOOST_CHECK(!run());
   BOOST_CHECK(pushed.empty());
   BOOST_CHECK(scan.lower_bound().empty());   // page one again next poll
   BOOST_CHECK(std::vector<std::string>({shadow_code}) == table.lookups);

   table.fail_stat = false;
   table.lookups.clear();
   BOOST_CHECK(run());
   BOOST_CHECK(std::vector<std::string>({shadow_code}) == table.lookups);   // page one's token only
   BOOST_CHECK(run());
   BOOST_CHECK(std::vector<std::string>({shadow_code, pool_code}) == table.lookups);
   BOOST_CHECK(std::vector<std::string>({shadow_code, pool_code}) == pushed);

   // A token with no stat row has no supply: it is never due, and the page still finishes.
   const auto page = table.read_page({});
   BOOST_REQUIRE(page.has_value());
   const auto none = read_page_supplies(page->rows, [](sysio::chain::symbol_code) {
      return std::optional<fc::variants>(fc::variants{});
   });
   BOOST_REQUIRE(none.has_value());
   BOOST_CHECK(none->empty());
}

/// Review of #675: `--batch-kick-interval-ms` is the backoff's first wait. Zero would double to zero forever and
/// retry an unpaid kick every poll; more than the ceiling would exceed it on the first wait.
BOOST_AUTO_TEST_CASE(kick_interval_must_be_positive_and_within_the_retry_ceiling) {
   const auto ceiling_ms = static_cast<uint32_t>(kicker::max_retry_wait.count() / fc::milliseconds(1).count());
   BOOST_CHECK_EQUAL(3'600'000u, ceiling_ms);
   BOOST_CHECK(!kick_interval_valid(0));
   BOOST_CHECK(kick_interval_valid(1));
   BOOST_CHECK(kick_interval_valid(60'000));   // the default
   BOOST_CHECK(kick_interval_valid(ceiling_ms));
   BOOST_CHECK(!kick_interval_valid(ceiling_ms + 1));
}

BOOST_AUTO_TEST_SUITE_END()
