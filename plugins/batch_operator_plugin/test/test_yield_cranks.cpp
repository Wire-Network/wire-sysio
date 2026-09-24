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
