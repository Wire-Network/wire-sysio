#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
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
    BOOST_TEST( ram_bytes == 2808 ); // RAM usage allowed to account

    int64_t ram_usage = resource_manager.get_account_ram_usage("kevin"_n);
    BOOST_TEST( ram_usage == 2724 ); // RAM used by "kevin" during account creation, provided by sysio

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

BOOST_AUTO_TEST_SUITE_END()
