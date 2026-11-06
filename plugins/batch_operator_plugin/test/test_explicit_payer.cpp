#include "../src/role_config.hpp"

#include <boost/test/unit_test.hpp>

#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/resource_limits_private.hpp>
#include <sysio/testing/tester.hpp>

#include <cstdint>

using namespace sysio::chain;
using namespace sysio::testing;
using sysio::batch_operator_detail::action_payer;
using sysio::batch_operator_detail::declared_authorization;
using sysio::batch_operator_detail::has_own_resources;

namespace {

/// A chain whose operator account starts as `sysio.system::newaccount` leaves one, with no CPU or NET. It pushes
/// `sysio::reqauth`, which, like the depot actions the plugin pushes, is a `sysio.*` contract's action that needs
/// only the role's authorization.
struct explicit_payer_fixture : tester {
   static constexpr account_name    operator_account = "batchop"_n;
   static constexpr permission_name underwrite       = "underwrite"_n;
   static constexpr action_name     setalimits       = "setalimits"_n;
   static constexpr action_name     reqauth          = "reqauth"_n;
   static constexpr int64_t         unlimited        = -1;
   static constexpr int64_t         no_weight        = 0;
   /// The ROA policy an operator holds before turning the option on.
   static constexpr auto policy_weight = "10.0000 SYS";
   static constexpr auto policy_ram    = "1.0000 SYS";

   /// NET and CPU billed to an account, read within one pending block.
   struct billed {
      uint64_t net = 0;
      uint64_t cpu = 0;
   };

   const permission_level active{operator_account, config::active_name};

   explicit_payer_fixture()
      : tester(setup_policy::full) {
      produce_block();
      create_account(operator_account, config::system_account_name, false, false, false, false);
      push_action(config::system_account_name, setalimits, config::system_account_name,
                  fc::mutable_variant_object()("account", operator_account)("ram_bytes", unlimited)(
                     "net_weight", no_weight)("cpu_weight", no_weight));
      produce_block();
   }

   /// Give the operator CPU and NET through an ROA policy.
   void add_policy() {
      add_roa_policy(NODE_DADDY, operator_account, policy_weight, policy_weight, policy_ram, 0, 0);
      produce_block();
   }

   /// Push `sysio::reqauth` for the operator declaring `level` as `payer` says, signed only by `key`.
   transaction_trace_ptr push_as(const permission_level& level, action_payer payer, const private_key_type& key) {
      return push_reqauth(operator_account, declared_authorization(level, payer), {key});
   }

   /// Whether the operator has CPU and NET of its own, by its limits on chain.
   bool can_pay() const {
      int64_t ram_bytes = 0, net_weight = 0, cpu_weight = 0;
      control->get_resource_limits_manager().get_account_limits(operator_account, ram_bytes, net_weight, cpu_weight);
      return has_own_resources(net_weight, cpu_weight);
   }

   billed billed_to(account_name account) const {
      const auto& usage =
         control->db().get<resource_limits::resource_object, resource_limits::by_owner>(account);
      return {.net = usage.net_usage.value_ex, .cpu = usage.cpu_usage.value_ex};
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(explicit_payer_tests)

/// Off, the contract pays, so an operator without CPU or NET still pushes and is billed nothing.
BOOST_FIXTURE_TEST_CASE(the_contract_pays_by_default, explicit_payer_fixture) try {
   BOOST_REQUIRE(!can_pay());
   const billed operator_before = billed_to(operator_account);
   const billed contract_before = billed_to(config::system_account_name);

   const transaction_trace_ptr trace =
      push_as(active, action_payer::contract, get_private_key(operator_account, "active"));
   BOOST_REQUIRE(!trace->action_traces.empty());
   BOOST_CHECK_EQUAL(config::system_account_name, trace->action_traces.front().act.payer());

   const billed operator_after = billed_to(operator_account);
   const billed contract_after = billed_to(config::system_account_name);
   BOOST_CHECK_EQUAL(operator_before.net, operator_after.net);
   BOOST_CHECK_EQUAL(operator_before.cpu, operator_after.cpu);
   BOOST_CHECK_GT(contract_after.net, contract_before.net);
   BOOST_CHECK_GT(contract_after.cpu, contract_before.cpu);
} FC_LOG_AND_RETHROW()

/// An operator without CPU or NET cannot pay, which is why the plugin refuses to start that way.
BOOST_FIXTURE_TEST_CASE(an_operator_without_resources_cannot_pay, explicit_payer_fixture) try {
   BOOST_REQUIRE(!can_pay());
   BOOST_CHECK_THROW(push_as(active, action_payer::account, get_private_key(operator_account, "active")),
                     resource_exhausted_exception);
} FC_LOG_AND_RETHROW()

/// With an ROA policy, the explicitly paid push passes authorization with only the active key's signature, and the
/// operator pays instead of the contract.
BOOST_FIXTURE_TEST_CASE(an_operator_with_a_policy_pays_for_its_pushes, explicit_payer_fixture) try {
   add_policy();
   BOOST_REQUIRE(can_pay());
   const billed operator_before = billed_to(operator_account);
   const billed contract_before = billed_to(config::system_account_name);

   const transaction_trace_ptr trace =
      push_as(active, action_payer::account, get_private_key(operator_account, "active"));
   BOOST_REQUIRE(!trace->action_traces.empty());
   BOOST_CHECK_EQUAL(operator_account, trace->action_traces.front().act.payer());

   const billed operator_after = billed_to(operator_account);
   const billed contract_after = billed_to(config::system_account_name);
   BOOST_CHECK_GT(operator_after.net, operator_before.net);
   BOOST_CHECK_GT(operator_after.cpu, operator_before.cpu);
   BOOST_CHECK_EQUAL(contract_before.net, contract_after.net);
   BOOST_CHECK_EQUAL(contract_before.cpu, contract_after.cpu);
} FC_LOG_AND_RETHROW()

/// The underwriter declares a dedicated permission linked to its actions: the marker pairs with it, and that
/// permission's key alone signs.
BOOST_FIXTURE_TEST_CASE(a_linked_permission_pays_without_the_active_key, explicit_payer_fixture) try {
   add_policy();
   set_authority(operator_account, underwrite, authority(get_public_key(operator_account, underwrite.to_string())),
                 config::active_name);
   link_authority(operator_account, config::system_account_name, underwrite, reqauth);
   produce_block();
   const billed operator_before = billed_to(operator_account);
   const billed contract_before = billed_to(config::system_account_name);

   push_as({operator_account, underwrite}, action_payer::account,
           get_private_key(operator_account, underwrite.to_string()));

   const billed operator_after = billed_to(operator_account);
   const billed contract_after = billed_to(config::system_account_name);
   BOOST_CHECK_GT(operator_after.cpu, operator_before.cpu);
   BOOST_CHECK_EQUAL(contract_before.cpu, contract_after.cpu);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
