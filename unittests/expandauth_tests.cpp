#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/testing/tester.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <boost/test/unit_test.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>
#include <sysio_system_tester.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

using mvo = fc::mutable_variant_object;

BOOST_AUTO_TEST_SUITE(expandauth_tests)

// Helper: query a permission_object from the database
static const permission_object*
find_permission(base_tester& chain, name account, name perm) {
   return chain.find<permission_object, by_owner>(boost::make_tuple(account, perm));
}

// ---- get_permission_lower_bound intrinsic + expandauth basic ----

BOOST_FIXTURE_TEST_CASE( expandauth_add_key, sysio_system::sysio_system_tester ) { try {
   // alice1111111 already created by sysio_system_tester
   const auto alice = "alice1111111"_n;

   // Verify alice has an active permission with one key
   {
      auto obj = find_permission(*this, alice, config::active_name);
      BOOST_REQUIRE( obj != nullptr );
      auto auth = obj->auth.to_authority();
      BOOST_TEST( auth.threshold == 1u );
      BOOST_TEST( auth.keys.size() == 1u );
   }

   // Generate a new key to add
   auto new_priv_key = get_private_key(alice, "extra");
   auto new_pub_key  = new_priv_key.get_public_key();

   // Push expandauth (system contract calls itself, so signer is sysio)
   BOOST_REQUIRE_EQUAL( success(), push_action(config::system_account_name, "expandauth"_n, mvo()
      ("account",    alice)
      ("permission", "active")
      ("keys",       fc::variants{ mvo()("key", new_pub_key)("weight", 1) })
      ("accounts",   fc::variants{})
   ));
   produce_block();

   // Verify the permission now has two keys
   {
      auto obj = find_permission(*this, alice, config::active_name);
      BOOST_REQUIRE( obj != nullptr );
      auto auth = obj->auth.to_authority();
      BOOST_TEST( auth.threshold == 1u );
      BOOST_TEST( auth.keys.size() == 2u );
      BOOST_TEST( auth.accounts.size() == 0u );

      // Both original and new key should be present
      bool found_new = false;
      for( const auto& kw : auth.keys ) {
         if( kw.key == new_pub_key ) {
            BOOST_TEST( kw.weight == 1u );
            found_new = true;
         }
      }
      BOOST_TEST( found_new );
   }
} FC_LOG_AND_RETHROW() }

// ---- Verify new key is recognized by the authorization manager ----

BOOST_FIXTURE_TEST_CASE( expandauth_new_key_can_sign, sysio_system::sysio_system_tester ) { try {
   const auto alice = "alice1111111"_n;

   auto new_priv_key = get_private_key(alice, "extra");
   auto new_pub_key  = new_priv_key.get_public_key();

   // Add the new key
   BOOST_REQUIRE_EQUAL( success(), push_action(config::system_account_name, "expandauth"_n, mvo()
      ("account",    alice)
      ("permission", "active")
      ("keys",       fc::variants{ mvo()("key", new_pub_key)("weight", 1) })
      ("accounts",   fc::variants{})
   ));
   produce_block();

   // Verify the new key satisfies alice@active authorization check
   const auto& auth_mgr = control->get_authorization_manager();
   BOOST_REQUIRE_NO_THROW(
      auth_mgr.check_authorization( alice, config::active_name,
                                    flat_set<public_key_type>{ new_pub_key },
                                    flat_set<permission_level>{},
                                    std::function<void()>{},
                                    true )
   );

   // Original key should also still work
   auto orig_pub_key = get_public_key(alice, "active");
   BOOST_REQUIRE_NO_THROW(
      auth_mgr.check_authorization( alice, config::active_name,
                                    flat_set<public_key_type>{ orig_pub_key },
                                    flat_set<permission_level>{},
                                    std::function<void()>{},
                                    true )
   );

} FC_LOG_AND_RETHROW() }

// ---- Duplicate keys are skipped ----

