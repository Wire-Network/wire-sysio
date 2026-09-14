#include <boost/test/unit_test.hpp>

#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/wast_to_wasm.hpp>

#include <sstream>
#include <string>

#include "sysio.system_tester.hpp"

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace sysio_system;

/** Wire representation of auth.msg::onlinkauth's notification payload. */
struct authmsg_onlinkauth {
   name            user;
   name            permission;
   public_key_type pub_key;
};

FC_REFLECT( authmsg_onlinkauth, (user)(permission)(pub_key) )

namespace {

constexpr auto auth_msg_account    = "auth.msg"_n;
constexpr auto auth_ext_permission = "auth.ext"_n;
constexpr auto onlinkauth_action   = "onlinkauth"_n;

/** Build a minimal auth.msg stand-in that notifies sysio for every action. */
std::vector<uint8_t> authmsg_notification_wasm() {
   std::ostringstream wast;
   wast << R"((module
      (import "env" "require_recipient" (func $require_recipient (param i64)))
      (func (export "apply") (param i64 i64 i64)
         (call $require_recipient (i64.const 0x)"
        << std::hex << config::system_account_name.value << R"())
      )
   ))";
   return wast_to_wasm( wast.str() );
}

/** System-contract fixture with a notifying contract installed on auth.msg. */
struct authmsg_notification_tester : sysio_system_tester {
   authmsg_notification_tester() {
      create_account( auth_msg_account );
      set_code( auth_msg_account, authmsg_notification_wasm() );
      produce_block();
   }

   /** Send auth.msg::onlinkauth and return the transaction result. */
   action_result notify_link( name permission, const public_key_type& pub_key ) {
      action notification;
      notification.account = auth_msg_account;
      notification.name    = onlinkauth_action;
      notification.data = fc::raw::pack(
         authmsg_onlinkauth{ config::system_account_name, permission, pub_key } );
      return push_contract_paid_action( std::move( notification ), auth_msg_account.value );
   }

   /** Find one permission on the system account affected by the privileged inline action. */
   const permission_object* find_system_permission( name permission ) const {
      return control->get_authorization_manager().find_permission( { config::system_account_name, permission } );
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE( sysio_system_authmsg_tests )

/** The reserved auth.ext permission is created below active and can be rotated. */
BOOST_FIXTURE_TEST_CASE( installs_and_rotates_reserved_auth_ext_permission, authmsg_notification_tester ) try {
   const auto first_key  = get_public_key( config::system_account_name, "auth-ext-first" );
   const auto second_key = get_public_key( config::system_account_name, "auth-ext-second" );

   BOOST_REQUIRE_EQUAL( success(), notify_link( auth_ext_permission, first_key ) );

   const auto* active   = find_system_permission( config::active_name );
   const auto* auth_ext = find_system_permission( auth_ext_permission );
   BOOST_REQUIRE( active != nullptr );
   BOOST_REQUIRE( auth_ext != nullptr );
   BOOST_CHECK( auth_ext->parent == active->id );
   const auto first_authority = auth_ext->auth.to_authority();
   BOOST_CHECK_EQUAL( first_authority.threshold, 1u );
   BOOST_REQUIRE_EQUAL( first_authority.keys.size(), 1u );
   BOOST_CHECK( first_authority.keys.front().key == first_key );
   BOOST_CHECK( first_authority.accounts.empty() );

   BOOST_REQUIRE_EQUAL( success(), notify_link( auth_ext_permission, second_key ) );
   auth_ext = find_system_permission( auth_ext_permission );
   BOOST_REQUIRE( auth_ext != nullptr );
   const auto second_authority = auth_ext->auth.to_authority();
   BOOST_REQUIRE_EQUAL( second_authority.keys.size(), 1u );
   BOOST_CHECK( second_authority.keys.front().key == second_key );
} FC_LOG_AND_RETHROW()

/** Privileged owner and active authorities cannot be targeted through the notification payload. */
BOOST_FIXTURE_TEST_CASE( rejects_owner_and_active_permissions, authmsg_notification_tester ) try {
   const auto attacker_key = get_public_key( config::system_account_name, "attacker" );
   const auto* owner        = find_system_permission( config::owner_name );
   const auto* active       = find_system_permission( config::active_name );
   BOOST_REQUIRE( owner != nullptr );
   BOOST_REQUIRE( active != nullptr );
   const auto owner_before  = owner->auth.to_authority();
   const auto active_before = active->auth.to_authority();

   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "onlinkauth may only update auth.ext" ),
                        notify_link( config::owner_name, attacker_key ) );
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "onlinkauth may only update auth.ext" ),
                        notify_link( config::active_name, attacker_key ) );

   owner  = find_system_permission( config::owner_name );
   active = find_system_permission( config::active_name );
   BOOST_REQUIRE( owner != nullptr );
   BOOST_REQUIRE( active != nullptr );
   const auto owner_authority  = owner->auth.to_authority();
   const auto active_authority = active->auth.to_authority();
   BOOST_CHECK( owner_authority == owner_before );
   BOOST_CHECK( active_authority == active_before );
} FC_LOG_AND_RETHROW()

/** The whitelist also rejects non-critical but non-reserved child permission names. */
BOOST_FIXTURE_TEST_CASE( rejects_arbitrary_child_permission, authmsg_notification_tester ) try {
   constexpr auto arbitrary_permission = "session"_n;

   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "onlinkauth may only update auth.ext" ),
                        notify_link( arbitrary_permission,
                                     get_public_key( config::system_account_name, "session" ) ) );
   BOOST_CHECK( find_system_permission( arbitrary_permission ) == nullptr );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
