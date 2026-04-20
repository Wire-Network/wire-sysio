#include <boost/test/unit_test.hpp>

#include <cmath>
#include <limits>

#include <fc/io/json.hpp>
#include <fc/time.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/beacon_chain_update_detail.hpp>
#include <sysio/beacon_chain_config_updates.hpp>

using namespace sysio::beacon_chain_detail;
using namespace sysio;

namespace {
   // A long-horizon EPA used by tests that only need a value "far enough in the future" to
   // produce a positive ETA. Picked for readability, not precision; test assertions must not
   // depend on the exact magnitude. Update before 2100-01-01 (the wall-clock value).
   constexpr uint64_t far_future_epa = 4102444800ull; // 2100-01-01 00:00:00 UTC

   fc::variant make_queue(const char* branch_name) {
      auto json = std::string("{\"") + branch_name + R"(": {"estimated_processed_at": )" +
                  std::to_string(far_future_epa) + "}}";
      return fc::json::from_string(json);
   }
}

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
   auto queues = fc::json::from_string(R"({"exit_queue": 12345})");
   BOOST_CHECK_THROW(get_queue_length(queues, "exit_queue"), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(get_queue_length_valid_returns_eta) {
   auto result = get_queue_length(make_queue("exit_queue"), "exit_queue");
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_GT(*result, uint64_t{0});
   // eta must be less than the raw epoch (now_sec > 0, so delta < epa)
   BOOST_CHECK_LT(*result, far_future_epa);
}

BOOST_AUTO_TEST_CASE(get_queue_length_deposit_queue_branch) {
   auto result = get_queue_length(make_queue("deposit_queue"), "deposit_queue");
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_GT(*result, uint64_t{0});
}

BOOST_AUTO_TEST_CASE(get_queue_length_past_epa_returns_empty) {
   // epa=1 is in the past; should return nullopt rather than wrapping
   auto queues = fc::json::from_string(R"({"exit_queue": {"estimated_processed_at": 1}})");
   auto result = get_queue_length(queues, "exit_queue");
   BOOST_CHECK(!result.has_value());
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

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_negative_clamped_to_zero) {
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(-0.05), 0u);
}

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_nan_clamped_to_zero) {
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(std::nan("")), 0u);
}

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_inf_clamped_to_zero) {
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(std::numeric_limits<double>::infinity()), 0u);
}

BOOST_AUTO_TEST_CASE(apy_fraction_to_bps_extremely_large_clamped) {
   // Wave 2 caps fraction at 100.0 (= 1,000,000 bps) before the uint64_t cast.
   BOOST_CHECK_EQUAL(apy_fraction_to_bps(1e300), 1000000u);
}

