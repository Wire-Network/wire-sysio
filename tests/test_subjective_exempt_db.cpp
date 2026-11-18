#include <boost/test/unit_test.hpp>

#include <sysio/chain/controller.hpp>
#include <sysio/chain/subjective_billing.hpp>
#include <sysio/producer_plugin/subjective_exempt_db.hpp>
#include <sysio_system_tester.hpp>

#include <fc/variant_object.hpp>

#include <string_view>
#include <unordered_set>

BOOST_AUTO_TEST_SUITE(subjective_exempt_db_tests)

using namespace sysio::testing;
using namespace sysio;

namespace {

using account_set = std::unordered_set<chain::account_name>;

const account_name opreg_account = "sysio.opreg"_n;
const account_name bond_account  = "sysio.bond"_n;
const account_name chalg_account = "sysio.chalg"_n;

/// A system contract chain with sysio.opreg, whose irreversible blocks drive `exempt_db` as producer_plugin does.
struct exempt_db_tester : sysio_system::sysio_system_tester {
   subjective_exempt_db                exempt_db;
   boost::signals2::scoped_connection lib_connection;

   exempt_db_tester() {
      lib_connection = control->irreversible_block().connect([&](const chain::block_signal_params& t) {
         const auto& [block, id] = t;
         exempt_db.on_irreversible_block(block, *control);
      });
      deploy_opreg_once();
   }

   /// Registers `account` in sysio.opreg as an operator of `type`, the ABI spelling of an OperatorType.
   void register_operator(account_name account, std::string_view type, bool is_bootstrapped) {
      base_tester::push_action(opreg_account, "regoperator"_n, opreg_account,
                               mvo()("account", account)("type", type)("is_bootstrapped", is_bootstrapped));
   }

   void terminate_operator(account_name account) {
      base_tester::push_action(opreg_account, "terminate"_n, opreg_account,
                               mvo()("account", account)("reason", "test"));
   }

   /// Slashes `account` as sysio.chalg, the only account sysio.opreg takes a slash from.
   void slash_operator(account_name account) {
      base_tester::push_action(opreg_account, "slash"_n, chalg_account, mvo()("account", account)("reason", "test"));
   }

   /// Creates the sysio.* account `account` with the NET and CPU to sign a transaction, which it is created without.
   void create_system_signer(account_name account) {
      create_account(account, config::system_account_name, false, false, false, true);
      BOOST_TEST_REQUIRE(success() == push_action(config::system_account_name, "setacctnet"_n,
                                                  mvo()("account", account)("net_weight", int64_t{1'000'000})));
      BOOST_TEST_REQUIRE(success() == push_action(config::system_account_name, "setacctcpu"_n,
                                                  mvo()("account", account)("cpu_weight", int64_t{1'000'000})));
   }

   action_result add_exempt(account_name signer, account_name account) {
      return push_action(signer, "addsubjex"_n, mvo()("account", account));
   }

   action_result remove_exempt(account_name signer, account_name account) {
      return push_action(signer, "delsubjex"_n, mvo()("account", account));
   }

   /// The accounts chain state exempts now, read directly.
   account_set chain_exempt() const { return subjective_exempt_db::load(control->kv()); }

   /// The accounts subjective billing holds as exempt, as of the last reload.
   const account_set& billing_exempt() const { return control->get_subjective_billing().get_chain_exempt_accounts(); }

   /// Produces enough blocks for a reload to see everything pushed so far.
   void produce_past_reload() { produce_blocks(subjective_exempt_db::refresh_interval + 3); }
};

} // anonymous namespace

// The list's actions: who may add and remove, and what they do to the table.
BOOST_FIXTURE_TEST_CASE(list_actions, exempt_db_tester) { try {
   create_accounts({"alice"_n, "bob"_n});
   create_system_signer(bond_account); // sysio.bond adds with an inline action; here it signs instead
   produce_block();
   BOOST_TEST(chain_exempt().empty());

   BOOST_TEST_REQUIRE(success() == add_exempt(config::system_account_name, "alice"_n));
   BOOST_TEST((chain_exempt() == account_set{"alice"_n}));
   // adding a listed account changes nothing
   BOOST_TEST_REQUIRE(success() == add_exempt(config::system_account_name, "alice"_n));
   BOOST_TEST((chain_exempt() == account_set{"alice"_n}));

   // sysio.bond may add, as it does for an underwriter's first bond, but not remove
   BOOST_TEST_REQUIRE(success() == add_exempt(bond_account, "bob"_n));
   BOOST_TEST((chain_exempt() == account_set{"alice"_n, "bob"_n}));
   BOOST_TEST(error("missing authority of sysio") == remove_exempt(bond_account, "bob"_n));

   // nobody else may add, and only an existing account is added
   BOOST_TEST(error("missing authority of sysio") == add_exempt("alice"_n, "alice"_n));
   BOOST_TEST(wasm_assert_msg("account does not exist") == add_exempt(config::system_account_name, "nobody"_n));

   BOOST_TEST_REQUIRE(success() == remove_exempt(config::system_account_name, "alice"_n));
   BOOST_TEST((chain_exempt() == account_set{"bob"_n}));
   BOOST_TEST(wasm_assert_msg("account is not on the subjective billing exempt list") ==
              remove_exempt(config::system_account_name, "alice"_n));
} FC_LOG_AND_RETHROW() }

