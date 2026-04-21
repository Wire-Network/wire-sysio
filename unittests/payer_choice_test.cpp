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
        c.add_roa_policy(bob_account, contract_account, "10.0000 SYS", "10.0000 SYS", "1.1000 SYS", 0, 0);
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

BOOST_AUTO_TEST_SUITE_END()
