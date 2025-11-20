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
     * Test to ensure that a user without ram calling a non-whitelisted contract fails
     */
    BOOST_AUTO_TEST_CASE(no_resources_no_love) try {
        tester c(setup_policy::full);
        c.produce_block();

        const auto &tester1_account = account_name("tester1");
        const auto &alice_account = account_name("alice");
        const auto &bob_account = account_name("bob");

        ilog("Creating accounts: ${a}, ${b}, ${c}", ("a", tester1_account)("b", alice_account)("c", bob_account));
        c.create_accounts({tester1_account, alice_account, bob_account}, false, true, false, true);
        c.produce_block();

        auto ram_restrictions_wasm = test_contracts::ram_restrictions_test_wasm();
        ilog("Registering bob as node owner and assigning _just_ enough resources to tester1 to load the contract, wasm size ${w}",
            ("w", ram_restrictions_wasm.size()));
        c.register_node_owner(bob_account, 2);
        c.add_roa_policy(bob_account, tester1_account, "1.0000 SYS", "1.0000 SYS", "0.0828 SYS", 0, 0);
        c.produce_block();

        ilog("Setting code and ABI for ${a}", ("a", tester1_account));
        c.set_contract(tester1_account, ram_restrictions_wasm, test_contracts::ram_restrictions_test_abi());
        c.produce_block();

        auto tester1_ram_usage = c.control->get_resource_limits_manager().get_account_ram_usage(tester1_account);
        dlog("{account} ram usage: ${ram}", ("account", tester1_account)("ram", tester1_ram_usage));
        auto alice_ram_usage = c.control->get_resource_limits_manager().get_account_ram_usage(alice_account);
        dlog("{account} ram usage: ${ram}", ("account", alice_account)("ram", alice_ram_usage));

        ilog("No Resource Testing");

        ilog("Attempt by contract to charge itself with no resources should fail with resource_exhausted_exception");
        // If no exception thrown it could be that the size of the ram_restriction_test contract was reduced
        BOOST_REQUIRE_EXCEPTION(
            c.push_action(tester1_account, "setdata"_n, alice_account, mutable_variant_object()
                ("len1", 1000)
                ("len2", 0)
                ("payer", tester1_account)
            ),
            resource_exhausted_exception,
            fc_exception_message_contains("tester1 has insufficient ram")
        );

        // Ram usage should not change for tester1 or alice
        BOOST_REQUIRE_EQUAL(c.control->get_resource_limits_manager().get_account_ram_usage(tester1_account), tester1_ram_usage);
        BOOST_REQUIRE_EQUAL(c.control->get_resource_limits_manager().get_account_ram_usage(alice_account), alice_ram_usage);

        ilog("Now we register Alice as a node owner to assign her some resources");
        c.register_node_owner(alice_account, 2);
        c.produce_block();

        ilog("Attempt to charge node owner should fail without sysio.payer permission");
        BOOST_REQUIRE_EXCEPTION(
            c.push_action(tester1_account, "setdata"_n, {
                permission_level(alice_account, "active"_n)
                }, mutable_variant_object()
                ("len1", 1000)
                ("len2", 0)
                ("payer", alice_account)
            ),
            unsatisfied_authorization,
            fc_exception_message_contains("alice did not authorize")
        );
        c.produce_block();

        // Ram usage should still not change for tester1 or alice
        BOOST_REQUIRE_EQUAL(c.control->get_resource_limits_manager().get_account_ram_usage(tester1_account), tester1_ram_usage);
        BOOST_REQUIRE_EQUAL(c.control->get_resource_limits_manager().get_account_ram_usage(alice_account), alice_ram_usage);

        ilog("Attempt to charge node owner with sysio.payer permission should succeed");
        c.push_action(tester1_account, "setdata"_n, {
                          permission_level(alice_account, "active"_n),
                          permission_level(alice_account, config::sysio_payer_name)
                      }, mutable_variant_object()
                      ("len1", 1000)
                      ("len2", 0)
                      ("payer", alice_account)
        );
        c.produce_block();

        // Now ram usage should have stayed the same for tester1, but increased for alice
        BOOST_REQUIRE_EQUAL(c.control->get_resource_limits_manager().get_account_ram_usage(tester1_account), tester1_ram_usage);
        BOOST_REQUIRE_GT(c.control->get_resource_limits_manager().get_account_ram_usage(alice_account), alice_ram_usage);

    } FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
