#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/permission_link_object.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>

#include <contracts.hpp>

#include "sysio_system_tester.hpp"

/*
 * register test suite `ram_tests`
 */
BOOST_AUTO_TEST_SUITE(ram_tests)

BOOST_FIXTURE_TEST_CASE(new_account_ram_tests, sysio_system::sysio_system_tester) { try {
    produce_block();

    auto& resource_manager = control->get_mutable_resource_limits_manager();
    int64_t org_sys_ram_bytes, org_sys_net_limit, org_sys_cpu_limit;
    resource_manager.get_account_limits(config::system_account_name, org_sys_ram_bytes, org_sys_net_limit, org_sys_cpu_limit);
    int64_t org_sys_ram_usage = resource_manager.get_account_ram_usage(config::system_account_name);

    create_account("kevin"_n, config::system_account_name, false, false, false, false);

    int64_t ram_bytes, net_limit, cpu_limit;
    resource_manager.get_account_limits("kevin"_n, ram_bytes, net_limit, cpu_limit);
    // RAM account limit ram_bytes is total RAM usage allowed; not what is left
    BOOST_TEST( ram_bytes == newaccount_ram ); // RAM usage allowed to account

    int64_t ram_usage = resource_manager.get_account_ram_usage("kevin"_n);
    BOOST_TEST( ram_usage == 1004 ); // RAM used by "kevin" during account creation, provided by sysio

    int64_t sys_ram_bytes, sys_net_limit, sys_cpu_limit;
    resource_manager.get_account_limits(config::system_account_name, sys_ram_bytes, sys_net_limit, sys_cpu_limit);
    int64_t sys_ram_usage = resource_manager.get_account_ram_usage(config::system_account_name);
    BOOST_TEST( sys_ram_bytes == org_sys_ram_bytes - ram_bytes ); // sysio has ram_bytes less because they were provided to kevin
    BOOST_TEST( sys_ram_usage == org_sys_ram_usage ); // no change in what sysio used, the RAM was used by kevin

    produce_block(); // note onblock uses sysio to pay for blockinfo table entry

    //
    // user should be allowed to change keys
    //

    // Change active permission
    const auto new_active_priv_key = get_private_key("kevin"_n, "new_active");
    const auto new_active_pub_key = new_active_priv_key.get_public_key();
    set_authority("kevin"_n, "active"_n, authority(new_active_pub_key), "owner"_n);

    produce_blocks();

    // Change owner permission
    const auto new_owner_priv_key = get_private_key("kevin"_n, "new_owner");
    const auto new_owner_pub_key = new_owner_priv_key.get_public_key();
    set_authority("kevin"_n, "owner"_n, authority(new_owner_pub_key), {});

    produce_blocks();

    //
    // user should not have enough RAM to add additional permissions
    //
    auto spending_priv_key = get_private_key(name("kevin"), "spending");
    auto spending_pub_key = spending_priv_key.get_public_key();

    BOOST_CHECK_THROW( set_authority(name("kevin"), name("spending"), authority(spending_pub_key), name("active"),
                                     { permission_level{name("kevin"), name("active")} }, { new_active_priv_key }),
                       ram_usage_exceeded);

} FC_LOG_AND_RETHROW() }

