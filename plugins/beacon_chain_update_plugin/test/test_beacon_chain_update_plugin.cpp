#include <boost/test/unit_test.hpp>

#include <fc/io/json.hpp>
#include <fc/time.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/beacon_chain_update_detail.hpp>

using namespace sysio::beacon_chain_detail;

BOOST_AUTO_TEST_SUITE(beacon_chain_update_detail_tests)

// ---------------------------------------------------------------------------
// get_field_from_object
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(get_field_from_object_non_object_returns_empty) {
   auto v = fc::variant("not an object");
   BOOST_CHECK(!get_field_from_object(v, "key").has_value());
}

BOOST_AUTO_TEST_CASE(get_field_from_object_null_returns_empty) {
   fc::variant v;
   BOOST_CHECK(!get_field_from_object(v, "key").has_value());
}

BOOST_AUTO_TEST_CASE(get_field_from_object_missing_field_returns_empty) {
   auto v = fc::json::from_string(R"({"other": 1})");
   BOOST_CHECK(!get_field_from_object(v, "missing").has_value());
}

BOOST_AUTO_TEST_CASE(get_field_from_object_present_string_field) {
   auto v = fc::json::from_string(R"({"name": "hello"})");
   auto result = get_field_from_object(v, "name");
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_EQUAL(result->as_string(), "hello");
}

BOOST_AUTO_TEST_CASE(get_field_from_object_present_numeric_field) {
   auto v = fc::json::from_string(R"({"count": 42})");
   auto result = get_field_from_object(v, "count");
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_EQUAL(result->as_uint64(), 42u);
}

BOOST_AUTO_TEST_CASE(get_field_from_object_nested_object_field) {
   auto v = fc::json::from_string(R"({"inner": {"x": 7}})");
   auto result = get_field_from_object(v, "inner");
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK(result->is_object());
}

// ---------------------------------------------------------------------------
// get_queue_length
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(get_queue_length_missing_branch_throws) {
   auto queues = fc::json::from_string(R"({"other_queue": {"estimated_processed_at": 9999999999}})");
   BOOST_CHECK_THROW(get_queue_length(queues, "exit_queue"), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(get_queue_length_missing_epa_field_throws) {
   auto queues = fc::json::from_string(R"({"exit_queue": {"some_other_field": 123}})");
   BOOST_CHECK_THROW(get_queue_length(queues, "exit_queue"), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(get_queue_length_non_numeric_epa_throws) {
   auto queues = fc::json::from_string(R"({"exit_queue": {"estimated_processed_at": "not-a-number"}})");
   BOOST_CHECK_THROW(get_queue_length(queues, "exit_queue"), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(get_queue_length_branch_not_object_throws) {
   // branch present but is a scalar, not an object — get_field_from_object on it will return empty
   auto queues = fc::json::from_string(R"({"exit_queue": 12345})");
   BOOST_CHECK_THROW(get_queue_length(queues, "exit_queue"), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(get_queue_length_valid_returns_eta) {
   // Use a far-future epoch (year 2100) to guarantee epa > now_sec for the duration of any test run
   constexpr uint64_t far_future_epa = 4102444800ull; // 2100-01-01 00:00:00 UTC
   auto queues_str = std::string(R"({"exit_queue": {"estimated_processed_at": )") +
                     std::to_string(far_future_epa) + "}}";
   auto queues = fc::json::from_string(queues_str);
   auto result = get_queue_length(queues, "exit_queue");
   BOOST_REQUIRE(result.has_value());
   // The ETA should be a large positive number (many seconds until year 2100)
   BOOST_CHECK_GT(*result, uint64_t{0});
   // Sanity: eta must be less than far_future_epa itself (since now_sec > 0)
   BOOST_CHECK_LT(*result, far_future_epa);
}

BOOST_AUTO_TEST_CASE(get_queue_length_deposit_queue_branch) {
   constexpr uint64_t far_future_epa = 4102444800ull;
   auto queues_str = std::string(R"({"deposit_queue": {"estimated_processed_at": )") +
                     std::to_string(far_future_epa) + "}}";
   auto queues = fc::json::from_string(queues_str);
   auto result = get_queue_length(queues, "deposit_queue");
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_GT(*result, uint64_t{0});
}

// ---------------------------------------------------------------------------
// apy_fraction_to_bps
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_zero) {
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(0.0), 0u);
}

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_five_percent) {
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(0.05), 500u);
}

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_one_hundred_percent) {
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(1.0), 10000u);
}

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_three_point_four_two_percent) {
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(0.0342), 342u);
}

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_twelve_point_three_four_percent) {
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(0.1234), 1234u);
}

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_epsilon_robustness) {
   // 0.03 * 10000 may produce 299.9999... in floating point without the epsilon guard
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(0.03), 300u);
}

BOOST_AUTO_TEST_SUITE_END()
