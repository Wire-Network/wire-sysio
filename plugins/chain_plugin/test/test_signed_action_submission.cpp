#include <sysio/chain_plugin/chain_plugin.hpp>

#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <future>
#include <memory>

namespace {

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::chain_apis;

struct signed_action_submission_tester : sysio::testing::validating_tester {
   signed_action_submission_tester()
      : validating_tester({}, nullptr,
                          sysio::testing::setup_policy::preactivate_feature_only) {}

   std::optional<trx_retry_db> retry_db;
};

} // namespace

BOOST_AUTO_TEST_SUITE(signed_action_submission_tests)

BOOST_FIXTURE_TEST_CASE(delayed_completion_retains_read_write_api,
                        signed_action_submission_tester) {
   auto read_write_api = std::make_shared<read_write>(
      *control, retry_db, fc::seconds(1), fc::seconds(1), true);
   std::weak_ptr<read_write> weak_read_write_api = read_write_api;

   auto promise = std::make_shared<std::promise<signed_action_result>>();
   auto future = promise->get_future();
   auto callback = signed_action_detail::create_signed_action_submission_callback(
      read_write_api, promise);

   // Model push_signed_action returning on timeout while chain processing still
   // owns the completion callback.
   read_write_api.reset();
   promise.reset();
   BOOST_TEST(!weak_read_write_api.expired());
   BOOST_CHECK(future.wait_for(std::chrono::milliseconds{0}) ==
               std::future_status::timeout);

   using callback_result = chain::plugin_interface::next_function_variant<
      read_write::push_transaction_results>;
   callback(callback_result{read_write::push_transaction_results{
      transaction_id_type{}, fc::variant{}}});

   const auto result = future.get();
   BOOST_CHECK(result.state == signed_action_result::status::succeeded);

   callback = {};
   BOOST_TEST(weak_read_write_api.expired());
}

BOOST_AUTO_TEST_SUITE_END()