// An active batch operator is exempt, as a bootstrapped one is from registration. One that only registered is not,
// nor is one terminated or slashed, nor any other operator type.
BOOST_FIXTURE_TEST_CASE(operators, exempt_db_tester) { try {
   create_accounts({"batch1"_n, "batch2"_n, "batch3"_n, "newbatch"_n, "uwriter"_n, "prod"_n, "challenger"_n});
   create_system_signer(chalg_account);
   produce_block();

   register_operator("batch1"_n, "OPERATOR_TYPE_BATCH", true);
   register_operator("batch2"_n, "OPERATOR_TYPE_BATCH", true);
   register_operator("batch3"_n, "OPERATOR_TYPE_BATCH", true);
   // registered after bootstrap: OPERATOR_STATUS_UNKNOWN until they hold the collateral minimum
   register_operator("newbatch"_n, "OPERATOR_TYPE_BATCH", false);
   register_operator("uwriter"_n, "OPERATOR_TYPE_UNDERWRITER", false);
   register_operator("prod"_n, "OPERATOR_TYPE_PRODUCER", true);
   register_operator("challenger"_n, "OPERATOR_TYPE_CHALLENGER", true);
   produce_block();
   BOOST_TEST((chain_exempt() == account_set{"batch1"_n, "batch2"_n, "batch3"_n}));

   terminate_operator("batch2"_n);
   produce_block();
   BOOST_TEST((chain_exempt() == account_set{"batch1"_n, "batch3"_n}));
   slash_operator("batch3"_n);
   produce_block();
   BOOST_TEST((chain_exempt() == account_set{"batch1"_n}));

   // a listed account is exempt whatever its operator status
   BOOST_TEST_REQUIRE(success() == add_exempt(config::system_account_name, "batch2"_n));
   BOOST_TEST_REQUIRE(success() == add_exempt(config::system_account_name, "uwriter"_n));
   BOOST_TEST((chain_exempt() == account_set{"batch1"_n, "batch2"_n, "uwriter"_n}));
} FC_LOG_AND_RETHROW() }

// Subjective billing takes the exempt accounts on the first irreversible block and at every reload after it.
BOOST_FIXTURE_TEST_CASE(reloads_into_subjective_billing, exempt_db_tester) { try {
   create_accounts({"alice"_n, "batch1"_n});
   produce_block();
   chain::subjective_billing& billing = control->get_mutable_subjective_billing();
   billing.set_disabled(false);

   BOOST_TEST_REQUIRE(success() == add_exempt(config::system_account_name, "alice"_n));
   BOOST_TEST(!billing.is_account_disabled("alice"_n)); // not until a reload
   produce_past_reload();
   BOOST_TEST((billing_exempt() == account_set{"alice"_n}));
   BOOST_TEST(billing.is_account_disabled("alice"_n));
   BOOST_TEST(!billing.is_account_disabled("batch1"_n));

   register_operator("batch1"_n, "OPERATOR_TYPE_BATCH", true);
   BOOST_TEST_REQUIRE(success() == remove_exempt(config::system_account_name, "alice"_n));
   produce_past_reload();
   BOOST_TEST((billing_exempt() == account_set{"batch1"_n}));
   BOOST_TEST(!billing.is_account_disabled("alice"_n));
   BOOST_TEST(billing.is_account_disabled("batch1"_n));

   // a node that starts later loads on its first irreversible block, without waiting for the interval
   subjective_exempt_db late_db;
   billing.set_chain_exempt_accounts({});
   const signed_block_ptr block = produce_block();
   BOOST_REQUIRE(block->block_num() % subjective_exempt_db::refresh_interval != 0);
   BOOST_REQUIRE(billing_exempt().empty()); // the fixture's own reload was not due
   late_db.on_irreversible_block(block, *control);
   BOOST_TEST((billing_exempt() == account_set{"batch1"_n}));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
