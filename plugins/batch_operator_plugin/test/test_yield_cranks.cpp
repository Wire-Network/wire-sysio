#include "../src/yield_cranks.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>

#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/symbol.hpp>

using namespace sysio::batch_operator_detail;
using mvo = fc::mutable_variant_object;

namespace {

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

/// A `sysio.kicker::kickpools` value as the chain renders it: the LIQ token, its
/// rate and minimum, and the accrual clock as an ISO time point.
mvo kick_pool_row(fc::time_point last_kick) {
   return mvo()
      ("sym",              shadow_code)
      ("rate_bps",         200)
      ("min_gift",         one_token)
      ("last_kick",        last_kick.to_iso_string())
      ("gifted_total",     "0")
      ("shortfall_amount", 0)
      ("max_gift_per_day", 0);
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

BOOST_AUTO_TEST_CASE(kick_min_interval_reads_the_config_and_refuses_what_setconfig_would) {
   const auto interval = kick_min_interval(mvo()("budget_remaining", 0)("min_interval_sec", 3600));
   BOOST_REQUIRE(interval.has_value());
   BOOST_CHECK_EQUAL(fc::seconds(3600).count(), interval->count());

   BOOST_CHECK(!kick_min_interval(mvo()("budget_remaining", 0)).has_value());   // absent
   BOOST_CHECK(!kick_min_interval(mvo()("min_interval_sec", 0)).has_value());   // setconfig refuses 0
   BOOST_CHECK(!kick_min_interval(mvo()("min_interval_sec", "not a number")).has_value());
}

BOOST_AUTO_TEST_CASE(kick_is_due_exactly_when_the_contract_clock_check_passes) {
   const auto last_kick    = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   const auto min_interval = fc::seconds(3600);
   const auto pool         = kick_pool_row(last_kick);

   // `kick` returns without writing while `now - last_kick < min_interval`.
   BOOST_CHECK(!kick_due(pool, min_interval, last_kick));
   BOOST_CHECK(!kick_due(pool, min_interval, last_kick + min_interval - fc::microseconds(1)));
   BOOST_CHECK(kick_due(pool, min_interval, last_kick + min_interval));
   BOOST_CHECK(kick_due(pool, min_interval, last_kick + fc::days(3)));   // a long-unpaid pool stays due
   // A clock ahead of this node's (read from a later block) is never due.
   BOOST_CHECK(!kick_due(pool, min_interval, last_kick - fc::seconds(1)));
}

BOOST_AUTO_TEST_CASE(kick_last_kick_parses_the_rendered_time_point_and_nothing_else) {
   const auto last_kick = fc::time_point::from_iso_string("2026-10-10T12:00:00.500");
   const auto parsed    = kick_last_kick(kick_pool_row(last_kick));
   BOOST_REQUIRE(parsed.has_value());
   BOOST_CHECK(*parsed == last_kick);

   auto missing = kick_pool_row(last_kick);
   missing.erase("last_kick");
   BOOST_CHECK(!kick_last_kick(missing).has_value());
   BOOST_CHECK(!kick_due(missing, fc::seconds(1), last_kick + fc::days(1)));   // unreadable row: no push

   auto garbled = kick_pool_row(last_kick);
   garbled.set("last_kick", mvo()("not", "a time"));
   BOOST_CHECK(!kick_last_kick(garbled).has_value());
   BOOST_CHECK(!kick_due(garbled, fc::seconds(1), last_kick + fc::days(1)));
}

BOOST_AUTO_TEST_CASE(a_due_kick_pool_is_keyed_and_spaced_like_the_yield_cranks) {
   // The crank reads `kickpools` with its kv keys: the key is the `kick` argument.
   const auto last_kick = fc::time_point::from_iso_string("2026-10-10T12:00:00.000");
   const auto row       = kv(shadow_code, kick_pool_row(last_kick));
   const auto code      = row_symbol_code(row);
   const auto value     = row_value(row);
   BOOST_REQUIRE(code.has_value());
   BOOST_REQUIRE(value.has_value());
   BOOST_CHECK_EQUAL(std::string(shadow_code), symbol_code_name(*code));

   // An Andon hold or a below-minimum gift leaves `last_kick` alone, so the pool
   // stays due; the spacing is what keeps the retries to one per interval.
   const auto min_interval = fc::seconds(3600);
   const auto retry        = fc::seconds(60);
   const auto now          = last_kick + min_interval;
   const auto pool         = symbol_code_name(*code);
   crank_spacing spacing;
   BOOST_REQUIRE(kick_due(*value, min_interval, now));
   BOOST_CHECK(spacing.due(pool, now, retry));
   spacing.mark(pool, now);
   BOOST_CHECK(kick_due(*value, min_interval, now + fc::seconds(15)));
   BOOST_CHECK(!spacing.due(pool, now + fc::seconds(15), retry));
   BOOST_CHECK(spacing.due(pool, now + retry, retry));
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

BOOST_AUTO_TEST_SUITE_END()
