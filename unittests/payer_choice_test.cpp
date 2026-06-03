#include <algorithm>
#include <test_contracts.hpp>

#include <sysio/chain/config.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/testing/chainbase_fixture.hpp>

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>

using namespace sysio::testing;
using namespace sysio::chain;

namespace {

/**
 * Shared setup for the kv RAM-billing route tests: a contract account funded by a node
 * owner, plus two independently-funded payer accounts (alice, carol). Each test deploys the
 * contract it needs and asserts which account a given kv route bills. Keeping the accounts
 * separate from the contract is what makes a mis-billing visible -- when every row is billed
 * to the contract, a payer bug hides (which is exactly how the same_payer bug shipped).
 */
struct kv_billing_fixture : tester {
   const account_name CONTRACT = "kvbilltest"_n;
   const account_name ALICE    = "alice"_n;
   const account_name CAROL    = "carol"_n;
   const account_name OWNER    = "bob"_n;

   kv_billing_fixture() : tester(setup_policy::full) {
      produce_block();
      create_accounts({CONTRACT, ALICE, CAROL, OWNER}, false, true, false, true);
      produce_block();
      register_node_owner(OWNER, 2);
      add_roa_policy(OWNER, CONTRACT, "10.0000 SYS", "10.0000 SYS", "1.2000 SYS", 0, 0);
      add_roa_policy(NODE_DADDY, ALICE, "100.0000 SYS", "100.0000 SYS", "100.0000 SYS", 0, 0);
      add_roa_policy(NODE_DADDY, CAROL, "100.0000 SYS", "100.0000 SYS", "100.0000 SYS", 0, 0);
      produce_block();
   }

   int64_t ram(const account_name &a) {
      return control->get_resource_limits_manager().get_account_ram_usage(a);
   }
   void deploy_kv_api() {
      set_code(CONTRACT, test_contracts::test_kv_api_wasm());
      set_abi(CONTRACT, test_contracts::test_kv_api_abi().c_str());
      produce_block();
   }
   void deploy_ram_restrictions() {
      set_contract(CONTRACT, test_contracts::ram_restrictions_test_wasm(),
                   test_contracts::ram_restrictions_test_abi());
      produce_block();
   }
   // Authorize `p` both to run the action and to be named as a kv payer.
   static std::vector<permission_level> pay(const account_name &p) {
      return {{p, config::sysio_payer_name}, {p, config::active_name}};
   }
};

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(payer_choice_test)

    /**
     * KV billing semantics: the payer parameter flows through to kv_set.
     * When a contract names another account as payer, that account must
     * authorize via sysio_payer_name permission, and RAM is charged to them.
     * Without payer authorization, the transaction fails with
     * unsatisfied_authorization.
     */
    BOOST_AUTO_TEST_CASE(kv_payer_billing) try {
        tester c(setup_policy::full);
        c.produce_block();

        const auto &tester1_account = account_name("tester1");
        const auto &alice_account = account_name("alice");
        const auto &bob_account = account_name("bob");

        c.create_accounts({tester1_account, alice_account, bob_account}, false, true, false, true);
        c.produce_block();

        c.register_node_owner(bob_account, 2);
        c.add_roa_policy(bob_account, tester1_account, "1.0000 SYS", "1.0000 SYS", "0.1100 SYS", 0, 0);
        c.add_roa_policy(c.NODE_DADDY, alice_account, "100.0000 SYS", "100.0000 SYS", "100.0000 SYS", 0, 0);
        c.produce_block();

        c.set_contract(tester1_account,
                       test_contracts::ram_restrictions_test_wasm(),
                       test_contracts::ram_restrictions_test_abi());
        c.produce_block();

        auto tester1_ram_usage = c.control->get_resource_limits_manager().get_account_ram_usage(tester1_account);
        auto alice_ram_usage   = c.control->get_resource_limits_manager().get_account_ram_usage(alice_account);

        // 1. Without alice's payer authorization, naming her as payer fails.
        BOOST_REQUIRE_EXCEPTION(
            c.push_action(tester1_account, "setdata"_n, alice_account, mutable_variant_object()
                ("len1", 100)
                ("len2", 0)
                ("payer", alice_account)
            ),
            unsatisfied_authorization,
            fc_exception_message_contains("Missing sysio.payer")
        );

        // 2. With alice's payer authorization, RAM is billed to alice.
        vector<permission_level> levels = {{alice_account, config::sysio_payer_name}, {alice_account, config::active_name}};
        c.push_action(tester1_account, "setdata"_n, levels, mutable_variant_object()
            ("len1", 100)
            ("len2", 0)
            ("payer", alice_account)
        );
        c.produce_block();

        // 3. Alice's RAM increased, tester1's did not.
        BOOST_REQUIRE_GT(c.control->get_resource_limits_manager().get_account_ram_usage(alice_account), alice_ram_usage);
        BOOST_REQUIRE_EQUAL(c.control->get_resource_limits_manager().get_account_ram_usage(tester1_account), tester1_ram_usage);

    } FC_LOG_AND_RETHROW();