BOOST_FIXTURE_TEST_CASE( expandauth_skip_duplicates, sysio_system::sysio_system_tester ) { try {
   const auto alice = "alice1111111"_n;

   auto new_priv_key = get_private_key(alice, "extra");
   auto new_pub_key  = new_priv_key.get_public_key();

   // Add the key once
   BOOST_REQUIRE_EQUAL( success(), push_action(config::system_account_name, "expandauth"_n, mvo()
      ("account",    alice)
      ("permission", "active")
      ("keys",       fc::variants{ mvo()("key", new_pub_key)("weight", 1) })
      ("accounts",   fc::variants{})
   ));
   produce_block();

   // Add the same key again -- should succeed but not add a duplicate
   BOOST_REQUIRE_EQUAL( success(), push_action(config::system_account_name, "expandauth"_n, mvo()
      ("account",    alice)
      ("permission", "active")
      ("keys",       fc::variants{ mvo()("key", new_pub_key)("weight", 1) })
      ("accounts",   fc::variants{})
   ));
   produce_block();

   // Should still have only 2 keys (original + new), not 3
   {
      auto obj = find_permission(*this, alice, config::active_name);
      BOOST_REQUIRE( obj != nullptr );
      auto auth = obj->auth.to_authority();
      BOOST_TEST( auth.keys.size() == 2u );
   }
} FC_LOG_AND_RETHROW() }

// ---- Add account permission ----

BOOST_FIXTURE_TEST_CASE( expandauth_add_account, sysio_system::sysio_system_tester ) { try {
   const auto alice = "alice1111111"_n;
   const auto bob   = "bob111111111"_n;

   // Add bob@active as an account-based authority on alice's active permission
   BOOST_REQUIRE_EQUAL( success(), push_action(config::system_account_name, "expandauth"_n, mvo()
      ("account",    alice)
      ("permission", "active")
      ("keys",       fc::variants{})
      ("accounts",   fc::variants{
         mvo()("permission", mvo()("actor", bob)("permission", "active"))("weight", 1)
      })
   ));
   produce_block();

   // Verify the account permission was added
   {
      auto obj = find_permission(*this, alice, config::active_name);
      BOOST_REQUIRE( obj != nullptr );
      auto auth = obj->auth.to_authority();
      BOOST_TEST( auth.keys.size() == 1u );     // original key still there
      BOOST_TEST( auth.accounts.size() == 1u );  // bob@active added
      BOOST_TEST( auth.accounts[0].permission.actor == bob );
      BOOST_TEST( auth.accounts[0].permission.permission == config::active_name );
      BOOST_TEST( auth.accounts[0].weight == 1u );
   }
} FC_LOG_AND_RETHROW() }

// ---- Only system contract can call expandauth ----

BOOST_FIXTURE_TEST_CASE( expandauth_requires_system_auth, sysio_system::sysio_system_tester ) { try {
   const auto alice = "alice1111111"_n;
   auto new_pub_key = get_private_key(alice, "extra").get_public_key();

   // alice tries to call expandauth -- should fail (require_auth(get_self()) fails)
   BOOST_REQUIRE_EQUAL( string("missing authority of sysio"),
      push_action(alice, "expandauth"_n, mvo()
         ("account",    alice)
         ("permission", "active")
         ("keys",       fc::variants{ mvo()("key", new_pub_key)("weight", 1) })
         ("accounts",   fc::variants{})
   ));
} FC_LOG_AND_RETHROW() }

// ---- Nonexistent permission fails ----

BOOST_FIXTURE_TEST_CASE( expandauth_nonexistent_permission, sysio_system::sysio_system_tester ) { try {
   const auto alice = "alice1111111"_n;
   auto new_pub_key = get_private_key(alice, "extra").get_public_key();

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("permission does not exist"),
      push_action(config::system_account_name, "expandauth"_n, mvo()
         ("account",    alice)
         ("permission", "nonexistent")
         ("keys",       fc::variants{ mvo()("key", new_pub_key)("weight", 1) })
         ("accounts",   fc::variants{})
   ));
} FC_LOG_AND_RETHROW() }

// ---- Must provide at least one key or account ----

