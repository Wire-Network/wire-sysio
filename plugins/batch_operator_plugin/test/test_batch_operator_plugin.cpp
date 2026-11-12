#include "../src/async_action_completion.hpp"
#include "../src/role_config.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/services/cron_service.hpp>

#include <fc/slug_name.hpp>

using namespace std::literals;

namespace {
constexpr auto immediate_wait = 0ms;
constexpr auto simulated_push_failure = "simulated push failure";
constexpr auto simulated_deferred_push_failure = "simulated deferred push failure";
constexpr auto callback_failure_message = "callback failure";
constexpr uint32_t expected_error_log_count = 1;
/// Default delivery budget in milliseconds.
constexpr auto delivery_timeout_option = "batch-delivery-timeout-ms";
constexpr uint32_t expected_delivery_timeout_ms = 60'000;

namespace callback_lifetime_action {
constexpr auto contract = "sysio.system";
constexpr auto name     = "onblock";
} // namespace callback_lifetime_action
} // namespace

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
   BOOST_CHECK(option_names.count("batch-yield-tick-interval-ms") > 0);
   BOOST_CHECK(option_names.count("batch-underwriter-max-exposure") > 0);
   BOOST_CHECK(option_names.count("batch-underwriter-unlimited-exposure") > 0);
   BOOST_CHECK(option_names.count("batch-kick-crank") > 0);
   BOOST_CHECK(option_names.count("batch-kick-interval-ms") > 0);
   // Configuring batch-operator-account is what enables the relay. A separate
   // enable flag would let an operator set the account and still relay nothing,
   // which is indistinguishable from a healthy node until the group misses an epoch.
   BOOST_CHECK(option_names.count("batch-enabled") == 0);
   // Nothing about an outpost is declared per node any more: the remote contract
   // identities come from the chain's sysio.chains row, and the RPC client for a
   // chain is the one registered under that chain's own code.
   BOOST_CHECK(option_names.count("batch-outpost") == 0);
   BOOST_CHECK(option_names.count("batch-sol-client-id") == 0);
} FC_LOG_AND_RETHROW();

/// Defaults give a delivery room for confirmations and the progress-read retry budget.
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
   BOOST_CHECK_EQUAL(vm[delivery_timeout_option].as<uint32_t>(), expected_delivery_timeout_ms);
   BOOST_CHECK_EQUAL(vm["batch-yield-tick-interval-ms"].as<uint32_t>(), 60000u);
   // The kick crank is on by default, like the other depot cranks, and retries a
   // pool no faster than the tickyield crank ticks one.
   BOOST_CHECK(vm["batch-kick-crank"].as<bool>());
   BOOST_CHECK_EQUAL(vm["batch-kick-interval-ms"].as<uint32_t>(), 60000u);
   // No default account: an unconfigured node leaves the relay off.
   BOOST_CHECK_EQUAL(vm.count("batch-operator-account"), 0u);
   BOOST_CHECK_EQUAL(vm.count("batch-underwriter-max-exposure"), 0u);
   BOOST_CHECK_EQUAL(vm.count("batch-underwriter-unlimited-exposure"), 0u);
} FC_LOG_AND_RETHROW();

/// Repeated unlimited symbols compose across command-line and configuration-file options.
BOOST_AUTO_TEST_CASE(unlimited_exposure_options_compose) {
   namespace bpo = boost::program_options;
   sysio::batch_operator_plugin plugin;
   bpo::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);
   bpo::variables_map vm;
   const std::vector<std::string> args{"--batch-underwriter-unlimited-exposure", "LIQETH",
                                       "--batch-underwriter-unlimited-exposure", "LIQSOL"};
   bpo::store(bpo::command_line_parser(args).options(cfg).run(), vm);
   std::istringstream config("batch-underwriter-unlimited-exposure = LIQBTC\n");
   bpo::store(bpo::parse_config_file(config, cfg), vm);
   bpo::notify(vm);
   const std::vector<std::string> expected{"LIQETH", "LIQSOL", "LIQBTC"};
   BOOST_CHECK(vm["batch-underwriter-unlimited-exposure"].as<std::vector<std::string>>() == expected);
}