    /**
     * Secondary index payer billing: kv_idx_store must bill the specified
     * payer, not the contract (receiver).  This test verifies that when a
     * contract calls kv_set + kv_idx_store with alice as the payer,
     * alice's RAM increases and the contract's RAM does not.
     */
    BOOST_AUTO_TEST_CASE(kv_idx_payer_billing) try {
        tester c(setup_policy::full);
        c.produce_block();

        const auto& contract_account = account_name("kvtest");
        const auto& alice_account    = account_name("alice");
        const auto& bob_account      = account_name("bob");

        c.create_accounts({contract_account, alice_account, bob_account}, false, true, false, true);
        c.produce_block();

        // ROA: give kvtest enough RAM for contract deployment (WASM × 10 multiplier),
        // alice enough for data storage.
        c.register_node_owner(bob_account, 2);
        c.add_roa_policy(bob_account, contract_account, "10.0000 SYS", "10.0000 SYS", "1.2000 SYS", 0, 0);
        c.add_roa_policy(c.NODE_DADDY, alice_account, "100.0000 SYS", "100.0000 SYS", "100.0000 SYS", 0, 0);
        c.produce_block();

        c.set_code(contract_account, test_contracts::test_kv_api_wasm());
        c.set_abi(contract_account, test_contracts::test_kv_api_abi().c_str());
        c.produce_block();

        auto contract_ram_before = c.control->get_resource_limits_manager().get_account_ram_usage(contract_account);
        auto alice_ram_before    = c.control->get_resource_limits_manager().get_account_ram_usage(alice_account);

        // Push tstidxpayer with alice's payer authorization
        vector<permission_level> levels = {
            {alice_account, config::sysio_payer_name},
            {alice_account, config::active_name}
        };
        c.push_action(contract_account, "tstidxpayer"_n, levels,
                       mutable_variant_object()("payer", alice_account));
        c.produce_block();

        auto contract_ram_after = c.control->get_resource_limits_manager().get_account_ram_usage(contract_account);
        auto alice_ram_after    = c.control->get_resource_limits_manager().get_account_ram_usage(alice_account);

        // Alice's RAM must have increased (she paid for both primary + secondary)
        BOOST_REQUIRE_GT(alice_ram_after, alice_ram_before);

        // Contract's RAM must not have increased
        BOOST_REQUIRE_EQUAL(contract_ram_after, contract_ram_before);

        int64_t alice_delta = alice_ram_after - alice_ram_before;
        BOOST_TEST_MESSAGE("kv_idx_payer_billing: alice RAM delta = " << alice_delta);

        // Sanity: delta must cover both kv_object overhead and kv_index_object overhead
        int64_t min_expected = static_cast<int64_t>(config::billable_size_v<kv_object>)
                             + static_cast<int64_t>(config::billable_size_v<kv_index_object>);
        BOOST_REQUIRE_GE(alice_delta, min_expected);

    } FC_LOG_AND_RETHROW();

