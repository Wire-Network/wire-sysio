#include <sysio/query_engine_plugin/query.hpp>

#include <boost/test/unit_test.hpp>

#include <limits>

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

/// Released charges return only what was accounted; the raw counters are never touched.
BOOST_AUTO_TEST_CASE(memory_release_is_bounded_by_accounted_charges) {
   query_budget budget({});
   budget.charge_memory(100);
   budget.charge_raw(1, 10);
   budget.release_memory(50);
   BOOST_CHECK_EQUAL(budget.accounted_bytes, 60);
   budget.release_memory(1000);
   BOOST_CHECK_EQUAL(budget.accounted_bytes, 0);
   BOOST_CHECK_EQUAL(budget.raw_bytes, 10);
   BOOST_CHECK_EQUAL(budget.scanned_rows, 1);
}

/// Every int64 microsecond value renders and parses back exactly; the ISO range is not a boundary.
BOOST_AUTO_TEST_CASE(time_round_trips_across_the_full_range) {
   BOOST_CHECK_EQUAL(format_time(0), "1970-01-01T00:00:00");
   BOOST_CHECK_EQUAL(format_time(1700000000123456), "2023-11-14T22:13:20.123456");
   BOOST_CHECK_EQUAL(format_time(1700000000123000), "2023-11-14T22:13:20.123000");
   BOOST_CHECK_EQUAL(format_time(-1), "1969-12-31T23:59:59.999999");
   BOOST_CHECK_EQUAL(format_time(253402300800000000), "+10000-01-01T00:00:00");
   BOOST_CHECK_EQUAL(format_time(std::numeric_limits<int64_t>::max()), "+294247-01-10T04:00:54.775807");
   BOOST_CHECK_EQUAL(format_time(std::numeric_limits<int64_t>::min()), "-290308-12-21T19:59:05.224192");
   for (const auto microseconds :
        {int64_t{0}, int64_t{1700000000123456}, int64_t{-1}, int64_t{253402300800000000},
         std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min(), int64_t{951782400000000}})
      BOOST_CHECK_EQUAL(parse_time(format_time(microseconds)), microseconds);
   BOOST_CHECK_EQUAL(parse_time("2023-11-14T22:13:20.123Z"), 1700000000123000);
   BOOST_CHECK_EQUAL(parse_time("2000-02-29T00:00:00"), 951782400000000);
   for (const auto* invalid :
        {"2023-13-01T00:00:00", "2023-02-29T00:00:00", "2023-11-14T24:00:00", "2023-11-14", "2023-11-14T22:13",
         "2023-11-14T22:13:20.", "2023-11-14T22:13:20.1234567", "123-01-01T00:00:00", "+294248-01-01T00:00:00",
         "2023-11-14 22:13:20", "2023-11-14T22:13:20x"})
      BOOST_CHECK_THROW(parse_time(invalid), query_error);
}

/// Checksum and byte literals bind in the exact lowercase form the decoder renders.
BOOST_AUTO_TEST_CASE(hex_literals_are_canonicalized) {
   type_descriptor checksum;
   checksum.primitive = primitive_type::checksum256;
   const std::string digest = "A0B1C2D3E4F5A6B7C8D9E0F1A2B3C4D5E6F7A8B9C0D1E2F3A4B5C6D7E8F9A0B1";
   value literal;
   literal.null = false;
   literal.text = digest;
   const auto bound = coerce_literal(literal, checksum);
   BOOST_CHECK_EQUAL(bound.text, "a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1");
   for (const auto* invalid : {"ab", "a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1ff",
                               "z0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1"}) {
      literal.text = invalid;
      BOOST_CHECK_THROW(coerce_literal(literal, checksum), query_error);
   }
   type_descriptor bytes;
   bytes.primitive = primitive_type::bytes;
   literal.text = "DEADbeef";
   BOOST_CHECK_EQUAL(coerce_literal(literal, bytes).text, "deadbeef");
   literal.text = "abc";
   BOOST_CHECK_THROW(coerce_literal(literal, bytes), query_error);
}

/// Enum literals resolve by member name, by prefix-stripped member name, or by underlying value.
BOOST_AUTO_TEST_CASE(enum_literals_bind_member_names_and_values) {
   type_descriptor status;
   status.abi_type = "position_status";
   status.logical = logical_type::enumeration;
   status.primitive = primitive_type::uint8;
   status.members = {
      {"POSITION_STATUS_OPEN",     0},
      {"POSITION_STATUS_CLOSED",   1},
      {"POSITION_STATUS_ARCHIVED", 5}
   };
   value literal;
   literal.null = false;
   for (const auto* text : {"POSITION_STATUS_ARCHIVED", "ARCHIVED", "5"}) {
      literal.text = text;
      const auto bound = coerce_literal(literal, status);
      BOOST_CHECK(bound.type == logical_type::enumeration);
      BOOST_CHECK_EQUAL(bound.numerator.convert_to<std::string>(), "5");
      BOOST_CHECK_EQUAL(bound.text, "POSITION_STATUS_ARCHIVED");
   }
   literal.text = "7";
   const auto unlisted = coerce_literal(literal, status);
   BOOST_CHECK_EQUAL(unlisted.text, "7");
   BOOST_CHECK_EQUAL(unlisted.numerator.convert_to<std::string>(), "7");
   literal.text = "MISSING";
   BOOST_CHECK_THROW(coerce_literal(literal, status), query_error);
   literal.text = "1.5";
   BOOST_CHECK_THROW(coerce_literal(literal, status), query_error);
   literal.text = "POSITION_STATUS_CLOSED";
   const auto closed = coerce_literal(literal, status);
   BOOST_CHECK(compare_values(closed, unlisted) < 0);
   BOOST_CHECK(compare_values(closed, parse_number("1")) == 0);
   BOOST_CHECK(comparable(logical_type::enumeration, logical_type::integer));
   BOOST_CHECK(!comparable(logical_type::enumeration, logical_type::text));
   query_budget budget({});
   BOOST_CHECK_EQUAL(to_cell(closed, budget).as_string(), "POSITION_STATUS_CLOSED");
}
BOOST_AUTO_TEST_SUITE_END()
