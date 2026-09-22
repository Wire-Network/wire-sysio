#include <sysio/query_engine_plugin/query.hpp>

#include <boost/test/unit_test.hpp>

using namespace sysio::query_engine;
BOOST_AUTO_TEST_SUITE(query_values)

BOOST_AUTO_TEST_CASE(exact_integer_and_decimal_comparison) {
   const auto large = parse_number("340282366920938463463374607431768211455");
   BOOST_CHECK_EQUAL(render_number(large), "340282366920938463463374607431768211455");
   BOOST_CHECK(compare_values(parse_number("9007199254740993"), parse_number("9007199254740992")) > 0);
   BOOST_CHECK(compare_values(parse_number("-10.1"), parse_number("-10.01")) < 0);
   BOOST_CHECK_EQUAL(compare_values(parse_number("1.0"), parse_number("1")), 0);
   BOOST_CHECK_THROW(parse_number("0.0000000000000000001"), query_error);
   BOOST_CHECK_THROW(parse_number("1e2"), query_error);
}

BOOST_AUTO_TEST_CASE(average_rounds_half_even_only_at_output) {
   auto third = parse_number("1");
   third.denominator = 3;
   BOOST_CHECK_EQUAL(render_number(third), "0.333333333333333333");
   auto half_even = parse_number("1");
   half_even.denominator = integer("2000000000000000000");
   BOOST_CHECK_EQUAL(render_number(half_even), "0");
   half_even.numerator = 3;
   BOOST_CHECK_EQUAL(render_number(half_even), "0.000000000000000002");
   BOOST_CHECK(compare_values(third, parse_number("0.333333333333333333")) > 0);
}

BOOST_AUTO_TEST_CASE(asset_units_and_checked_accumulation) {
   type_descriptor asset;
   asset.primitive = primitive_type::asset;
   asset.logical = logical_type::asset;
   value first;
   first.null = false;
   first.text = "12.3456 SYS";
   first = coerce_literal(first, asset);
   value second;
   second.null = false;
   second.text = "0.6544 SYS";
   second = coerce_literal(second, asset);
   add_value(first, second);
   BOOST_CHECK_EQUAL(render_number(first), "13");
   query_budget budget({});
   const auto cell = to_cell(first, budget);
   BOOST_CHECK_EQUAL(cell["precision"].as_string(), "4");
   second.symbol = "OTHER";
   BOOST_CHECK_THROW(add_value(first, second), query_error);
   auto maximum = parse_number((std::numeric_limits<integer>::max)().convert_to<std::string>());
   BOOST_CHECK_THROW(add_value(maximum, parse_number("1")), query_error);
}

BOOST_AUTO_TEST_CASE(budgets_fail_before_counter_mutation) {
   query_config config;
   config.max_scan_rows = 1;
   query_budget budget(config);
   budget.charge_raw(1, 10);
   BOOST_CHECK_THROW(budget.charge_raw(1, 1), query_error);
   BOOST_CHECK_EQUAL(budget.scanned_rows, 1);
   BOOST_CHECK_EQUAL(budget.raw_bytes, 10);
   budget.cancelled = true;
   BOOST_CHECK_THROW(budget.check(), query_error);
}
BOOST_AUTO_TEST_SUITE_END()