    /**
     * Regression: `same_payer` on a kv UPDATE must KEEP the row's existing payer,
     * not silently move the row's RAM onto the receiver (contract).
     *
     * `same_payer` is name{} (value 0). The CDT passes payer.value == 0 to kv_set on a
     * modify(same_payer, ...). The host MUST resolve 0 to the row's existing payer on an
     * update; resolving it to the receiver re-bills any row whose payer differs from the
     * contract account (e.g. a row billed to a separate `payer` account, or to the `sysio`
     * pool). This was latent for as long as every row happened to be billed to the contract
     * (so same_payer -> receiver coincided with the real payer) and surfaced only once rows
     * were billed elsewhere.
     *
     * Setup mirrors kv_payer_billing: insert a row billed to alice, then modify it with
     * same_payer at the same size (zero RAM delta). Correct behavior leaves alice's and the
     * contract's RAM unchanged. The pre-fix behavior moved the row's RAM from alice onto
     * tester1, which both assertions below catch.
     */
    BOOST_AUTO_TEST_CASE(kv_same_payer_update_keeps_payer) try {
        tester c(setup_policy::full);
        c.produce_block();

        const auto &tester1_account = account_name("tester1");
        const auto &alice_account = account_name("alice");
        const auto &bob_account = account_name("bob");

        c.create_accounts({tester1_account, alice_account, bob_account}, false, true, false, true);
        c.produce_block();

        c.register_node_owner(bob_account, 2);
        c.add_roa_policy(bob_account, tester1_account, "1.0000 SYS", "1.0000 SYS", "0.1100 SYS", 0, 0);
        c.add_roa_policy(c.NODE_DADDY, alice_account, "100.0000 SYS", "100.0000 SYS", "100.0000 SYS", 0, 0);
        c.produce_block();

        c.set_contract(tester1_account,
                       test_contracts::ram_restrictions_test_wasm(),
                       test_contracts::ram_restrictions_test_abi());
        c.produce_block();

        auto ram = [&](const account_name &a) {
            return c.control->get_resource_limits_manager().get_account_ram_usage(a);
        };

        const auto alice_before   = ram(alice_account);
        const auto tester1_before = ram(tester1_account);

        // 1. Insert a row billed to alice (alice authorizes as payer).
        vector<permission_level> alice_levels = {{alice_account, config::sysio_payer_name},
                                                 {alice_account, config::active_name}};
        c.push_action(tester1_account, "setdata"_n, alice_levels, mutable_variant_object()
            ("len1", 100)
            ("len2", 0)
            ("payer", alice_account)
        );
        c.produce_block();

        const auto alice_after_insert   = ram(alice_account);
        const auto tester1_after_insert = ram(tester1_account);
        BOOST_REQUIRE_GT(alice_after_insert, alice_before);              // alice paid for the row
        BOOST_REQUIRE_EQUAL(tester1_after_insert, tester1_before);       // contract did not

        // 2. Update the SAME row with same_payer (name{}), same size -> zero RAM delta.
        //    No payer authorization is supplied: same_payer keeps the existing payer and does
        //    not increase her usage, so it needs none. Signed by tester1 to run the action.
        c.push_action(tester1_account, "setdata"_n, tester1_account, mutable_variant_object()
            ("len1", 100)
            ("len2", 0)
            ("payer", account_name{})
        );
        c.produce_block();

        // 3. The row's RAM stays with alice; the receiver (contract) is NOT billed.
        //    Pre-fix, the modify moved the row from alice onto tester1 -- both checks catch it.
        BOOST_REQUIRE_EQUAL(ram(alice_account), alice_after_insert);
        BOOST_REQUIRE_EQUAL(ram(tester1_account), tester1_after_insert);

    } FC_LOG_AND_RETHROW();

    // =========================================================================
    // Complete kv RAM-billing route coverage. Every RAM-affecting kv route in
    // apply_context is exercised here with a payer distinct from the contract,
    // so that mis-billing to the receiver (the class of bug fixed in kv_set /
    // kv_idx_update) is caught on every route:
    //   kv_set insert       -> kv_payer_billing (explicit), probe payzero (=0)
    //   kv_set update        -> change (below), same_payer (above)
    //   kv_erase             -> refund row payer (below)
    //   kv_idx_store insert  -> kv_idx_payer_billing (explicit), payer=0 (below)
    //   kv_idx_update        -> change (below), same_payer (below)
    //   kv_idx_remove        -> refund entry payer (below)
    // =========================================================================

    // kv_set UPDATE with an explicit different payer moves the row's RAM from the
    // old payer to the new one (alice -> carol), never touching the contract.
    BOOST_FIXTURE_TEST_CASE(kv_set_update_payer_change_moves_billing, kv_billing_fixture) try {
        deploy_ram_restrictions();
        const auto alice0 = ram(ALICE), carol0 = ram(CAROL), c0 = ram(CONTRACT);

        push_action(CONTRACT, "setdata"_n, pay(ALICE), mutable_variant_object()
            ("len1", 100)("len2", 0)("payer", ALICE));
        produce_block();
        BOOST_REQUIRE_GT(ram(ALICE), alice0);

        push_action(CONTRACT, "setdata"_n, pay(CAROL), mutable_variant_object()
            ("len1", 100)("len2", 0)("payer", CAROL));
        produce_block();

        BOOST_REQUIRE_EQUAL(ram(ALICE), alice0);   // alice refunded
        BOOST_REQUIRE_GT(ram(CAROL), carol0);      // carol billed
        BOOST_REQUIRE_EQUAL(ram(CONTRACT), c0);    // contract never billed
    } FC_LOG_AND_RETHROW();

