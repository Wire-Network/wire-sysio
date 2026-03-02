#include <boost/test/unit_test.hpp>

#include <sysio/services/cron_parser.hpp>
#include <sysio/services/cron_service.hpp>
#include <fc/exception/exception.hpp>

using namespace sysio::services;
using svc = cron_service;

BOOST_AUTO_TEST_SUITE(cron_parser_tests)

// -----------------------------------------------------------------------
// Basic parsing tests
// -----------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(parse_wildcard_all_fields) try {
   auto sched_opt = parse_cron_schedule("* * * * *");
   BOOST_REQUIRE(sched_opt.has_value());

   auto& sched = *sched_opt;
   // All fields should be empty (wildcard)
   BOOST_CHECK(sched.milliseconds.empty());
   BOOST_CHECK(sched.minutes.empty());
   BOOST_CHECK(sched.hours.empty());
   BOOST_CHECK(sched.day_of_month.empty());
   BOOST_CHECK(sched.month.empty());
   BOOST_CHECK(sched.day_of_week.empty());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_exact_values) try {
   auto sched_opt = parse_cron_schedule("30 9 15 6 1");
   BOOST_REQUIRE(sched_opt.has_value());

   auto& sched = *sched_opt;
   BOOST_CHECK_EQUAL(sched.minutes.size(), 1);
   BOOST_CHECK_EQUAL(sched.hours.size(), 1);
   BOOST_CHECK_EQUAL(sched.day_of_month.size(), 1);
   BOOST_CHECK_EQUAL(sched.month.size(), 1);
   BOOST_CHECK_EQUAL(sched.day_of_week.size(), 1);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_range_values) try {
   auto sched_opt = parse_cron_schedule("0 9-17 * * 1-5");
   BOOST_REQUIRE(sched_opt.has_value());

   auto& sched = *sched_opt;
   // Minute 0 (exact)
   BOOST_CHECK_EQUAL(sched.minutes.size(), 1);
   // Hours 9-17 (range)
   BOOST_CHECK_EQUAL(sched.hours.size(), 1);
   // Day of week 1-5 (range)
   BOOST_CHECK_EQUAL(sched.day_of_week.size(), 1);

   // Verify it's a range_value
   auto hour_val = *sched.hours.begin();
   BOOST_CHECK(std::holds_alternative<svc::job_schedule::range_value>(hour_val));
   auto range = std::get<svc::job_schedule::range_value>(hour_val);
   BOOST_CHECK_EQUAL(range.from, 9);
   BOOST_CHECK_EQUAL(range.to, 17);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_step_values) try {
   auto sched_opt = parse_cron_schedule("*/5 * * * *");
   BOOST_REQUIRE(sched_opt.has_value());

   auto& sched = *sched_opt;
   // Minutes every 5 (step)
   BOOST_CHECK_EQUAL(sched.minutes.size(), 1);

   auto minute_val = *sched.minutes.begin();
   BOOST_CHECK(std::holds_alternative<svc::job_schedule::step_value>(minute_val));
   auto step = std::get<svc::job_schedule::step_value>(minute_val);
   BOOST_CHECK_EQUAL(step.step, 5);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_list_values) try {
   auto sched_opt = parse_cron_schedule("0,15,30,45 * * * *");
   BOOST_REQUIRE(sched_opt.has_value());

   auto& sched = *sched_opt;
   // Four exact minute values
   BOOST_CHECK_EQUAL(sched.minutes.size(), 4);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_range_with_step) try {
   auto sched_opt = parse_cron_schedule("10-50/10 * * * *");
   BOOST_REQUIRE(sched_opt.has_value());

   auto& sched = *sched_opt;
   // Should expand to: 10, 20, 30, 40, 50 (5 values)
   BOOST_CHECK_EQUAL(sched.minutes.size(), 5);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_extended_format_with_milliseconds) try {
   auto sched_opt = parse_cron_schedule("5000 * * * * *");
   BOOST_REQUIRE(sched_opt.has_value());

   auto& sched = *sched_opt;
   // Milliseconds field should have one exact value
   BOOST_CHECK_EQUAL(sched.milliseconds.size(), 1);

   auto ms_val = *sched.milliseconds.begin();
   BOOST_CHECK(std::holds_alternative<svc::job_schedule::exact_value>(ms_val));
   auto exact = std::get<svc::job_schedule::exact_value>(ms_val);
   BOOST_CHECK_EQUAL(exact.value, 5000);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_complex_schedule) try {
   // Every 15 minutes, between 9 AM and 5 PM, on weekdays
   auto sched_opt = parse_cron_schedule("*/15 9-17 * * 1-5");
   BOOST_REQUIRE(sched_opt.has_value());

   auto& sched = *sched_opt;
   BOOST_CHECK_EQUAL(sched.minutes.size(), 1); // step
   BOOST_CHECK_EQUAL(sched.hours.size(), 1);   // range
   BOOST_CHECK(sched.day_of_month.empty());    // wildcard
   BOOST_CHECK(sched.month.empty());           // wildcard
   BOOST_CHECK_EQUAL(sched.day_of_week.size(), 1); // range
} FC_LOG_AND_RETHROW();

