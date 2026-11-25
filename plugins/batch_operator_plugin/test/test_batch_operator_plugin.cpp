#include "../src/async_action_completion.hpp"
#include "../src/role_config.hpp"
#include "../src/served_chains.hpp"
#include "mocks/mock_outpost_client.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/opp/depot/opreg_status.hpp>
#include <sysio/services/cron_service.hpp>

#include <fc/log/logger.hpp>
#include <fc/slug_name.hpp>
#include <fc/variant_object.hpp>

#include <spdlog/sinks/ostream_sink.h>

using namespace std::literals;

namespace {
constexpr auto immediate_wait = 0ms;
constexpr auto simulated_push_failure = "simulated push failure";
constexpr auto simulated_deferred_push_failure = "simulated deferred push failure";
constexpr auto simulated_contract_refusal = "simulated contract refusal";
constexpr auto callback_failure_message = "callback failure";
constexpr uint32_t expected_error_log_count = 1;

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
   // Each chain's delivery timeout comes from its on-chain epoch duration.
   BOOST_CHECK(option_names.count("batch-delivery-timeout-ms") == 0);
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

/// A failed push is the contract's refusal only when one of its checks failed, immediately or deferred; any other
/// failure, and a success, is not.
BOOST_AUTO_TEST_CASE(async_action_completion_tells_a_contract_refusal) try {
   using push_result = sysio::chain::next_function_variant<sysio::chain_apis::read_write::push_transaction_results>;
   using deferred_t  = sysio::chain::t_or_exception<sysio::chain_apis::read_write::push_transaction_results>;
   const auto error_of = [](const auto& raise) {
      try {
         raise();
      } catch (const fc::exception& error) {
         return error.dynamic_copy_exception();
      }
      return fc::exception_ptr{};
   };
   const fc::exception_ptr assert_message = error_of([] {
      FC_THROW_EXCEPTION(sysio::chain::sysio_assert_message_exception, simulated_contract_refusal);
   });
   const fc::exception_ptr assert_code = error_of([] {
      FC_THROW_EXCEPTION(sysio::chain::sysio_assert_code_exception, simulated_contract_refusal);
   });
   const fc::exception_ptr other = error_of([] { FC_THROW_EXCEPTION(fc::assert_exception, simulated_push_failure); });
   const auto refused = [](const push_result& result) {
      sysio::batch_operator_detail::async_action_completion completion;
      completion.complete_push_result(result, [] {}, [](const fc::exception_ptr&) {});
      return completion.refused_by_contract();
   };
   const auto deferred = [](const fc::exception_ptr& error) {
      return push_result{std::function<deferred_t()>{[error] { return deferred_t{error}; }}};
   };

   BOOST_CHECK(refused(push_result{assert_message}));
   BOOST_CHECK(refused(push_result{assert_code}));
   BOOST_CHECK(refused(deferred(assert_message)));
   BOOST_CHECK(!refused(push_result{other}));
   BOOST_CHECK(!refused(deferred(other)));
   BOOST_CHECK(!refused(push_result{sysio::chain_apis::read_write::push_transaction_results{}}));
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

namespace {

/// Capture error-level log lines while preserving the process-wide logger on every test exit path.
struct error_log_capture {
   std::ostringstream               output;
   fc::log_level                    saved_level = fc::logger::default_logger().get_log_level();
   std::unique_ptr<spdlog::logger>  saved_agent;

   error_log_capture() {
      auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
      sink->set_pattern("%v");
      auto agent = std::make_unique<spdlog::logger>("push-refusal-test", sink);
      agent->set_level(spdlog::level::err);
      auto& logger = fc::logger::default_logger();
      saved_agent = std::move(logger.get_agent_logger());
      logger.update_agent_logger(std::move(agent));
      logger.set_log_level(fc::log_level::error);
   }

   ~error_log_capture() {
      auto& logger = fc::logger::default_logger();
      logger.update_agent_logger(std::move(saved_agent));
      logger.set_log_level(saved_level);
   }
};

} // namespace

/// A refusal is logged as an error by default, and below it for a push whose refusal is an expected race; either
/// way the push completes unsucceeded.
BOOST_AUTO_TEST_CASE(push_refusal_is_logged_at_the_level_the_push_asks_for) try {
   using push_result = sysio::chain::next_function_variant<sysio::chain_apis::read_write::push_transaction_results>;
   using sysio::batch_operator_detail::refusal_log;
   fc::exception_ptr push_error;
   try {
      FC_THROW_EXCEPTION(fc::assert_exception, simulated_push_failure);
   } catch (const fc::exception& error) {
      push_error = error.dynamic_copy_exception();
   }
   for (const refusal_log refusal : {refusal_log::error, refusal_log::info}) {
      error_log_capture capture;
      auto completion = std::make_shared<sysio::batch_operator_detail::async_action_completion>();
      auto done       = completion->get_future();
      auto callback   = sysio::batch_operator_detail::create_push_action_callback(
         std::shared_ptr<void>{}, completion, callback_lifetime_action::contract, callback_lifetime_action::name,
         refusal);
      callback(push_result{push_error});
      BOOST_CHECK(done.wait_for(immediate_wait) == std::future_status::ready);
      BOOST_CHECK(!completion->succeeded());
      const bool logged_as_error = capture.output.str().find(simulated_push_failure) != std::string::npos;
      BOOST_CHECK_EQUAL(logged_as_error, refusal == refusal_log::error);
   }
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

// ─── Served chains ──────────────────────────────────────────────────────────

namespace {

using namespace sysio::batch_operator_detail;
using fc::operator""_s;

constexpr auto self     = "batchop.a";
constexpr auto stranger = "batchop.b";

/// One chain for each membership the relay rule tells apart.
constexpr fc::slug_name serving_chain     = "ETHEREUM"_s;
constexpr fc::slug_name left_seated_chain = "SOLANA"_s;
constexpr fc::slug_name left_free_chain   = "BASE"_s;
constexpr fc::slug_name evicted_chain     = "ARBITRUM"_s;
/// An active outpost this operator has no membership on.
constexpr fc::slug_name unserved_chain    = "POLYGON"_s;

constexpr uint8_t seated_group = 0;
/// A `servchains` status value this build does not know.
constexpr uint64_t unknown_status_value = 7;
constexpr auto     unknown_status_name  = "RETIRED";
constexpr auto     no_rpc_client        = "no RPC client is registered under this chain code";

/// A `servchains` value as the ABI renders it.
fc::variant membership_row(std::string_view account, fc::slug_name chain, const fc::variant& status) {
   return fc::variant(fc::mutable_variant_object()(sysio::opreg_names::field::account, std::string(account))
                                                  (sysio::opreg_names::field::chain_code, chain)
                                                  (sysio::opreg_names::field::status, status));
}

/// The status's ABI spelling.
fc::variant status_name(chain_service_status status) { return fc::variant(std::string(magic_enum::enum_name(status))); }

/// A chain view whose window seats `self` in `group`, or nobody.
chain_epoch_view view_seating(fc::slug_name chain, uint8_t group) {
   chain_epoch_view view;
   view.chain_code        = chain.value;
   view.election.my_group = group;
   return view;
}

outpost_descriptor outpost(fc::slug_name chain) { return outpost_descriptor{.id = chain.value}; }

/// An `operators` value as the ABI renders it, with `status`'s spelling.
fc::variant operator_row(std::string_view account, sysio::opp::types::OperatorStatus status) {
   return fc::variant(fc::mutable_variant_object()(sysio::opreg_names::field::account, std::string(account))
                                                  (sysio::opreg_names::field::status,
                                                   sysio::opp::types::OperatorStatus_Name(status)));
}

/// The chain codes of `outposts`, in order.
std::vector<uint64_t> codes_of(const std::vector<outpost_descriptor>& outposts) {
   std::vector<uint64_t> codes;
   for (const outpost_descriptor& op : outposts) codes.push_back(op.id);
   return codes;
}

} // namespace

BOOST_AUTO_TEST_SUITE(batch_operator_served_chains_tests)

/// Memberships decode from the status's name or its value; another account's row and a status this build does not
/// know are left out.
BOOST_AUTO_TEST_CASE(memberships_decode_by_name_and_value) try {
   const fc::variants rows{
      membership_row(self, serving_chain, status_name(chain_service_status::SERVING)),
      membership_row(self, left_seated_chain,
                     fc::variant(magic_enum::enum_integer(chain_service_status::LEFT))),
      membership_row(self, evicted_chain, status_name(chain_service_status::EVICTED)),
      membership_row(stranger, unserved_chain, status_name(chain_service_status::SERVING)),
      membership_row(self, left_free_chain, fc::variant(std::string(unknown_status_name))),
      membership_row(self, unserved_chain, fc::variant(unknown_status_value)),
   };
   const chain_memberships memberships = decode_memberships(rows, sysio::chain::name(self));
   const chain_memberships expected{{serving_chain.value, chain_service_status::SERVING},
                                    {left_seated_chain.value, chain_service_status::LEFT},
                                    {evicted_chain.value, chain_service_status::EVICTED}};
   BOOST_CHECK(memberships == expected);
} FC_LOG_AND_RETHROW();

/// The `byaccount` range reads every row of the account, from its key to the next account's (exclusive); the largest
/// name reads to the end of the index.
BOOST_AUTO_TEST_CASE(the_membership_range_spans_the_account) try {
   const uint64_t    value = sysio::chain::name(self).to_uint64_t();
   const index_range range = membership_range(sysio::chain::name(self));
   BOOST_CHECK_EQUAL(range.lower_bound, byaccount_bound(value));
   BOOST_CHECK_EQUAL(range.upper_bound, byaccount_bound(value + 1));
   // A uint64 key is written as a decimal string, as fc::json writes every 64-bit integer.
   BOOST_CHECK_EQUAL(range.lower_bound, "{\"byaccount\":\"" + std::to_string(value) + "\"}");

   const index_range last = membership_range(sysio::chain::name(std::numeric_limits<uint64_t>::max()));
   BOOST_CHECK_EQUAL(last.lower_bound, byaccount_bound(std::numeric_limits<uint64_t>::max()));
   BOOST_CHECK(last.upper_bound.empty());
} FC_LOG_AND_RETHROW();

/// An operator serving two chains holds two `servchains` rows under one `byaccount` key. The range read follows
/// each page's cursor to the end, so both memberships decode, whatever page boundary falls between them.
BOOST_AUTO_TEST_CASE(two_memberships_of_one_account_are_both_read) try {
   const index_range range = membership_range(sysio::chain::name(self));
   constexpr auto    second_page = "second-page-cursor";
   std::vector<std::pair<std::string, std::string>> reads;
   const std::optional<fc::variants> rows = read_range(range, [&](const std::string& lower, const std::string& upper) {
      reads.emplace_back(lower, upper);
      if (reads.size() == 1) {
         return std::optional<table_page>{table_page{
            .rows = {membership_row(self, serving_chain, status_name(chain_service_status::SERVING))},
            .more = true,
            .next_key = second_page}};
      }
      return std::optional<table_page>{table_page{
         .rows = {membership_row(self, left_seated_chain, status_name(chain_service_status::LEFT))}}};
   });
   BOOST_REQUIRE(rows.has_value());
   const std::vector<std::pair<std::string, std::string>> expected_reads{{range.lower_bound, range.upper_bound},
                                                                         {second_page, range.upper_bound}};
   BOOST_CHECK(reads == expected_reads);
   const chain_memberships expected{{serving_chain.value, chain_service_status::SERVING},
                                    {left_seated_chain.value, chain_service_status::LEFT}};
   BOOST_CHECK(decode_memberships(*rows, sysio::chain::name(self)) == expected);
} FC_LOG_AND_RETHROW();

/// A range read fails as a whole when any page's read fails, or when a page reports more rows without moving its
/// cursor, so the caller keeps its last good reading instead of acting on part of the range.
BOOST_AUTO_TEST_CASE(a_range_read_fails_on_a_failed_or_stuck_page) try {
   const index_range range = membership_range(sysio::chain::name(self));
   const fc::variants first{membership_row(self, serving_chain, status_name(chain_service_status::SERVING))};
   uint32_t           calls = 0;
   BOOST_CHECK(!read_range(range, [&](const std::string&, const std::string&) -> std::optional<table_page> {
      if (++calls == 1) return table_page{.rows = first, .more = true, .next_key = "cursor"};
      return std::nullopt;
   }));
   BOOST_CHECK_EQUAL(calls, 2u);

   for (const std::string& stuck : {std::string{}, range.lower_bound}) {
      calls = 0;
      BOOST_CHECK(!read_range(range, [&](const std::string&, const std::string&) -> std::optional<table_page> {
         ++calls;
         return table_page{.rows = first, .more = true, .next_key = stuck};
      }));
      BOOST_CHECK_EQUAL(calls, 1u);
   }
} FC_LOG_AND_RETHROW();

/// The relay rule: SERVING relays, LEFT relays while it still holds a seat, EVICTED never.
BOOST_AUTO_TEST_CASE(serving_and_seated_left_members_relay) try {
   for (const bool seated : {false, true}) {
      BOOST_CHECK(relays_chain(chain_service_status::SERVING, seated));
      BOOST_CHECK_EQUAL(relays_chain(chain_service_status::LEFT, seated), seated);
      BOOST_CHECK(!relays_chain(chain_service_status::EVICTED, seated));
   }
} FC_LOG_AND_RETHROW();

/// The relayed chains: SERVING (seated or not), LEFT while seated in that chain's own window; not LEFT without a
/// seat, EVICTED though seated, or a chain with no membership.
BOOST_AUTO_TEST_CASE(relayed_chains_follow_membership_and_seats) try {
   const chain_memberships memberships{{serving_chain.value, chain_service_status::SERVING},
                                       {left_seated_chain.value, chain_service_status::LEFT},
                                       {left_free_chain.value, chain_service_status::LEFT},
                                       {evicted_chain.value, chain_service_status::EVICTED}};
   chain_epochs views;
   views.emplace(left_seated_chain.value, view_seating(left_seated_chain, seated_group));
   views.emplace(left_free_chain.value, view_seating(left_free_chain, GROUP_NONE));
   views.emplace(evicted_chain.value, view_seating(evicted_chain, seated_group));
   views.emplace(unserved_chain.value, view_seating(unserved_chain, seated_group));

   const std::set<uint64_t> expected{serving_chain.value, left_seated_chain.value};
   BOOST_CHECK(relayed_chains(memberships, views) == expected);
   // Its seat on another chain does not keep a LEFT member on this one.
   const chain_memberships left_only{{left_free_chain.value, chain_service_status::LEFT}};
   BOOST_CHECK(relayed_chains(left_only, views).empty());
} FC_LOG_AND_RETHROW();

/// The own-status lookup is an exact read of the operator's `operators` row, keyed by its account's value.
BOOST_AUTO_TEST_CASE(the_own_status_lookup_keys_the_account) try {
   const uint64_t value = sysio::chain::name(self).to_uint64_t();
   BOOST_CHECK_EQUAL(operator_find_bound(sysio::chain::name(self)), "{\"account\":\"" + std::to_string(value) + "\"}");
} FC_LOG_AND_RETHROW();

/// The own status is the operator's row's; with no row of its own the operator reads as UNKNOWN, whose deliveries
/// `sysio.msgch::deliver` refuses.
BOOST_AUTO_TEST_CASE(own_status_is_the_row_or_unknown_without_one) try {
   using sysio::opp::types::OperatorStatus_Name;
   const sysio::chain::name me(self);
   BOOST_CHECK_EQUAL(own_status_of({operator_row(self, sysio::opp::types::OPERATOR_STATUS_SLASHED)}, me),
                     OperatorStatus_Name(sysio::opp::types::OPERATOR_STATUS_SLASHED));
   BOOST_CHECK_EQUAL(own_status_of({}, me), OperatorStatus_Name(sysio::opp::types::OPERATOR_STATUS_UNKNOWN));
   BOOST_CHECK_EQUAL(own_status_of({operator_row(stranger, sysio::opp::types::OPERATOR_STATUS_ACTIVE)}, me),
                     OperatorStatus_Name(sysio::opp::types::OPERATOR_STATUS_UNKNOWN));
} FC_LOG_AND_RETHROW();

/// Across polls the relay jobs follow `sysio.msgch::deliver`'s status gate: none run before a read of ACTIVE (a node
/// that starts before its operator row is readable), they run on every relayed chain while the operator reads ACTIVE,
/// stop on all of them under UNKNOWN, without a row, or once SLASHED or TERMINATED, and restart when it reads ACTIVE
/// again (a terminated operator that registers again). A failed read leaves them as they were.
BOOST_AUTO_TEST_CASE(relay_jobs_run_only_while_deliver_accepts_the_status) try {
   using namespace sysio::opp::types;
   const std::vector<outpost_descriptor> relayed{outpost(serving_chain), outpost(left_seated_chain)};
   struct poll {
      std::optional<fc::variants> lookup;   ///< nullopt: the read failed
      bool                        relays;
   };
   const std::vector<poll> polls{
      {std::nullopt, false},
      {fc::variants{}, false},
      {fc::variants{operator_row(self, OPERATOR_STATUS_ACTIVE)}, true},
      {fc::variants{operator_row(self, OPERATOR_STATUS_UNKNOWN)}, false},
      {std::nullopt, false},
      {fc::variants{operator_row(self, OPERATOR_STATUS_ACTIVE)}, true},
      {std::nullopt, true},
      {fc::variants{}, false},
      {fc::variants{operator_row(self, OPERATOR_STATUS_ACTIVE)}, true},
      {fc::variants{operator_row(self, OPERATOR_STATUS_SLASHED)}, false},
      {fc::variants{operator_row(self, OPERATOR_STATUS_ACTIVE)}, true},
      {fc::variants{operator_row(self, OPERATOR_STATUS_TERMINATED)}, false},
      {fc::variants{operator_row(self, OPERATOR_STATUS_UNKNOWN)}, false},
      {fc::variants{operator_row(self, OPERATOR_STATUS_ACTIVE)}, true},
   };
   bool status_delivers = false;
   for (const poll& each : polls) {
      if (each.lookup) {
         status_delivers = sysio::opp::depot::opreg_status::compute_delivers(
            own_status_of(*each.lookup, sysio::chain::name(self)), status_delivers);
      }
      const std::vector<uint64_t> expected = each.relays ? codes_of(relayed) : std::vector<uint64_t>{};
      BOOST_CHECK(codes_of(relay_job_outposts(relayed, status_delivers)) == expected);
   }
} FC_LOG_AND_RETHROW();

/// Only relayed outposts are built: a chain this operator does not serve is never built, so no client for it is
/// required, while a relayed chain without one is still reported unserviceable (the plugin then shuts the node down).
BOOST_AUTO_TEST_CASE(an_unserved_chain_is_ignored_and_an_unserviceable_served_chain_refuses) try {
   const std::vector<outpost_descriptor> active{outpost(serving_chain), outpost(unserved_chain),
                                                outpost(left_seated_chain)};
   const std::set<uint64_t>              relayed{serving_chain.value, left_seated_chain.value};
   const std::vector<outpost_descriptor> relay = relayed_outposts(active, relayed);
   BOOST_REQUIRE_EQUAL(relay.size(), 2u);
   BOOST_CHECK_EQUAL(relay[0].id, serving_chain.value);
   BOOST_CHECK_EQUAL(relay[1].id, left_seated_chain.value);

   // No RPC client for any chain: only the relayed ones are reported.
   std::map<uint64_t, std::shared_ptr<sysio::outpost_client>> built;
   std::vector<uint64_t>                                      made;
   const auto wrap = [](const outpost_descriptor&, const std::shared_ptr<sysio::outpost_client>& client) {
      return client;
   };
   const std::vector<unserviceable_chain> unserviceable = build_missing(
      built, relay,
      [&](const outpost_descriptor& op) {
         made.push_back(op.id);
         return client_build{.unserviceable = no_rpc_client};
      },
      wrap);
   BOOST_REQUIRE_EQUAL(unserviceable.size(), 2u);
   BOOST_CHECK_EQUAL(unserviceable[0].code, serving_chain.to_string());
   BOOST_CHECK_EQUAL(unserviceable[0].why, no_rpc_client);
   BOOST_CHECK_EQUAL(unserviceable[1].code, left_seated_chain.to_string());
   BOOST_CHECK(std::ranges::find(made, unserved_chain.value) == made.end());
   BOOST_CHECK(built.empty());

   // With a client for the served chain only, the relay builds it and reports nothing: the unserved chain, which
   // has no client, is not asked for one.
   made.clear();
   const std::set<uint64_t> serving_only{serving_chain.value};
   const std::vector<unserviceable_chain> served = build_missing(
      built, relayed_outposts(active, serving_only),
      [&](const outpost_descriptor& op) {
         made.push_back(op.id);
         if (op.id != serving_chain.value) return client_build{.unserviceable = no_rpc_client};
         return client_build{.client = std::make_shared<sysio::test::mock_outpost_client>(
                                sysio::opp::types::CHAIN_KIND_EVM, op.id, 0)};
      },
      wrap);
   BOOST_CHECK(served.empty());
   BOOST_CHECK(made == std::vector<uint64_t>{serving_chain.value});
   BOOST_REQUIRE_EQUAL(built.size(), 1u);
   BOOST_CHECK(built.contains(serving_chain.value));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