// Verify ram paid by contract when contract uses get_self() for payer even when explicit payer is used
BOOST_FIXTURE_TEST_CASE(auth_ram_tests, validating_tester) { try {
    using mvo = fc::mutable_variant_object;
    produce_block();

    create_accounts( {"noauthtable"_n, "alice"_n} );
    // insert uses `emplace(get_self()`, noauthtable always pays for RAM
    set_code( "noauthtable"_n, test_contracts::no_auth_table_wasm() );
    set_abi( "noauthtable"_n, test_contracts::no_auth_table_abi() );
    produce_block();

    const resource_limits_manager& mgr = control->get_resource_limits_manager();
    auto noauthtable_cpu_limit0 = mgr.get_account_cpu_limit_ex("noauthtable"_n).first.current_used;
    auto alice_cpu_limit0 = mgr.get_account_cpu_limit_ex("alice"_n).first.current_used;
    auto noauthtable_net_limit0 = mgr.get_account_net_limit_ex("noauthtable"_n).first.current_used;
    auto alice_net_limit0 = mgr.get_account_net_limit_ex("alice"_n).first.current_used;
    auto noauthtable_ram_usage0 = mgr.get_account_ram_usage("noauthtable"_n);
    auto alice_ram_usage0 = mgr.get_account_ram_usage("alice"_n);

    vector<permission_level> noauthtable_auth {{"noauthtable"_n, config::active_name} };
    // noauthtable pays for CPU,NET
    push_action( "noauthtable"_n, "insert"_n, noauthtable_auth, mvo()("user", "a") ("id", 1) ("age", 10));

    auto noauthtable_cpu_limit1 = mgr.get_account_cpu_limit_ex("noauthtable"_n).first.current_used;
    auto alice_cpu_limit1 = mgr.get_account_cpu_limit_ex("alice"_n).first.current_used;
    auto noauthtable_net_limit1 = mgr.get_account_net_limit_ex("noauthtable"_n).first.current_used;
    auto alice_net_limit1 = mgr.get_account_net_limit_ex("alice"_n).first.current_used;
    auto noauthtable_ram_usage1 = mgr.get_account_ram_usage("noauthtable"_n);
    auto alice_ram_usage1 = mgr.get_account_ram_usage("alice"_n);

    BOOST_TEST(noauthtable_cpu_limit0 < noauthtable_cpu_limit1);
    BOOST_TEST(alice_cpu_limit0 == alice_cpu_limit1);
    BOOST_TEST(noauthtable_net_limit0 < noauthtable_net_limit1);
    BOOST_TEST(alice_net_limit0 == alice_net_limit1);
    BOOST_TEST(noauthtable_ram_usage0 < noauthtable_ram_usage1);
    BOOST_TEST(alice_ram_usage0 == alice_ram_usage1);

    vector<permission_level> alice_auth {{"alice"_n, config::active_name} };
    // noauthtable still pays for CPU,NET
    push_action( "noauthtable"_n, "insert"_n, alice_auth, mvo()("user", "b") ("id", 1) ("age", 10));

    auto noauthtable_cpu_limit2 = mgr.get_account_cpu_limit_ex("noauthtable"_n).first.current_used;
    auto alice_cpu_limit2 = mgr.get_account_cpu_limit_ex("alice"_n).first.current_used;
    auto noauthtable_net_limit2 = mgr.get_account_net_limit_ex("noauthtable"_n).first.current_used;
    auto alice_net_limit2 = mgr.get_account_net_limit_ex("alice"_n).first.current_used;
    auto noauthtable_ram_usage2 = mgr.get_account_ram_usage("noauthtable"_n);
    auto alice_ram_usage2 = mgr.get_account_ram_usage("alice"_n);

    BOOST_TEST(noauthtable_cpu_limit1 < noauthtable_cpu_limit2);
    BOOST_TEST(alice_cpu_limit1 == alice_cpu_limit2);
    BOOST_TEST(noauthtable_net_limit1 < noauthtable_net_limit2);
    BOOST_TEST(alice_net_limit1 == alice_net_limit2);
    BOOST_TEST(noauthtable_ram_usage1 < noauthtable_ram_usage2);
    BOOST_TEST(alice_ram_usage1 == alice_ram_usage2);

    vector<permission_level> alice_explicit {{"alice"_n, config::sysio_payer_name}, {"alice"_n, config::active_name} };
    // alice pays for CPU,NET
    push_action( "noauthtable"_n, "insert"_n, alice_explicit, mvo()("user", "c") ("id", 1) ("age", 10));

    auto noauthtable_cpu_limit3 = mgr.get_account_cpu_limit_ex("noauthtable"_n).first.current_used;
    auto alice_cpu_limit3 = mgr.get_account_cpu_limit_ex("alice"_n).first.current_used;
    auto noauthtable_net_limit3 = mgr.get_account_net_limit_ex("noauthtable"_n).first.current_used;
    auto alice_net_limit3 = mgr.get_account_net_limit_ex("alice"_n).first.current_used;
    auto noauthtable_ram_usage3 = mgr.get_account_ram_usage("noauthtable"_n);
    auto alice_ram_usage3 = mgr.get_account_ram_usage("alice"_n);

    BOOST_TEST(noauthtable_cpu_limit2 == noauthtable_cpu_limit3);
    BOOST_TEST(alice_cpu_limit2 < alice_cpu_limit3);
    BOOST_TEST(noauthtable_net_limit2 == noauthtable_net_limit3);
    BOOST_TEST(alice_net_limit2 < alice_net_limit3);
    BOOST_TEST(noauthtable_ram_usage2 < noauthtable_ram_usage3);
    BOOST_TEST(alice_ram_usage2 == alice_ram_usage3);
} FC_LOG_AND_RETHROW() }