/// Capped and unlimited options for the same symbol conflict, regardless of argument order or source.
/// Exercise the parser directly so a regression fails an assertion without reaching node dependencies.
BOOST_AUTO_TEST_CASE(conflicting_exposure_options_are_rejected) {
   namespace bpo = boost::program_options;
   const std::vector<std::string> capped{"--batch-underwriter-max-exposure", "100.000000000 LIQETH"};
   const std::vector<std::string> unlimited{"--batch-underwriter-unlimited-exposure", "LIQETH"};
   for (const bool capped_first : {false, true}) {
      sysio::batch_operator_plugin plugin;
      bpo::options_description cli, cfg;
      plugin.set_program_options(cli, cfg);
      auto args = capped_first ? capped : unlimited;
      const auto& second = capped_first ? unlimited : capped;
      args.insert(args.end(), second.begin(), second.end());
      bpo::variables_map vm;
      bpo::store(bpo::command_line_parser(args).options(cfg).run(), vm);
      bpo::notify(vm);
      BOOST_CHECK_EXCEPTION(sysio::batch_operator_detail::parse_exposure_policies(
         vm["batch-underwriter-max-exposure"].as<std::vector<std::string>>(),
         vm["batch-underwriter-unlimited-exposure"].as<std::vector<std::string>>()),
         fc::exception, [](const fc::exception& e) {
            return e.to_detail_string().find("both name LIQETH") != std::string::npos;
      });
   }

   sysio::batch_operator_plugin plugin;
   bpo::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);
   bpo::variables_map vm;
   bpo::store(bpo::command_line_parser(capped).options(cfg).run(), vm);
   std::istringstream config("batch-underwriter-unlimited-exposure = LIQETH\n");
   bpo::store(bpo::parse_config_file(config, cfg), vm);
   bpo::notify(vm);
   BOOST_CHECK_EXCEPTION(sysio::batch_operator_detail::parse_exposure_policies(
      vm["batch-underwriter-max-exposure"].as<std::vector<std::string>>(),
      vm["batch-underwriter-unlimited-exposure"].as<std::vector<std::string>>()),
      fc::exception, [](const fc::exception& e) {
         return e.to_detail_string().find("both name LIQETH") != std::string::npos;
   });
}

/// Unlimited-only configuration is validated during initialization, even without an underwriter account.
/// An invalid kick interval stops initialization before app() if exposure parsing regresses, yielding a normal
/// assertion failure on the wrong diagnostic instead of crashing the test process on missing dependencies.
BOOST_AUTO_TEST_CASE(invalid_unlimited_only_exposure_fails_initialization) {
   namespace bpo = boost::program_options;
   sysio::batch_operator_plugin plugin;
   bpo::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);
   const std::vector<std::string> args{"--batch-underwriter-unlimited-exposure", "liqeth",
                                       "--batch-kick-interval-ms", "0"};
   bpo::variables_map vm;
   bpo::store(bpo::command_line_parser(args).options(cfg).run(), vm);
   bpo::notify(vm);
   BOOST_CHECK_EQUAL(vm.count("batch-underwriter-max-exposure"), 0u);
   BOOST_CHECK_EXCEPTION(plugin.plugin_initialize(vm), fc::exception, [](const fc::exception& e) {
      return e.to_detail_string().find("unlimited exposure symbol 'liqeth'") != std::string::npos;
   });
}

/// A normal push result invokes its callback and wakes the waiting relay job.
BOOST_AUTO_TEST_CASE(async_action_completion_handles_normal_completion) {
   sysio::batch_operator_detail::async_action_completion completion;
   auto done = completion.get_future();
   using push_result = sysio::chain::next_function_variant<sysio::chain_apis::read_write::push_transaction_results>;
   push_result successful_result{sysio::chain_apis::read_write::push_transaction_results{}};
   bool success_logged = false;
   bool failure_logged = false;

   BOOST_CHECK(completion.complete_push_result(
      successful_result,
      [&success_logged] { success_logged = true; },
      [&failure_logged](const fc::exception_ptr&) { failure_logged = true; }));
   BOOST_CHECK(success_logged);
   BOOST_CHECK(!failure_logged);
   BOOST_CHECK(done.wait_for(immediate_wait) == std::future_status::ready);
}

/// A failed transaction result dispatches its failure logging handler once and
/// releases the waiting relay job.
BOOST_AUTO_TEST_CASE(async_action_completion_handles_failed_push_result) try {
   sysio::batch_operator_detail::async_action_completion completion;
   auto done = completion.get_future();
   using push_result = sysio::chain::next_function_variant<sysio::chain_apis::read_write::push_transaction_results>;
   fc::exception_ptr push_error;
   try {
      FC_THROW_EXCEPTION(fc::assert_exception, simulated_push_failure);
   } catch (const fc::exception& error) {
      push_error = error.dynamic_copy_exception();
   }

   push_result failed_result{push_error};
   uint32_t error_logs{};
   bool success_logged = false;

   BOOST_CHECK(completion.complete_push_result(
      failed_result,
      [&success_logged] { success_logged = true; },
      [&error_logs, &push_error](const fc::exception_ptr& logged_error) {
         BOOST_CHECK_EQUAL(logged_error.get(), push_error.get());
         ++error_logs;
      }));
   BOOST_CHECK(!completion.complete_push_result(
      failed_result,
      [&success_logged] { success_logged = true; },
      [&error_logs](const fc::exception_ptr&) { ++error_logs; }));
   BOOST_CHECK(!success_logged);
   BOOST_CHECK_EQUAL(error_logs, expected_error_log_count);
   BOOST_CHECK(done.wait_for(immediate_wait) == std::future_status::ready);
} FC_LOG_AND_RETHROW();