    // kv_erase refunds the row's payer (alice), not the receiver: insert billed to
    // alice then erase nets to zero for alice and leaves the contract untouched.
    BOOST_FIXTURE_TEST_CASE(kv_erase_refunds_row_payer, kv_billing_fixture) try {
        deploy_kv_api();
        const auto alice0 = ram(ALICE), c0 = ram(CONTRACT);

        push_action(CONTRACT, "bilkvera"_n, pay(ALICE), mutable_variant_object()("pa", ALICE));
        produce_block();

        BOOST_REQUIRE_EQUAL(ram(ALICE), alice0);   // billed by insert, refunded by erase
        BOOST_REQUIRE_EQUAL(ram(CONTRACT), c0);
    } FC_LOG_AND_RETHROW();

    // kv_idx_store with payer == {} bills the receiver (contract) for both the
    // primary row and the secondary entry; alice is uninvolved.
    BOOST_FIXTURE_TEST_CASE(kv_idx_store_payer_zero_bills_receiver, kv_billing_fixture) try {
        deploy_kv_api();
        const auto alice0 = ram(ALICE), c0 = ram(CONTRACT);

        push_action(CONTRACT, "tstidxpayer"_n, CONTRACT,
                    mutable_variant_object()("payer", account_name{}));
        produce_block();

        BOOST_REQUIRE_GT(ram(CONTRACT), c0);       // receiver billed (no payer named)
        BOOST_REQUIRE_EQUAL(ram(ALICE), alice0);
    } FC_LOG_AND_RETHROW();

    // kv_idx_update with an explicit different payer moves the secondary entry's
    // RAM from the old payer to the new one. The chain forbids naming two distinct
    // payers in one transaction, so the entry is first stored billed to the receiver
    // (payer {}), then updated billed to carol -- exercising the move branch
    // (payer != old_payer) with a single named payer.
    BOOST_FIXTURE_TEST_CASE(kv_idx_update_payer_change_moves_billing, kv_billing_fixture) try {
        deploy_kv_api();
        const auto carol0 = ram(CAROL), c0 = ram(CONTRACT);

        push_action(CONTRACT, "bilidxchg"_n, pay(CAROL),
                    mutable_variant_object()("pa", account_name{})("pb", CAROL));
        produce_block();

        BOOST_REQUIRE_GT(ram(CAROL), carol0);      // entry moved onto carol
        BOOST_REQUIRE_EQUAL(ram(CONTRACT), c0);    // contract billed by store, refunded by update
    } FC_LOG_AND_RETHROW();

    // kv_idx_update with same_payer ({}) keeps the secondary entry billed to alice
    // instead of moving it onto the receiver -- the secondary-index analog of the
    // bug fixed in kv_set, and the path that overflowed sysio.msgch.
    BOOST_FIXTURE_TEST_CASE(kv_idx_update_same_payer_keeps_payer, kv_billing_fixture) try {
        deploy_kv_api();
        const auto alice0 = ram(ALICE), c0 = ram(CONTRACT);

        push_action(CONTRACT, "bilidxchg"_n, pay(ALICE),
                    mutable_variant_object()("pa", ALICE)("pb", account_name{}));
        produce_block();

        BOOST_REQUIRE_GT(ram(ALICE), alice0);      // alice still billed for the entry
        BOOST_REQUIRE_EQUAL(ram(CONTRACT), c0);    // receiver NOT billed (regression check)
    } FC_LOG_AND_RETHROW();

    // kv_idx_remove refunds the secondary entry's payer (alice), not the receiver.
    BOOST_FIXTURE_TEST_CASE(kv_idx_remove_refunds_entry_payer, kv_billing_fixture) try {
        deploy_kv_api();
        const auto alice0 = ram(ALICE), c0 = ram(CONTRACT);

        push_action(CONTRACT, "bilidxrm"_n, pay(ALICE), mutable_variant_object()("pa", ALICE));
        produce_block();

        BOOST_REQUIRE_EQUAL(ram(ALICE), alice0);   // stored then removed -> net zero
        BOOST_REQUIRE_EQUAL(ram(CONTRACT), c0);
    } FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
