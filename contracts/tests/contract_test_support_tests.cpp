#include <boost/test/unit_test.hpp>
#include <fc/io/json.hpp>
#include "contract_test_support.hpp"

using namespace sysio;
using namespace sysio::chain;

namespace {
// Shadows the budget with 0 us so a wall-clock check in the support-header helpers
// (load_account_abi, create_contract_action_transaction) would fail every run.
struct fixture_without_host_budget : testing::tester {
   [[maybe_unused]] static constexpr auto abi_serializer_max_time = fc::microseconds(0);
};
}

BOOST_AUTO_TEST_SUITE(contract_test_support_tests)

BOOST_AUTO_TEST_CASE(action_fixture_validates_and_packs_without_host_deadline) { try {
   fixture_without_host_budget chain;
   const auto account = "abifixture"_n;
   chain.create_account(account);
   chain.set_abi(account, R"({
      "version":"sysio::abi/1.0",
      "structs":[{"name":"entry","base":"","fields":[{"name":"children","type":"entry[]"}]}],
      "actions":[{"name":"visit","type":"entry","ricardian_contract":""}]
   })");
   abi_serializer serializer;
   sysio_system::test_support::load_account_abi(chain, account, serializer);
   const auto input = fc::json::from_string(R"({"children":[{"children":[]}]})");
   const auto transaction = sysio_system::test_support::create_contract_action_transaction(
      chain, account, serializer, account, "visit"_n, input.get_object());
   BOOST_REQUIRE_EQUAL(transaction.actions.size(), 1u);
   const auto& action = transaction.actions.front();
   BOOST_CHECK_EQUAL(action.account, account);
   BOOST_CHECK_EQUAL(action.name, "visit"_n);
   BOOST_REQUIRE_EQUAL(action.authorization.size(), 1u);
   BOOST_CHECK_EQUAL(action.authorization.front().actor, account);
   BOOST_CHECK_EQUAL(action.authorization.front().permission, config::active_name);
   BOOST_CHECK(!transaction.signatures.empty());
   BOOST_CHECK_EQUAL(fc::to_hex(action.data), "0100");
   const auto decoded = serializer.binary_to_variant("entry", action.data,
                                                     abi_serializer::create_depth_yield_function());
   const auto& children = decoded.get_object()["children"].get_array();
   BOOST_REQUIRE_EQUAL(children.size(), 1u);
   BOOST_CHECK(children.front().get_object()["children"].get_array().empty());

   fc::variant nested = fc::mutable_variant_object()("children", fc::variants{});
   for (size_t depth = 0; depth <= abi_serializer::max_recursion_depth; ++depth)
      nested = fc::mutable_variant_object()("children", fc::variants{std::move(nested)});
   BOOST_CHECK_THROW(sysio_system::test_support::create_contract_action_transaction(
      chain, account, serializer, account, "visit"_n, nested.get_object()), abi_recursion_depth_exception);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