BOOST_AUTO_TEST_CASE(get_queue_length_negative_epa_throws) {
   // Wave 2 rejects negative int64 EPA before casting to uint64 (which would wrap huge).
   auto queues = fc::json::from_string(R"({"exit_queue": {"estimated_processed_at": -1}})");
   BOOST_CHECK_THROW(get_queue_length(queues, "exit_queue"), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(get_queue_length_uint64_max_far_future_returns_eta) {
   // Extremely large but still a valid uint64 - must not wrap; returns a huge ETA.
   // Separate sanity caps in compute_queue_updates guard against pushing such a value on-chain.
   auto queues = fc::json::from_string(R"({"exit_queue": {"estimated_processed_at": 18446744073709551614}})");
   auto result = get_queue_length(queues, "exit_queue");
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_GT(*result, uint64_t{0});
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// compute_queue_updates
// ---------------------------------------------------------------------------

namespace {
   constexpr uint64_t seconds_per_day = 60 * 60 * 24;
   constexpr uint64_t nine_days_sec = seconds_per_day * 9;

   // Near-future EPA helpers chosen to fit under the sanity caps enforced by
   // beacon_chain_config_updates (180-day withdraw cap, 365-day entry cap).
   uint64_t near_future_epa(uint64_t days_from_now) {
      return fc::time_point::now().sec_since_epoch() + days_from_now * seconds_per_day + 100;
   }

   fc::variant make_queues_response(std::optional<uint64_t> exit_epa,
                                    std::optional<uint64_t> deposit_epa) {
      auto exit_val = exit_epa ? std::to_string(*exit_epa) : "1";
      auto dep_val = deposit_epa ? std::to_string(*deposit_epa) : "1";
      auto json = R"({"exit_queue": {"estimated_processed_at": )" + exit_val +
                  R"(}, "deposit_queue": {"estimated_processed_at": )" + dep_val + "}}";
      return fc::json::from_string(json);
   }

   fc::variant make_ethstore_response(std::optional<double> avgapr7d) {
      if (!avgapr7d)
         return fc::json::from_string(R"({"other_field": 123})");
      auto json = R"({"avgapr7d": )" + std::to_string(*avgapr7d) + "}";
      return fc::json::from_string(json);
   }

   beacon_chain_config_updates make_crank(uint64_t exit_buffer_days = 9) {
      return beacon_chain_config_updates({}, exit_buffer_days);
   }
}

BOOST_AUTO_TEST_SUITE(compute_queue_updates_tests)

BOOST_AUTO_TEST_CASE(exit_queue_with_valid_eta) {
   auto queues = make_queues_response(near_future_epa(7), near_future_epa(3));
   auto result = make_crank().compute_queue_updates(queues);
   BOOST_REQUIRE(result.withdraw_delay_sec.has_value());
   BOOST_CHECK_GT(*result.withdraw_delay_sec, nine_days_sec);
}

BOOST_AUTO_TEST_CASE(exit_queue_past_epa_defaults_to_nine_days) {
   auto queues = make_queues_response(1, far_future_epa);
   auto result = make_crank().compute_queue_updates(queues);
   BOOST_REQUIRE(result.withdraw_delay_sec.has_value());
   BOOST_CHECK_EQUAL(*result.withdraw_delay_sec, nine_days_sec);
}

BOOST_AUTO_TEST_CASE(deposit_queue_valid_eta_converts_to_days) {
   uint64_t three_days_from_now_epa =
      fc::time_point::now().sec_since_epoch() + 3 * seconds_per_day + 100;
   auto queues = make_queues_response(far_future_epa, three_days_from_now_epa);
   auto result = make_crank().compute_queue_updates(queues);
   BOOST_REQUIRE(result.entry_queue_days.has_value());
   BOOST_CHECK_GE(*result.entry_queue_days, 2u);
   BOOST_CHECK_LE(*result.entry_queue_days, 4u);
}

BOOST_AUTO_TEST_CASE(deposit_queue_past_epa_defaults_to_one_day) {
   auto queues = make_queues_response(far_future_epa, 1);
   auto result = make_crank().compute_queue_updates(queues);
   BOOST_REQUIRE(result.entry_queue_days.has_value());
   BOOST_CHECK_EQUAL(*result.entry_queue_days, 1u);
}

BOOST_AUTO_TEST_CASE(all_queue_fields_populated) {
   auto queues = make_queues_response(near_future_epa(7), near_future_epa(3));
   auto result = make_crank().compute_queue_updates(queues);
   BOOST_CHECK(result.withdraw_delay_sec.has_value());
   BOOST_CHECK(result.entry_queue_days.has_value());
}

BOOST_AUTO_TEST_CASE(exit_queue_buffer_days_is_configurable) {
   auto queues = make_queues_response(1, far_future_epa); // past ETA, uses pure buffer
   auto result = make_crank(14).compute_queue_updates(queues);
   BOOST_REQUIRE(result.withdraw_delay_sec.has_value());
   BOOST_CHECK_EQUAL(*result.withdraw_delay_sec, 14u * seconds_per_day);
}

BOOST_AUTO_TEST_CASE(withdraw_delay_exceeding_cap_is_skipped) {
   // Wave 2 sanity cap: 180 days. Use buffer large enough to blow past it.
   // buffer_days=200 with past ETA -> 200-day withdraw -> exceeds 180-day cap -> skipped.
   auto queues = make_queues_response(1, near_future_epa(3));
   auto result = make_crank(200).compute_queue_updates(queues);
   BOOST_CHECK(!result.withdraw_delay_sec.has_value());
   BOOST_CHECK(result.entry_queue_days.has_value()); // unaffected
}

BOOST_AUTO_TEST_CASE(entry_queue_days_exceeding_cap_is_skipped) {
   // deposit_eta with 400-day horizon -> entry_queue_days=400 -> exceeds 365-day cap -> skipped.
   auto queues = make_queues_response(near_future_epa(7), near_future_epa(400));
   auto result = make_crank().compute_queue_updates(queues);
   BOOST_CHECK(result.withdraw_delay_sec.has_value()); // unaffected
   BOOST_CHECK(!result.entry_queue_days.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// compute_apy_updates
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(compute_apy_updates_tests)

BOOST_AUTO_TEST_CASE(apy_present_and_numeric) {
   auto ethstore = make_ethstore_response(0.05);
   auto result = make_crank().compute_apy_updates(ethstore);
   BOOST_REQUIRE(result.apy_bps.has_value());
   BOOST_CHECK_EQUAL(*result.apy_bps, 500u);
}

BOOST_AUTO_TEST_CASE(apy_missing_field_returns_nullopt) {
   auto ethstore = make_ethstore_response(std::nullopt);
   auto result = make_crank().compute_apy_updates(ethstore);
   BOOST_CHECK(!result.apy_bps.has_value());
}

BOOST_AUTO_TEST_CASE(apy_three_point_four_two_percent) {
   auto ethstore = make_ethstore_response(0.0342);
   auto result = make_crank().compute_apy_updates(ethstore);
   BOOST_REQUIRE(result.apy_bps.has_value());
   BOOST_CHECK_EQUAL(*result.apy_bps, 342u);
}

BOOST_AUTO_TEST_CASE(apy_integer_value_is_accepted) {
   // Wave 2 broadened acceptance from is_double() to is_numeric(); an unquoted int
   // like `"avgapr7d": 5` (meaning 500% APR) must parse, not silently yield 0.
   auto ethstore = fc::json::from_string(R"({"avgapr7d": 1})");
   auto result = make_crank().compute_apy_updates(ethstore);
   BOOST_REQUIRE(result.apy_bps.has_value());
   // 1.0 fraction = 10000 bps = the max_apy_bps cap, so it's accepted (not skipped).
   BOOST_CHECK_EQUAL(*result.apy_bps, 10000u);
}

BOOST_AUTO_TEST_CASE(apy_exceeds_cap_is_skipped) {
   // 2.0 fraction -> 20000 bps -> exceeds 10000 bps cap -> skipped.
   auto ethstore = make_ethstore_response(2.0);
   auto result = make_crank().compute_apy_updates(ethstore);
   BOOST_CHECK(!result.apy_bps.has_value());
}

BOOST_AUTO_TEST_CASE(apy_string_field_is_skipped) {
   // Present-but-non-numeric field (string) must not broadcast a bogus value.
   auto ethstore = fc::json::from_string(R"({"avgapr7d": "oops"})");
   auto result = make_crank().compute_apy_updates(ethstore);
   BOOST_CHECK(!result.apy_bps.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// beacon_chain_config_updates orchestration
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(beacon_chain_config_updates_tests)

BOOST_AUTO_TEST_CASE(happy_path_all_txs_sent_and_confirmed) {
   int withdraw_called = 0, entry_called = 0, apy_called = 0;
   std::vector<pending_tx> confirmed_txs;

   beacon_chain_config_updates crank({
      .fetch_queues = []() { return make_queues_response(near_future_epa(7), near_future_epa(3)); },
      .fetch_apy = []() { return make_ethstore_response(0.05); },
      .send_set_withdraw_delay = [&](uint64_t v) { ++withdraw_called; return "0xhash1"; },
      .send_set_entry_queue = [&](uint64_t v) { ++entry_called; return "0xhash2"; },
      .send_update_apy_bps = [&](uint64_t v) { ++apy_called; return "0xhash3"; },
      .confirm_txs = [&](const std::vector<pending_tx>& txs) { confirmed_txs = txs; }
   }, 9);
   crank();

   BOOST_CHECK_EQUAL(withdraw_called, 1);
   BOOST_CHECK_EQUAL(entry_called, 1);
   BOOST_CHECK_EQUAL(apy_called, 1);
   BOOST_CHECK_EQUAL(confirmed_txs.size(), 3u);
}

BOOST_AUTO_TEST_CASE(null_withdraw_contract_skips_set_withdraw_delay) {
   int withdraw_called = 0;
   std::vector<pending_tx> confirmed_txs;

   beacon_chain_config_updates crank({
      .fetch_queues = []() { return make_queues_response(near_future_epa(7), near_future_epa(3)); },
      .fetch_apy = []() { return make_ethstore_response(0.05); },
      .send_set_withdraw_delay = {},
      .send_set_entry_queue = [](uint64_t) { return "0xhash"; },
      .send_update_apy_bps = [](uint64_t) { return "0xhash"; },
      .confirm_txs = [&](const std::vector<pending_tx>& txs) { confirmed_txs = txs; }
   }, 9);
   crank();

   BOOST_CHECK_EQUAL(withdraw_called, 0);
   BOOST_CHECK_EQUAL(confirmed_txs.size(), 2u);
}

BOOST_AUTO_TEST_CASE(null_deposit_manager_skips_entry_and_apy) {
   int entry_called = 0, apy_called = 0;
   std::vector<pending_tx> confirmed_txs;

   beacon_chain_config_updates crank({
      .fetch_queues = []() { return make_queues_response(near_future_epa(7), near_future_epa(3)); },
      .fetch_apy = []() { return make_ethstore_response(0.05); },
      .send_set_withdraw_delay = [](uint64_t) { return "0xhash"; },
      .send_set_entry_queue = {},
      .send_update_apy_bps = {},
      .confirm_txs = [&](const std::vector<pending_tx>& txs) { confirmed_txs = txs; }
   }, 9);
   crank();

   BOOST_CHECK_EQUAL(entry_called, 0);
   BOOST_CHECK_EQUAL(apy_called, 0);
   BOOST_CHECK_EQUAL(confirmed_txs.size(), 1u);
}

BOOST_AUTO_TEST_CASE(apy_missing_skips_update_apy_bps) {
   int apy_called = 0;
   std::vector<pending_tx> confirmed_txs;

   beacon_chain_config_updates crank({
      .fetch_queues = []() { return make_queues_response(near_future_epa(7), near_future_epa(3)); },
      .fetch_apy = []() { return make_ethstore_response(std::nullopt); },
      .send_set_withdraw_delay = [](uint64_t) { return "0xhash1"; },
      .send_set_entry_queue = [](uint64_t) { return "0xhash2"; },
      .send_update_apy_bps = [&](uint64_t) { ++apy_called; return "0xhash3"; },
      .confirm_txs = [&](const std::vector<pending_tx>& txs) { confirmed_txs = txs; }
   }, 9);
   crank();

   BOOST_CHECK_EQUAL(apy_called, 0);
   BOOST_CHECK_EQUAL(confirmed_txs.size(), 2u);
}

BOOST_AUTO_TEST_CASE(fetch_throws_does_not_crash) {
   beacon_chain_config_updates crank({
      .fetch_queues = []() -> fc::variant { throw std::runtime_error("network error"); },
      .fetch_apy = []() { return make_ethstore_response(0.05); },
      .send_set_withdraw_delay = [](uint64_t) { return "0xhash"; },
      .send_set_entry_queue = [](uint64_t) { return "0xhash"; },
      .send_update_apy_bps = [](uint64_t) { return "0xhash"; },
      .confirm_txs = [](const std::vector<pending_tx>&) {}
   }, 9);
   BOOST_CHECK_NO_THROW(crank());
}

BOOST_AUTO_TEST_CASE(send_callback_throws_does_not_crash) {
   beacon_chain_config_updates crank({
      .fetch_queues = []() { return make_queues_response(near_future_epa(7), near_future_epa(3)); },
      .fetch_apy = []() { return make_ethstore_response(0.05); },
      .send_set_withdraw_delay = [](uint64_t) -> std::string { throw std::runtime_error("send failed"); },
      .send_set_entry_queue = [](uint64_t) { return std::string("0xhash2"); },
      .send_update_apy_bps = [](uint64_t) { return std::string("0xhash3"); },
      .confirm_txs = [](const std::vector<pending_tx>&) {}
   }, 9);
   BOOST_CHECK_NO_THROW(crank());
}

BOOST_AUTO_TEST_CASE(confirm_txs_throws_does_not_crash) {
   beacon_chain_config_updates crank({
      .fetch_queues = []() { return make_queues_response(near_future_epa(7), near_future_epa(3)); },
      .fetch_apy = []() { return make_ethstore_response(0.05); },
      .send_set_withdraw_delay = [](uint64_t) { return std::string("0xhash1"); },
      .send_set_entry_queue = [](uint64_t) { return std::string("0xhash2"); },
      .send_update_apy_bps = [](uint64_t) { return std::string("0xhash3"); },
      .confirm_txs = [](const std::vector<pending_tx>&) { throw std::runtime_error("confirm failed"); }
   }, 9);
   BOOST_CHECK_NO_THROW(crank());
}

BOOST_AUTO_TEST_SUITE_END()