BOOST_FIXTURE_TEST_CASE( expandauth_empty_params, sysio_system::sysio_system_tester ) { try {
   const auto alice = "alice1111111"_n;

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must provide at least one key or account to add"),
      push_action(config::system_account_name, "expandauth"_n, mvo()
         ("account",    alice)
         ("permission", "active")
         ("keys",       fc::variants{})
         ("accounts",   fc::variants{})
   ));
} FC_LOG_AND_RETHROW() }

// ---- Add an ETH (EM) key via expandauth ----

BOOST_FIXTURE_TEST_CASE( expandauth_add_eth_key, sysio_system::sysio_system_tester ) { try {
   const auto alice = "alice1111111"_n;

   // Generate an EM (ETH / secp256k1-uncompressed) key
   auto em_priv_key = private_key_type::generate(private_key_type::key_type::em);
   auto em_pub_key  = em_priv_key.get_public_key();

   // Add the ETH key to alice's active permission
   BOOST_REQUIRE_EQUAL( success(), push_action(config::system_account_name, "expandauth"_n, mvo()
      ("account",    alice)
      ("permission", "active")
      ("keys",       fc::variants{ mvo()("key", em_pub_key)("weight", 1) })
      ("accounts",   fc::variants{})
   ));
   produce_block();

   // Verify the permission now has two keys (original K1 + new EM)
   {
      auto obj = find_permission(*this, alice, config::active_name);
      BOOST_REQUIRE( obj != nullptr );
      auto auth = obj->auth.to_authority();
      BOOST_TEST( auth.keys.size() == 2u );

      bool found_em = false;
      for( const auto& kw : auth.keys ) {
         if( kw.key == em_pub_key ) {
            BOOST_TEST( kw.weight == 1u );
            found_em = true;
         }
      }
      BOOST_TEST( found_em );
   }

   // Verify the EM key satisfies alice@active authorization
   const auto& auth_mgr = control->get_authorization_manager();
   BOOST_REQUIRE_NO_THROW(
      auth_mgr.check_authorization( alice, config::active_name,
                                    flat_set<public_key_type>{ em_pub_key },
                                    flat_set<permission_level>{},
                                    std::function<void()>{},
                                    true )
   );

} FC_LOG_AND_RETHROW() }

// ---- Sign a transaction with the ETH key added via expandauth ----

BOOST_FIXTURE_TEST_CASE( expandauth_sign_with_eth_key, sysio_system::sysio_system_tester ) { try {
   const auto alice = "alice1111111"_n;

   // Generate an EM (ETH) key and add it to alice's active permission
   auto em_priv_key = private_key_type::generate(private_key_type::key_type::em);
   auto em_pub_key  = em_priv_key.get_public_key();

   BOOST_REQUIRE_EQUAL( success(), push_action(config::system_account_name, "expandauth"_n, mvo()
      ("account",    alice)
      ("permission", "active")
      ("keys",       fc::variants{ mvo()("key", em_pub_key)("weight", 1) })
      ("accounts",   fc::variants{})
   ));
   produce_block();

   // Sign a reqauth action exclusively with the EM key
   {
      signed_transaction trx;
      action reqauth_act;
      reqauth_act.account = config::system_account_name;
      reqauth_act.name = "reqauth"_n;
      reqauth_act.authorization = { permission_level{alice, config::active_name} };
      reqauth_act.data = fc::raw::pack(alice);
      trx.actions.push_back( reqauth_act );
      set_transaction_headers(trx);
      trx.sign( em_priv_key, control->get_chain_id() );
      push_transaction( trx );
   }
   produce_block();

   // Sanity: a random EM key that is NOT in the permission must fail
   auto bad_priv_key = private_key_type::generate(private_key_type::key_type::em);
   {
      signed_transaction trx;
      action reqauth_act;
      reqauth_act.account = config::system_account_name;
      reqauth_act.name = "reqauth"_n;
      reqauth_act.authorization = { permission_level{alice, config::active_name} };
      reqauth_act.data = fc::raw::pack(alice);
      trx.actions.push_back( reqauth_act );
      set_transaction_headers(trx);
      trx.sign( bad_priv_key, control->get_chain_id() );
      BOOST_REQUIRE_THROW( push_transaction( trx ), unsatisfied_authorization );
   }

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