// -----------------------------------------------------------------------
// Error handling tests
// -----------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(parse_invalid_empty_string) try {
   auto sched_opt = parse_cron_schedule("");
   BOOST_CHECK(!sched_opt.has_value());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_invalid_too_few_fields) try {
   auto sched_opt = parse_cron_schedule("* * *");
   BOOST_CHECK(!sched_opt.has_value());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_invalid_too_many_fields) try {
   auto sched_opt = parse_cron_schedule("* * * * * * *");
   BOOST_CHECK(!sched_opt.has_value());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_invalid_value_out_of_range) try {
   // Minute 60 is out of range (0-59)
   auto sched_opt = parse_cron_schedule("60 * * * *");
   BOOST_CHECK(!sched_opt.has_value());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_invalid_range_backwards) try {
   // Range 17-9 is invalid (from > to)
   auto sched_opt = parse_cron_schedule("* 17-9 * * *");
   BOOST_CHECK(!sched_opt.has_value());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_invalid_non_numeric) try {
   auto sched_opt = parse_cron_schedule("abc * * * *");
   BOOST_CHECK(!sched_opt.has_value());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_invalid_zero_step) try {
   auto sched_opt = parse_cron_schedule("*/0 * * * *");
   BOOST_CHECK(!sched_opt.has_value());
} FC_LOG_AND_RETHROW();

// -----------------------------------------------------------------------
// parse_cron_schedule_or_throw tests
// -----------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(parse_or_throw_valid) try {
   auto sched = parse_cron_schedule_or_throw("* * * * *");
   BOOST_CHECK(sched.minutes.empty()); // Should succeed
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_or_throw_invalid) try {
   BOOST_CHECK_THROW(
      parse_cron_schedule_or_throw("invalid"),
      fc::exception
   );
} FC_LOG_AND_RETHROW();

// -----------------------------------------------------------------------
// Real-world examples
// -----------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(parse_example_every_minute) try {
   auto sched_opt = parse_cron_schedule("* * * * *");
   BOOST_REQUIRE(sched_opt.has_value());
   // All wildcards - fires every minute
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_example_daily_midnight) try {
   auto sched_opt = parse_cron_schedule("0 0 * * *");
   BOOST_REQUIRE(sched_opt.has_value());
   auto& sched = *sched_opt;
   BOOST_CHECK_EQUAL(sched.minutes.size(), 1);
   BOOST_CHECK_EQUAL(sched.hours.size(), 1);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_example_weekday_business_hours) try {
   auto sched_opt = parse_cron_schedule("0 9-17 * * 1-5");
   BOOST_REQUIRE(sched_opt.has_value());
   // Every hour from 9-5 on weekdays
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_example_first_of_month) try {
   auto sched_opt = parse_cron_schedule("0 0 1 * *");
   BOOST_REQUIRE(sched_opt.has_value());
   auto& sched = *sched_opt;
   // First day of every month at midnight
   BOOST_CHECK_EQUAL(sched.day_of_month.size(), 1);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(parse_example_every_5_seconds_extended) try {
   auto sched_opt = parse_cron_schedule("*/5000 * * * * *");
   BOOST_REQUIRE(sched_opt.has_value());
   auto& sched = *sched_opt;
   // Every 5 seconds (5000 ms)
   BOOST_CHECK_EQUAL(sched.milliseconds.size(), 1);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