// ════════════════════════════════════════════════════════════════════════════
// Permission lifecycle RAM billing — exact-delta tests that verify the
// sysio_contract.cpp billing flows use billable_size_v<permission_object>,
// billable_size_v<permission_link_object>, and shared_authority::get_billable_size()
// correctly. These are the runtime complement to the compile-time
// static_asserts on those billable_size specializations.
// ════════════════════════════════════════════════════════════════════════════

BOOST_FIXTURE_TEST_CASE(linkauth_unlinkauth_ram_billing, validating_tester) { try {
    create_account("alice"_n);
    produce_block();

    // Alice needs a non-active permission to link.
    const auto spending_pub_key = get_public_key("alice"_n, "spending");
    set_authority("alice"_n, "spending"_n, authority(spending_pub_key), "active"_n);
    produce_block();

    const auto& rlm = control->get_resource_limits_manager();
    auto before_link = rlm.get_account_ram_usage("alice"_n);

    link_authority("alice"_n, "sysio"_n, "spending"_n, "reqauth"_n);
    produce_block();

    auto after_link = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_link - before_link,
                        (int64_t)config::billable_size_v<permission_link_object>);

    unlink_authority("alice"_n, "sysio"_n, "reqauth"_n);
    produce_block();

    auto after_unlink = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_unlink - after_link,
                        -(int64_t)config::billable_size_v<permission_link_object>);
    BOOST_REQUIRE_EQUAL(after_unlink, before_link); // full roundtrip
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(createauth_deleteauth_ram_billing, validating_tester) { try {
    create_account("alice"_n);
    produce_block();

    const auto& rlm = control->get_resource_limits_manager();
    auto before_create = rlm.get_account_ram_usage("alice"_n);

    const auto spending_pub_key = get_public_key("alice"_n, "spending");
    set_authority("alice"_n, "spending"_n, authority(spending_pub_key), "active"_n);
    produce_block();

    // Fetch the freshly created permission to compute its exact billable size.
    const auto* perm = control->db().find<permission_object, by_owner>(
        boost::make_tuple("alice"_n, "spending"_n));
    BOOST_REQUIRE(perm != nullptr);

    const int64_t expected_create_bill =
        (int64_t)(config::billable_size_v<permission_object> + perm->auth.get_billable_size());

    auto after_create = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_create - before_create, expected_create_bill);

    delete_authority("alice"_n, "spending"_n);
    produce_block();

    auto after_delete = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_delete - after_create, -expected_create_bill);
    BOOST_REQUIRE_EQUAL(after_delete, before_create); // full roundtrip
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(updateauth_ram_billing, validating_tester) { try {
    create_account("alice"_n);
    produce_block();

    // Start with a single-key spending permission.
    const auto key1 = get_public_key("alice"_n, "spending1");
    set_authority("alice"_n, "spending"_n, authority(key1), "active"_n);
    produce_block();

    const auto* perm_before = control->db().find<permission_object, by_owner>(
        boost::make_tuple("alice"_n, "spending"_n));
    BOOST_REQUIRE(perm_before != nullptr);
    const int64_t old_auth_bill = perm_before->auth.get_billable_size();

    const auto& rlm = control->get_resource_limits_manager();
    auto before_update = rlm.get_account_ram_usage("alice"_n);

    // Grow the authority to two keys. permission_object fixed size is
    // unchanged; only the auth bill should grow.
    const auto key2 = get_public_key("alice"_n, "spending2");
    authority two_key_auth(1,
                           { key_weight{key1, 1}, key_weight{key2, 1} },
                           {});
    two_key_auth.sort_fields(); // validate() requires keys in strict order
    set_authority("alice"_n, "spending"_n, two_key_auth, "active"_n);
    produce_block();

    const auto* perm_after = control->db().find<permission_object, by_owner>(
        boost::make_tuple("alice"_n, "spending"_n));
    BOOST_REQUIRE(perm_after != nullptr);
    const int64_t new_auth_bill = perm_after->auth.get_billable_size();

    auto after_update = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_update - before_update, new_auth_bill - old_auth_bill);
    BOOST_REQUIRE_GT(new_auth_bill, old_auth_bill); // sanity: growing added bytes
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
