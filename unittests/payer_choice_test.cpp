#include <algorithm>
#include <test_contracts.hpp>

#include <sysio/chain/config.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/testing/chainbase_fixture.hpp>

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>

using namespace sysio::testing;
using namespace sysio::chain;

BOOST_AUTO_TEST_SUITE(payer_choice_test)

    /**
     * KV billing semantics: RAM is always charged to the contract (receiver) account,
     * regardless of the payer argument passed by the contract.  The payer parameter in
     * kv_multi_index::emplace / modify is accepted for API compatibility but ignored.
     *
     * This test verifies:
     *  1. A contract with insufficient RAM cannot store data (resource_exhausted_exception).
     *  2. After granting the contract more RAM, storage succeeds and RAM is billed to the contract.
     *  3. The caller (alice) is not billed even when named as payer.
     */
    BOOST_AUTO_TEST_CASE(kv_bills_contract_not_payer) try {
        tester c(setup_policy::full);
        c.produce_block();

        const auto &tester1_account = account_name("tester1");
        const auto &alice_account = account_name("alice");
        const auto &bob_account = account_name("bob");

        c.create_accounts({tester1_account, alice_account, bob_account}, false, true, false, true);
        c.produce_block();

        // Give tester1 just enough RAM to load the contract but not enough for data storage.
        c.register_node_owner(bob_account, 2);
        c.add_roa_policy(bob_account, tester1_account, "1.0000 SYS", "1.0000 SYS", "0.0975 SYS", 0, 0);
        c.produce_block();

        c.set_contract(tester1_account,
                       test_contracts::ram_restrictions_test_wasm(),
                       test_contracts::ram_restrictions_test_abi());
        c.produce_block();

        auto tester1_ram_usage = c.control->get_resource_limits_manager().get_account_ram_usage(tester1_account);
        auto alice_ram_usage   = c.control->get_resource_limits_manager().get_account_ram_usage(alice_account);

        // 1. Contract has no spare RAM — storing data must fail regardless of named payer.
        BOOST_REQUIRE_EXCEPTION(
            c.push_action(tester1_account, "setdata"_n, alice_account, mutable_variant_object()
                ("len1", 10000)
                ("len2", 0)
                ("payer", alice_account)   // KV ignores this; bills tester1
            ),
            resource_exhausted_exception,
            fc_exception_message_contains("tester1 has insufficient ram")
        );

        // RAM unchanged for both accounts.
        BOOST_REQUIRE_EQUAL(c.control->get_resource_limits_manager().get_account_ram_usage(tester1_account), tester1_ram_usage);
        BOOST_REQUIRE_EQUAL(c.control->get_resource_limits_manager().get_account_ram_usage(alice_account), alice_ram_usage);

        // 2. Grant tester1 more RAM via a new ROA policy from NODE_DADDY.
        c.add_roa_policy(c.NODE_DADDY, tester1_account, "100.0000 SYS", "100.0000 SYS", "100.0000 SYS", 0, 0);
        c.produce_block();

        // Now storage succeeds — RAM billed to tester1 (the contract), not alice.
        c.push_action(tester1_account, "setdata"_n, alice_account, mutable_variant_object()
            ("len1", 100)
            ("len2", 0)
            ("payer", alice_account)   // KV ignores this; bills tester1
        );
        c.produce_block();

        // 3. Verify tester1's RAM increased and alice's did not.
        BOOST_REQUIRE_GT(c.control->get_resource_limits_manager().get_account_ram_usage(tester1_account), tester1_ram_usage);
        BOOST_REQUIRE_EQUAL(c.control->get_resource_limits_manager().get_account_ram_usage(alice_account), alice_ram_usage);

    } FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
