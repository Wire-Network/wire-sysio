#include <fc/io/raw.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/action.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <string>
#include <test_contracts.hpp>
#include <vector>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using mvo = fc::mutable_variant_object;

namespace {

constexpr account_name alice = "alice"_n;
constexpr account_name bob = "bob"_n;
constexpr account_name wrap_account = "sysio.wrap"_n;

/// The error both rejections share.
const std::string implicit_only = "runs only as the implicit transaction at the start of a block";

/** The onblock action as the implicit transaction carries it, for the block after @p chain's head. */
action onblock_action(const tester& chain) {
   return action(
      std::vector<permission_level>{
         {config::system_account_name, config::active_name}
   },
      config::system_account_name, config::onblock_action_name, fc::raw::pack(chain.control->head().header()));
}

/** sysio::reqauth by @p account, authorized by its active permission. */
action reqauth_action(account_name account) {
   return action(
      std::vector<permission_level>{
         {account, config::active_name}
   },
      config::system_account_name, "reqauth"_n, fc::raw::pack(account));
}

/** @p actions in one transaction signed by sysio and alice. */
signed_transaction signed_by_sysio_and_alice(tester& chain, std::vector<action> actions) {
   signed_transaction trx;
   trx.actions = std::move(actions);
   chain.set_transaction_headers(trx);
   trx.sign(tester::get_private_key(config::system_account_name, "active"), chain.control->get_chain_id());
   trx.sign(tester::get_private_key(alice, "active"), chain.control->get_chain_id());
   return trx;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(onblock_tests)

// The implicit transaction still runs onblock at the start of every block, and its trace is recognized.
BOOST_AUTO_TEST_CASE(implicit_onblock_runs_every_block) try {
   tester chain;
   for (int i = 0; i < 3; ++i) {
      const produce_block_result_t produced = chain.produce_block_ex();
      BOOST_REQUIRE(produced.onblock_trace);
      BOOST_CHECK(produced.onblock_trace->receipt);
      BOOST_CHECK(!produced.onblock_trace->except);
      BOOST_CHECK(is_onblock(*produced.onblock_trace));
   }
}
FC_LOG_AND_RETHROW()

// A signed transaction may not carry onblock, first or later, even with sysio's signature.
BOOST_AUTO_TEST_CASE(signed_transaction_cannot_run_onblock) try {
   tester chain;
   chain.create_accounts({alice});
   chain.produce_block();

   signed_transaction first = signed_by_sysio_and_alice(chain, {onblock_action(chain), reqauth_action(alice)});
   BOOST_CHECK_EXCEPTION(chain.push_transaction(first), action_validate_exception,
                         fc_exception_message_contains(implicit_only));

   signed_transaction later = signed_by_sysio_and_alice(chain, {reqauth_action(alice), onblock_action(chain)});
   BOOST_CHECK_EXCEPTION(chain.push_transaction(later), action_validate_exception,
                         fc_exception_message_contains(implicit_only));

   // The rest of the transaction, without onblock, goes through.
   signed_transaction without;
   without.actions.push_back(reqauth_action(alice));
   chain.set_transaction_headers(without);
   without.sign(tester::get_private_key(alice, "active"), chain.control->get_chain_id());
   chain.push_transaction(without);
   chain.produce_block();
}
FC_LOG_AND_RETHROW()

// A privileged contract may not send onblock inline: sysio.wrap executes a wrapped transaction's actions inline with
// their declared authorizations, so it can send onblock as sysio, and the chain rejects it.
BOOST_AUTO_TEST_CASE(inline_onblock_is_rejected) try {
   tester chain;
   chain.create_accounts({alice, bob, wrap_account});
   chain.set_code(wrap_account, test_contracts::sysio_wrap_wasm());
   chain.set_abi(wrap_account, test_contracts::sysio_wrap_abi().c_str());
   chain.set_privileged(wrap_account);
   chain.produce_block();

   const auto wrapped_exec = [&](const action& wrapped_action) {
      transaction wrapped;
      chain.set_transaction_headers(wrapped);
      wrapped.actions.push_back(wrapped_action);
      signed_transaction outer;
      chain.set_transaction_headers(outer);
      outer.actions.push_back(chain.get_action(
         wrap_account, "exec"_n,
         {
            {bob,          config::sysio_payer_name},
            {bob,          config::active_name     },
            {wrap_account, config::active_name     }
      },
         mvo()("executer", bob)("trx", wrapped)));
      outer.sign(tester::get_private_key(bob, "active"), chain.control->get_chain_id());
      outer.sign(tester::get_private_key(wrap_account, "active"), chain.control->get_chain_id());
      return outer;
   };

   signed_transaction inline_onblock = wrapped_exec(onblock_action(chain));
   BOOST_CHECK_EXCEPTION(chain.push_transaction(inline_onblock), action_validate_exception,
                         fc_exception_message_contains("never inline"));

   // sysio.wrap still executes other actions inline.
   signed_transaction inline_reqauth = wrapped_exec(reqauth_action(alice));
   chain.push_transaction(inline_reqauth);
   chain.produce_block();
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(is_onblock_action_names_only_the_system_onblock) {
   const std::vector<permission_level> sysio_active{
      {config::system_account_name, config::active_name}
   };
   BOOST_CHECK(is_onblock_action(action(sysio_active, config::system_account_name, config::onblock_action_name, {})));
   // Another contract's onblock, or another system action, is not the system onblock.
   BOOST_CHECK(!is_onblock_action(action(sysio_active, alice, config::onblock_action_name, {})));
   BOOST_CHECK(!is_onblock_action(action(sysio_active, config::system_account_name, "reqauth"_n, {})));
}

BOOST_AUTO_TEST_SUITE_END()
