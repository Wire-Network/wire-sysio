#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <set>

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/services/cron_service.hpp>

using namespace std::literals;

BOOST_AUTO_TEST_SUITE(batch_operator_plugin_tests)

BOOST_AUTO_TEST_CASE(plugin_can_be_constructed) try {
   // Verify the plugin can be default-constructed without crashing
   sysio::batch_operator_plugin plugin;
   BOOST_CHECK(true);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(plugin_options_are_registered) try {
   // Verify set_program_options doesn't throw
   sysio::batch_operator_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   // Check that our options exist in cfg
   const auto& opts = cfg.options();
   std::set<std::string> option_names;
   for (const auto& opt : opts) {
      option_names.insert(opt->long_name());
   }
   BOOST_CHECK(option_names.count("batch-operator-account") > 0);
   BOOST_CHECK(option_names.count("batch-epoch-poll-ms") > 0);
   BOOST_CHECK(option_names.count("batch-delivery-timeout-ms") > 0);
   BOOST_CHECK(option_names.count("batch-enabled") > 0);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(default_options_are_correct) try {
   sysio::batch_operator_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   // Parse with no overrides to get defaults
   boost::program_options::variables_map vm;
   boost::program_options::store(
      boost::program_options::parse_command_line(0, static_cast<char**>(nullptr), cfg), vm);
   boost::program_options::notify(vm);

   BOOST_CHECK_EQUAL(vm["batch-epoch-poll-ms"].as<uint32_t>(), 15000u);
   BOOST_CHECK_EQUAL(vm["batch-delivery-timeout-ms"].as<uint32_t>(), 15000u);
   BOOST_CHECK_EQUAL(vm["batch-enabled"].as<bool>(), false);
} FC_LOG_AND_RETHROW();

/// Regression coverage for SEC-7: the private cron service must support adding
/// and cancelling per-outpost jobs after startup, which is how refreshed active
/// outposts begin relaying without restarting the batch operator.
BOOST_AUTO_TEST_CASE(cron_service_accepts_dynamic_outpost_jobs_after_start) try {
   sysio::services::cron_service::options opts;
   opts.name = "batch_operator_dynamic_outpost_jobs_test";
   opts.num_threads = 1;
   opts.autostart = true;
   auto svc = sysio::services::cron_service::create(opts);

   sysio::services::cron_service::job_schedule sched;
   sched.milliseconds = {sysio::services::cron_service::job_schedule::step_value{1000}};
   sysio::services::cron_service::job_metadata_t meta;
   meta.label = "outpost_opp_outbound_42";
   meta.one_at_a_time = true;

   auto id = svc->add(sched, [] {}, meta);
   auto listed = svc->list({meta.label});
   BOOST_REQUIRE_EQUAL(listed.size(), 1u);
   BOOST_CHECK_EQUAL(listed.front(), id);

   svc->cancel(id);
   BOOST_CHECK(svc->list({meta.label}).empty());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