/// A deferred transaction result is evaluated before it reports completion.
BOOST_AUTO_TEST_CASE(async_action_completion_handles_deferred_push_result) {
   sysio::batch_operator_detail::async_action_completion completion;
   auto done = completion.get_future();
   using push_result = sysio::chain::next_function_variant<sysio::chain_apis::read_write::push_transaction_results>;
   bool deferred_called = false;
   bool success_logged = false;
   bool failure_logged = false;
   push_result deferred_result{
      std::function<sysio::chain::t_or_exception<sysio::chain_apis::read_write::push_transaction_results>()>{
         [&deferred_called] {
            deferred_called = true;
            return sysio::chain::t_or_exception<sysio::chain_apis::read_write::push_transaction_results>{
               sysio::chain_apis::read_write::push_transaction_results{}};
         }}};

   BOOST_CHECK(completion.complete_push_result(
      deferred_result,
      [&success_logged] { success_logged = true; },
      [&failure_logged](const fc::exception_ptr&) { failure_logged = true; }));
   BOOST_CHECK(deferred_called);
   BOOST_CHECK(success_logged);
   BOOST_CHECK(!failure_logged);
   BOOST_CHECK(done.wait_for(immediate_wait) == std::future_status::ready);
}

/// A deferred transaction error invokes the failure handler before completing.
BOOST_AUTO_TEST_CASE(async_action_completion_handles_deferred_push_failure) try {
   sysio::batch_operator_detail::async_action_completion completion;
   auto done = completion.get_future();
   using push_result = sysio::chain::next_function_variant<sysio::chain_apis::read_write::push_transaction_results>;
   fc::exception_ptr push_error;
   try {
      FC_THROW_EXCEPTION(fc::assert_exception, simulated_deferred_push_failure);
   } catch (const fc::exception& error) {
      push_error = error.dynamic_copy_exception();
   }

   bool success_logged = false;
   uint32_t error_logs{};
   push_result deferred_result{
      std::function<sysio::chain::t_or_exception<sysio::chain_apis::read_write::push_transaction_results>()>{
         [&push_error] {
            return sysio::chain::t_or_exception<sysio::chain_apis::read_write::push_transaction_results>{push_error};
         }}};

   BOOST_CHECK(completion.complete_push_result(
      deferred_result,
      [&success_logged] { success_logged = true; },
      [&error_logs, &push_error](const fc::exception_ptr& logged_error) {
         BOOST_CHECK_EQUAL(logged_error.get(), push_error.get());
         ++error_logs;
      }));
   BOOST_CHECK(!success_logged);
   BOOST_CHECK_EQUAL(error_logs, expected_error_log_count);
   BOOST_CHECK(done.wait_for(immediate_wait) == std::future_status::ready);
} FC_LOG_AND_RETHROW();

/// Callback-side exceptions are contained and still release the waiting relay job.
BOOST_AUTO_TEST_CASE(async_action_completion_contains_callback_exceptions) {
   sysio::batch_operator_detail::async_action_completion completion;
   auto done = completion.get_future();

   BOOST_CHECK_NO_THROW(completion.complete([] { throw std::runtime_error{callback_failure_message}; }));
   BOOST_CHECK(done.wait_for(immediate_wait) == std::future_status::ready);
}

/// Records whether a callback-retained API lifetime has been released.
struct callback_lifetime_sentinel {
   explicit callback_lifetime_sentinel(bool& destroyed)
      : destroyed(destroyed) {}

   ~callback_lifetime_sentinel() { destroyed = true; }

   bool& destroyed;
};

/// A delayed transaction callback keeps its API alive after the caller times out.
BOOST_AUTO_TEST_CASE(push_action_callback_retains_api_after_timeout) {
   using push_result = sysio::chain::next_function_variant<sysio::chain_apis::read_write::push_transaction_results>;
   using push_callback = sysio::chain::next_function<sysio::chain_apis::read_write::push_transaction_results>;
   push_callback late_callback;
   std::weak_ptr<callback_lifetime_sentinel> api_lifetime;
   bool api_destroyed = false;

   {
      auto api = std::make_shared<callback_lifetime_sentinel>(api_destroyed);
      api_lifetime = api;
      auto completion = std::make_shared<sysio::batch_operator_detail::async_action_completion>();
      auto done = completion->get_future();
      BOOST_CHECK(done.wait_for(immediate_wait) == std::future_status::timeout);
      late_callback = sysio::batch_operator_detail::create_push_action_callback(
         api, completion, callback_lifetime_action::contract, callback_lifetime_action::name);
      api.reset();
      BOOST_CHECK(!api_lifetime.expired());
   }

   push_result success{sysio::chain_apis::read_write::push_transaction_results{}};
   BOOST_CHECK_NO_THROW(late_callback(std::move(success)));
   BOOST_CHECK(!api_lifetime.expired());
   late_callback = {};
   BOOST_CHECK(api_lifetime.expired());
   BOOST_CHECK(api_destroyed);
}

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
