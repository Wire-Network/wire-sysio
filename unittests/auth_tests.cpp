#include <contracts.hpp>
#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/authorization_manager.hpp>

#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/resource_limits_private.hpp>

#include <sysio/testing/tester_network.hpp>

#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(auth_tests)

BOOST_FIXTURE_TEST_CASE( missing_sigs, validating_tester ) { try {
   create_accounts( {"alice"_n} );
   produce_block();

   BOOST_REQUIRE_THROW( push_reqauth( "alice"_n, {permission_level{"alice"_n, config::active_name}}, {} ), unsatisfied_authorization );
   auto trace = push_reqauth("alice"_n, "owner");

   produce_block();
   BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trace->id));

} FC_LOG_AND_RETHROW() } /// missing_sigs

BOOST_FIXTURE_TEST_CASE( missing_multi_sigs, validating_tester ) { try {
    produce_block();
    create_account("alice"_n, config::system_account_name, true);
    produce_block();

    BOOST_REQUIRE_THROW(push_reqauth("alice"_n, "owner"), unsatisfied_authorization); // without multisig
    auto trace = push_reqauth("alice"_n, "owner", true); // with multisig

    produce_block();
    BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trace->id));

 } FC_LOG_AND_RETHROW() } /// missing_multi_sigs

BOOST_FIXTURE_TEST_CASE( missing_auths, validating_tester ) { try {
   create_accounts( {"alice"_n, "bob"_n} );
   produce_block();

   /// action not provided from authority
   BOOST_REQUIRE_THROW( push_reqauth( "alice"_n, {permission_level{"bob"_n, config::active_name}}, { get_private_key("bob"_n, "active") } ), missing_auth_exception);

} FC_LOG_AND_RETHROW() } /// transfer_test

/**
 *  This test case will attempt to allow one account to transfer on behalf
 *  of another account by updating the active authority.
 */
BOOST_FIXTURE_TEST_CASE( delegate_auth, validating_tester ) { try {
   // TODO: bug in the current create account implementation undeflows ram usage refunding permission delta to account
   create_accounts( {"alice"_n,"bob"_n});
   produce_block();

   auto delegated_auth = authority( 1, {},
                          {
                            { .permission = {"bob"_n,config::active_name}, .weight = 1}
                          });

   auto original_auth = control->get_authorization_manager().get_permission({"alice"_n, config::active_name}).auth.to_authority();
   wdump((original_auth));

   // TODO: need to fix this!
   elog("BUG HERE: Ram usage delta would underflow UINT64_MAX (crediting Alice for a negative delta)");
   set_authority( "alice"_n, config::active_name,  delegated_auth );

   auto new_auth = control->get_authorization_manager().get_permission({"alice"_n, config::active_name}).auth.to_authority();
   wdump((new_auth));
   BOOST_CHECK_EQUAL((new_auth == delegated_auth), true);

   produce_block();
   produce_block();

   auto auth = control->get_authorization_manager().get_permission({"alice"_n, config::active_name}).auth.to_authority();
   wdump((auth));
   BOOST_CHECK_EQUAL((new_auth == auth), true);

   /// execute nonce from alice signed by bob
   auto trace = push_reqauth("alice"_n, {permission_level{"alice"_n, config::active_name}}, { get_private_key("bob"_n, "active") } );

   produce_block();
   //todoBOOST_REQUIRE_EQUAL(true, chain_has_transaction(trace->id));

} FC_LOG_AND_RETHROW() }


BOOST_AUTO_TEST_CASE(update_auths) {
try {
   // TODO: bug in the current create account implementation undeflows ram usage refunding permission delta to account
   validating_tester chain;
   chain.create_account(name("alice"));
   chain.create_account(name("bob"));

   // Deleting active or owner should fail
   BOOST_CHECK_THROW(chain.delete_authority(name("alice"), name("active")), action_validate_exception);
   BOOST_CHECK_THROW(chain.delete_authority(name("alice"), name("owner")), action_validate_exception);

   // Change owner permission
   const auto new_owner_priv_key = chain.get_private_key(name("alice"), "new_owner");
   const auto new_owner_pub_key = new_owner_priv_key.get_public_key();
   chain.set_authority(name("alice"), name("owner"), authority(new_owner_pub_key), {});
   chain.produce_blocks();

   // Ensure the permission is updated
   permission_object::id_type owner_id;
   {
      auto obj = chain.find<permission_object, by_owner>(boost::make_tuple(name("alice"), name("owner")));
      BOOST_TEST(obj != nullptr);
      BOOST_TEST(obj->owner == name("alice"));
      BOOST_TEST(obj->name == name("owner"));
      BOOST_TEST(obj->parent == 0);
      owner_id = obj->id;
      auto auth = obj->auth.to_authority();
      BOOST_TEST(auth.threshold == 1u);
      BOOST_TEST(auth.keys.size() == 1u);
      BOOST_TEST(auth.accounts.size() == 0u);
      BOOST_TEST(auth.keys[0].key.to_string({}) == new_owner_pub_key.to_string({}));
      BOOST_TEST(auth.keys[0].key == new_owner_pub_key);
      BOOST_TEST(auth.keys[0].weight == 1);
   }

   // Change active permission, remember that the owner key has been changed
   const auto new_active_priv_key = chain.get_private_key(name("alice"), "new_active");
   const auto new_active_pub_key = new_active_priv_key.get_public_key();
   chain.set_authority(name("alice"), name("active"), authority(new_active_pub_key), name("owner"),
                       { permission_level{name("alice"), name("active")} }, { chain.get_private_key(name("alice"), "active") });
   chain.produce_blocks();

   {
      auto obj = chain.find<permission_object, by_owner>(boost::make_tuple(name("alice"), name("active")));
      BOOST_TEST(obj != nullptr);
      BOOST_TEST(obj->owner == name("alice"));
      BOOST_TEST(obj->name == name("active"));
      BOOST_TEST(obj->parent == owner_id);
      auto auth = obj->auth.to_authority();
      BOOST_TEST(auth.threshold == 1u);
      BOOST_TEST(auth.keys.size() == 1u);
      BOOST_TEST(auth.accounts.size() == 0u);
      BOOST_TEST(auth.keys[0].key == new_active_pub_key);
      BOOST_TEST(auth.keys[0].weight == 1u);
   }

   auto spending_priv_key = chain.get_private_key(name("alice"), "spending");
   auto spending_pub_key = spending_priv_key.get_public_key();
   auto trading_priv_key = chain.get_private_key(name("alice"), "trading");
   auto trading_pub_key = trading_priv_key.get_public_key();

   // Bob attempts to create new spending auth for Alice
   BOOST_CHECK_THROW( chain.set_authority( name("alice"), name("spending"), authority(spending_pub_key), name("active"),
                                           { permission_level{name("bob"), name("active")} },
                                           { chain.get_private_key(name("bob"), "active") } ),
                      irrelevant_auth_exception );

   // Create new spending auth
   chain.set_authority(name("alice"), name("spending"), authority(spending_pub_key), name("active"),
                       { permission_level{name("alice"), name("active")} }, { new_active_priv_key });
   chain.produce_blocks();
   {
      auto obj = chain.find<permission_object, by_owner>(boost::make_tuple(name("alice"), name("spending")));
      BOOST_TEST(obj != nullptr);
      BOOST_TEST(obj->owner == name("alice"));
      BOOST_TEST(obj->name == name("spending"));
      BOOST_TEST(chain.get<permission_object>(obj->parent).owner == name("alice"));
      BOOST_TEST(chain.get<permission_object>(obj->parent).name == name("active"));
   }

   // Update spending auth parent to be its own, should fail
   BOOST_CHECK_THROW(chain.set_authority(name("alice"), name("spending"), authority{spending_pub_key}, name("spending"),
                                         { permission_level{name("alice"), name("spending")} }, { spending_priv_key }), action_validate_exception);
   // Update spending auth parent to be owner, should fail
   BOOST_CHECK_THROW(chain.set_authority(name("alice"), name("spending"), authority{spending_pub_key}, name("owner"),
                                         { permission_level{name("alice"), name("spending")} }, { spending_priv_key }), action_validate_exception);

   // Remove spending auth
   chain.delete_authority(name("alice"), name("spending"), { permission_level{name("alice"), name("active")} }, { new_active_priv_key });
   {
      auto obj = chain.find<permission_object, by_owner>(boost::make_tuple(name("alice"), name("spending")));
      BOOST_TEST(obj == nullptr);
   }
   chain.produce_blocks();

   // Create new trading auth
   chain.set_authority(name("alice"), name("trading"), authority{trading_pub_key}, name("active"),
                       { permission_level{name("alice"), name("active")} }, { new_active_priv_key });
   // Recreate spending auth again, however this time, it's under trading instead of owner
   chain.set_authority(name("alice"), name("spending"), authority{spending_pub_key}, name("trading"),
                       { permission_level{name("alice"), name("trading")} }, { trading_priv_key });
   chain.produce_blocks();

   // Verify correctness of trading and spending
   {
      const auto* trading = chain.find<permission_object, by_owner>(boost::make_tuple(name("alice"), name("trading")));
      const auto* spending = chain.find<permission_object, by_owner>(boost::make_tuple(name("alice"), name("spending")));
      BOOST_TEST(trading != nullptr);
      BOOST_TEST(spending != nullptr);
      BOOST_TEST(trading->owner == name("alice"));
      BOOST_TEST(spending->owner == name("alice"));
      BOOST_TEST(trading->name == name("trading"));
      BOOST_TEST(spending->name == name("spending"));
      BOOST_TEST(spending->parent == trading->id);
      BOOST_TEST(chain.get(trading->parent).owner == name("alice"));
      BOOST_TEST(chain.get(trading->parent).name == name("active"));

   }

   // Delete trading, should fail since it has children (spending)
   BOOST_CHECK_THROW(chain.delete_authority(name("alice"), name("trading"),
                                            { permission_level{name("alice"), name("active")} }, { new_active_priv_key }), action_validate_exception);
   // Update trading parent to be spending, should fail since changing parent authority is not supported
   BOOST_CHECK_THROW(chain.set_authority(name("alice"), name("trading"), authority{trading_pub_key}, name("spending"),
                                         { permission_level{name("alice"), name("trading")} }, { trading_priv_key }), action_validate_exception);

   // Delete spending auth
   chain.delete_authority(name("alice"), name("spending"), { permission_level{name("alice"), name("active")} }, { new_active_priv_key });
   BOOST_TEST((chain.find<permission_object, by_owner>(boost::make_tuple(name("alice"), name("spending")))) == nullptr);
   // Delete trading auth, now it should succeed since it doesn't have any children anymore
   chain.delete_authority(name("alice"), name("trading"), { permission_level{name("alice"), name("active")} }, { new_active_priv_key });
   BOOST_TEST((chain.find<permission_object, by_owner>(boost::make_tuple(name("alice"), name("trading")))) == nullptr);

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(update_auth_unknown_private_key) {
   try {
      // TODO: bug in the current create account implementation undeflows ram usage refunding permission delta to account
      validating_tester chain;
      chain.create_account(name("alice"));

      // public key with no corresponding private key
      fc::ecc::public_key_data data;
      data.data[0] = 0x80; // not necessary, 0 also works
      fc::sha256 hash = fc::sha256::hash("unknown key");
      std::memcpy(&data.data[1], hash.data(), hash.data_size() );
      fc::ecc::public_key_shim shim(data);
      fc::crypto::public_key new_owner_pub_key(std::move(shim));

      chain.set_authority(name("alice"), name("owner"), authority(new_owner_pub_key), {});
      chain.produce_blocks();

      // Ensure the permission is updated
      permission_object::id_type owner_id;
      {
         auto obj = chain.find<permission_object, by_owner>(boost::make_tuple(name("alice"), name("owner")));
         BOOST_TEST(obj != nullptr);
         BOOST_TEST(obj->owner == name("alice"));
         BOOST_TEST(obj->name == name("owner"));
         BOOST_TEST(obj->parent == 0);
         owner_id = obj->id;
         auto auth = obj->auth.to_authority();
         BOOST_TEST(auth.threshold == 1u);
         BOOST_TEST(auth.keys.size() == 1u);
         BOOST_TEST(auth.accounts.size() == 0u);
         BOOST_TEST(auth.keys[0].key == new_owner_pub_key);
         BOOST_TEST(auth.keys[0].weight == 1);
      }
   } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE(link_auths) { try {
   validating_tester chain;

   chain.create_accounts({name("alice"),name("bob")});

   const auto spending_priv_key = chain.get_private_key(name("alice"), "spending");
   const auto spending_pub_key = spending_priv_key.get_public_key();
   const auto scud_priv_key = chain.get_private_key(name("alice"), "scud");
   const auto scud_pub_key = scud_priv_key.get_public_key();

   chain.set_authority(name("alice"), name("spending"), authority{spending_pub_key}, name("active"));
   chain.set_authority(name("alice"), name("scud"), authority{scud_pub_key}, name("spending"));

   // Send req auth action with alice's spending key, it should fail
   BOOST_CHECK_THROW(chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("spending")} }, { spending_priv_key }), irrelevant_auth_exception);
   // Link authority for sysio reqauth action with alice's spending key
   chain.link_authority(name("alice"), name("sysio"), name("spending"), name("reqauth"));
   // Now, req auth action with alice's spending key should succeed
   chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("spending")} }, { spending_priv_key });

   chain.produce_block();

   // Relink the same auth should fail
   BOOST_CHECK_THROW( chain.link_authority(name("alice"), name("sysio"), name("spending"), name("reqauth")), action_validate_exception);

   // Unlink alice with sysio reqauth
   chain.unlink_authority(name("alice"), name("sysio"), name("reqauth"));
   // Now, req auth action with alice's spending key should fail
   BOOST_CHECK_THROW(chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("spending")} }, { spending_priv_key }), irrelevant_auth_exception);

   chain.produce_block();

   // Send req auth action with scud key, it should fail
   BOOST_CHECK_THROW(chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("scud")} }, { scud_priv_key }), irrelevant_auth_exception);
   // Link authority for any sysio action with alice's scud key
   chain.link_authority(name("alice"), name("sysio"), name("scud"));
   // Now, req auth action with alice's scud key should succeed
   chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("scud")} }, { scud_priv_key });
   // req auth action with alice's spending key should also be fine, since it is the parent of alice's scud key
   chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("spending")} }, { spending_priv_key });

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(link_then_update_auth) { try {
   validating_tester chain;

   chain.create_account(name("alice"));

   const auto first_priv_key = chain.get_private_key(name("alice"), "first");
   const auto first_pub_key = first_priv_key.get_public_key();
   const auto second_priv_key = chain.get_private_key(name("alice"), "second");
   const auto second_pub_key = second_priv_key.get_public_key();

   chain.set_authority(name("alice"), name("first"), authority{first_pub_key}, name("active"));

   chain.link_authority(name("alice"), name("sysio"), name("first"), name("reqauth"));
   chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("first")} }, { first_priv_key });

   chain.produce_blocks(13); // Wait at least 6 seconds for first push_reqauth transaction to expire.

   // Update "first" auth public key
   chain.set_authority(name("alice"), name("first"), authority{second_pub_key}, name("active"));
   // Authority updated, using previous "first" auth should fail on linked auth
   BOOST_CHECK_THROW(chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("first")} }, { first_priv_key }), unsatisfied_authorization);
   // Using updated authority, should succeed
   chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("first")} }, { second_priv_key });

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(create_account) {
try {
   validating_tester chain;
   chain.create_account(name("joe"));
   chain.produce_block();

   // Verify account created properly
   const auto& joe_owner_authority = chain.get<permission_object, by_owner>(boost::make_tuple(name("joe"), name("owner")));
   BOOST_TEST(joe_owner_authority.auth.threshold == 1u);
   BOOST_TEST(joe_owner_authority.auth.accounts.size() == 1u);
   BOOST_TEST(joe_owner_authority.auth.keys.size() == 1u);
   BOOST_TEST(joe_owner_authority.auth.keys[0].key.to_string({}) == chain.get_public_key(name("joe"), "owner").to_string({}));
   BOOST_TEST(joe_owner_authority.auth.keys[0].weight == 1u);

   const auto& joe_active_authority = chain.get<permission_object, by_owner>(boost::make_tuple(name("joe"), name("active")));
   BOOST_TEST(joe_active_authority.auth.threshold == 1u);
   BOOST_TEST(joe_active_authority.auth.accounts.size() == 1u);
   BOOST_TEST(joe_active_authority.auth.keys.size() == 1u);
   BOOST_TEST(joe_active_authority.auth.keys[0].key.to_string({}) == chain.get_public_key(name("joe"), "active").to_string({}));
   BOOST_TEST(joe_active_authority.auth.keys[0].weight == 1u);

   // Create duplicate name
   BOOST_CHECK_EXCEPTION(chain.create_account(name("joe")), action_validate_exception,
                         fc_exception_message_is("Cannot create account named joe, as that name is already taken"));

   // Creating account with name more than 12 chars
   BOOST_CHECK_EXCEPTION(chain.create_account(name("aaaaaaaaaaaaa")), action_validate_exception,
                         fc_exception_message_is("account names can only be 12 chars long"));

   // Creating a new account with non-privileged account, should fail
   BOOST_CHECK_EXCEPTION(chain.create_account(name("dandy.joe"), name("joe")), action_validate_exception,
                         fc_exception_message_is("Only privileged accounts can create new accounts"));

   // Creating the same new account, this time with privileged account
   auto r = chain.push_action(config::system_account_name, "setpriv"_n, config::system_account_name,
     mutable_variant_object()
     ("account", name("joe"))
     ("is_priv", 1));
   chain.produce_block();

   // Creating account with sysio. prefix with privileged account
   chain.create_account(name("sysio.test1"), name("joe"));

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( any_auth ) { try {
   validating_tester chain;
   chain.create_accounts( {name("alice"), name("bob")} );
   chain.produce_block();

   const auto spending_priv_key = chain.get_private_key(name("alice"), "spending");
   const auto spending_pub_key = spending_priv_key.get_public_key();
   const auto bob_spending_pub_key = spending_priv_key.get_public_key();

   chain.set_authority(name("alice"), name("spending"), authority{spending_pub_key}, name("active"));
   chain.set_authority(name("bob"), name("spending"), authority{bob_spending_pub_key}, name("active"));

   /// this should fail because spending is not active which is default for reqauth
   BOOST_REQUIRE_THROW( chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("spending")} }, { spending_priv_key }),
                        irrelevant_auth_exception );

   chain.produce_block();

   //test.push_reqauth( "alice"_n, { permission_level{"alice"_n,"spending"} }, { spending_priv_key });

   chain.link_authority( name("alice"), name("sysio"), name("sysio.any"), name("reqauth") );
   chain.link_authority( name("bob"), name("sysio"), name("sysio.any"), name("reqauth") );

   /// this should succeed because sysio::reqauth is linked to any permission
   chain.push_reqauth(name("alice"), { permission_level{"alice"_n, name("spending")} }, { spending_priv_key });

   /// this should fail because bob cannot authorize for alice, the permission given must be one-of alices
   BOOST_REQUIRE_THROW( chain.push_reqauth(name("alice"), { permission_level{"bob"_n, name("spending")} }, { spending_priv_key }),
                        missing_auth_exception );


   chain.produce_block();

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(no_double_billing) {
try {
   SKIP_TEST
   fc::temp_directory tempdir;
   validating_tester chain( tempdir, true );
   chain.execute_setup_policy( setup_policy::preactivate_feature_and_new_bios );

   chain.produce_block();

   account_name acc1 = "bill1"_n;
   account_name acc2 = "bill2"_n;
   account_name acc1a = "bill1a"_n;

   chain.create_account(acc1);
   chain.create_account(acc1a);

   //  TODO: acc1 needs to be privileged to create accounts, but then it doesn't use CPU or Net....  need to do something else...
   auto r = chain.push_action(config::system_account_name, "setpriv"_n, config::system_account_name,
     mutable_variant_object()
     ("account", acc1)
     ("is_priv", 1));
   chain.produce_block();

   chain.produce_block();

   const chainbase::database &db = chain.control->db();

   using resource_usage_object = sysio::chain::resource_limits::resource_usage_object;
   using by_owner = sysio::chain::resource_limits::by_owner;

   auto create_acc = [&](account_name a) {

      signed_transaction trx;
      chain.set_transaction_headers(trx);

      authority owner_auth =  authority( chain.get_public_key( a, "owner" ) );

      vector<permission_level> pls = {{acc1, name("active")}};
      pls.push_back({acc1, name("owner")}); // same account but different permission names
      pls.push_back({acc1a, name("owner")});
      trx.actions.emplace_back( pls,
                                newaccount{
                                   .creator  = acc1,
                                   .name     = a,
                                   .owner    = owner_auth,
                                   .active   = authority( chain.get_public_key( a, "active" ) )
                                });

      chain.set_transaction_headers(trx);
      trx.sign( chain.get_private_key( acc1, "active" ), chain.control->get_chain_id()  );
      trx.sign( chain.get_private_key( acc1, "owner" ), chain.control->get_chain_id()  );
      trx.sign( chain.get_private_key( acc1a, "owner" ), chain.control->get_chain_id()  );
      return chain.push_transaction( trx );
   };

   create_acc(acc2);

   const auto &usage = db.get<resource_usage_object,by_owner>(acc1);

   const auto &usage2 = db.get<resource_usage_object,by_owner>(acc1a);

   BOOST_TEST(usage.cpu_usage.average() > 0U);
   BOOST_TEST(usage.net_usage.average() > 0U);
   BOOST_REQUIRE_EQUAL(usage.cpu_usage.average(), usage2.cpu_usage.average());
   BOOST_REQUIRE_EQUAL(usage.net_usage.average(), usage2.net_usage.average());
   chain.produce_block();

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(stricter_auth) {
try {
   SKIP_TEST
   validating_tester chain;

   chain.produce_block();

   account_name acc1 = "acc1"_n;
   account_name acc2 = "acc2"_n;
   account_name acc3 = "acc3"_n;
   account_name acc4 = "acc4"_n;



   chain.create_account(acc1);
   chain.create_accounts({acc2, acc3, acc4});
   chain.produce_block();

   auto create_acc = [&](account_name a, account_name creator, int threshold) {

      signed_transaction trx;
      chain.set_transaction_headers(trx);

      authority invalid_auth = authority(threshold, {key_weight{chain.get_public_key( a, "owner" ), 1}}, {permission_level_weight{{creator, config::active_name}, 1}});

      vector<permission_level> pls;
      pls.push_back({creator, name("active")});
      // force contracts::dancer_wasm() to be of type bytes

      auto wasm = test_contracts::noop_wasm();
      trx.actions.emplace_back( pls,
         setcode{
           .account = a,
           .code = bytes(wasm.begin(), wasm.end()),
         });
      // trx.actions.emplace_back( pls, setabi{ .account = a, .abi =  bytes(contracts::noop_abi().begin(), contracts::noop_abi().end()) });

                                // newaccount{
                                //    .creator  = creator,
                                //    .name     = a,
                                //    .owner    = authority( chain.get_public_key( a, "owner" ) ),
                                //    .active   = invalid_auth//authority( chain.get_public_key( a, "active" ) ),
                                // });

      chain.set_transaction_headers(trx);
      trx.sign( chain.get_private_key( creator, "active" ), chain.control->get_chain_id()  );
      return chain.push_transaction( trx );
   };

   try {
     create_acc(acc2, acc1, 0);
     BOOST_FAIL("threshold can't be zero");
   } catch (...) { }

   try {
     create_acc(acc4, acc1, 3);
     BOOST_FAIL("threshold can't be more than total weight");
   } catch (...) { }

   create_acc(acc3, acc1, 1);

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( linkauth_special ) { try {
   validating_tester chain;

   const auto& tester_account = "tester"_n;
   std::vector<transaction_id_type> ids;

   chain.produce_blocks();
   chain.create_account("currency"_n);

   chain.produce_blocks();
   chain.create_account("tester"_n);
   chain.create_account("tester2"_n);
   chain.produce_blocks();

   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")
           ("parent", "active")
           ("auth",  authority(chain.get_public_key(tester_account, "first"), 5))
   );

   auto validate_disallow = [&] (const char *type) {
      BOOST_REQUIRE_EXCEPTION(
         chain.push_action(config::system_account_name, linkauth::get_name(), tester_account, fc::mutable_variant_object()
               ("account", "tester")
               ("code", "sysio")
               ("type", type)
               ("requirement", "first")),
         action_validate_exception,
         fc_exception_message_is(std::string("Cannot link sysio::") + std::string(type) + std::string(" to a minimum permission"))
      );
   };

   validate_disallow("linkauth");
   validate_disallow("unlinkauth");
   validate_disallow("deleteauth");
   validate_disallow("updateauth");
   validate_disallow("canceldelay");

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(delete_auth) { try {
   validating_tester chain;

   const auto& tester_account = "tester"_n;

   chain.produce_blocks();
   chain.create_account("sysio.token"_n);
   chain.produce_blocks(10);

   chain.set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   chain.set_abi("sysio.token"_n, test_contracts::sysio_token_abi());

   chain.produce_blocks();
   chain.create_account("tester"_n);
   chain.create_account("tester2"_n);
   chain.produce_blocks(10);

   transaction_trace_ptr trace;

   // can't delete auth because it doesn't exist
   BOOST_REQUIRE_EXCEPTION(
   trace = chain.push_action(config::system_account_name, deleteauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")),
   permission_query_exception,
   [] (const permission_query_exception &e)->bool {
      expect_assert_message(e, "permission_query_exception: Permission Query Exception\nFailed to retrieve permission");
      return true;
   });

   // update auth
   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")
           ("parent", "active")
           ("auth",  authority(chain.get_public_key(tester_account, "first")))
   );

   // link auth
   chain.push_action(config::system_account_name, linkauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("code", "sysio.token")
           ("type", "transfer")
           ("requirement", "first"));

   // create CUR token
   chain.produce_blocks();
   chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
           ("issuer", "sysio.token" )
           ("maximum_supply", "9000000.0000 CUR" )
   );

   // issue to account "sysio.token"
   chain.push_action("sysio.token"_n, name("issue"), "sysio.token"_n, fc::mutable_variant_object()
           ("to",       "sysio.token")
           ("quantity", "1000000.0000 CUR")
           ("memo", "for stuff")
   );

   // transfer from sysio.token to tester
   trace = chain.push_action("sysio.token"_n, name("transfer"), "sysio.token"_n, fc::mutable_variant_object()
       ("from", "sysio.token")
       ("to", "tester")
       ("quantity", "100.0000 CUR")
       ("memo", "hi" )
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   auto liquid_balance = chain.get_currency_balance("sysio.token"_n, symbol(SY(4,CUR)), "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = chain.get_currency_balance("sysio.token"_n, symbol(SY(4,CUR)), "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);

   trace = chain.push_action("sysio.token"_n, name("transfer"),
      vector<permission_level>{{"tester"_n, config::active_name},{"tester"_n, config::sysio_payer_name}},
      fc::mutable_variant_object()
       ("from", "tester")
       ("to", "tester2")
       ("quantity", "1.0000 CUR")
       ("memo", "hi" )
   );

   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   liquid_balance = chain.get_currency_balance("sysio.token"_n, symbol(SY(4,CUR)), "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = chain.get_currency_balance("sysio.token"_n, symbol(SY(4,CUR)), "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = chain.get_currency_balance("sysio.token"_n, symbol(SY(4,CUR)), "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   // can't delete auth because it's linked
   BOOST_REQUIRE_EXCEPTION(
   trace = chain.push_action(config::system_account_name, deleteauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")),
   action_validate_exception,
   [] (const action_validate_exception &e)->bool {
      expect_assert_message(e, "action_validate_exception: message validation exception\nCannot delete a linked authority");
      return true;
   });

   // unlink auth
   trace = chain.push_action(config::system_account_name, unlinkauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("code", "sysio.token")
           ("type", "transfer"));
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   // delete auth
   trace = chain.push_action(config::system_account_name, deleteauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first"));

   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks(1);;

   trace = chain.push_action("sysio.token"_n, name("transfer"), "tester"_n, fc::mutable_variant_object()
       ("from", "tester")
       ("to", "tester2")
       ("quantity", "3.0000 CUR")
       ("memo", "hi" )
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   liquid_balance = chain.get_currency_balance("sysio.token"_n, symbol(SY(4,CUR)), "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("96.0000 CUR"), liquid_balance);
   liquid_balance = chain.get_currency_balance("sysio.token"_n, symbol(SY(4,CUR)), "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("4.0000 CUR"), liquid_balance);

} FC_LOG_AND_RETHROW() }/// delete_auth

// at the bottom of auth_tests.cpp, inside BOOST_AUTO_TEST_SUITE(auth_tests)
BOOST_FIXTURE_TEST_CASE(ext_permission_protection, validating_tester) {
   try {
      // 1) setup accounts and keys
      create_account(name("alice"));
      // 'sysio' already exists in genesis
      auto alice_ext_priv = get_private_key(name("alice"), "ext");
      auto alice_ext_pub  = alice_ext_priv.get_public_key();
      auto sysio_priv     = get_private_key(name("sysio"), "active");

      // build signing‐key vectors
      std::vector<private_key_type> alice_keys;
      alice_keys.push_back(alice_ext_priv);

      std::vector<private_key_type> sysio_keys;
      sysio_keys.push_back(sysio_priv);

      // 2) non-sysio may NOT CREATE foo.ext
      BOOST_CHECK_THROW(
         set_authority(
            name("alice"),                // account
            name("foo.ext"),              // new .ext permission
            authority(alice_ext_pub),     // authority ctor from pub_key
            name("active"),               // parent
            std::vector<permission_level>{{name("alice"), name("active")}},
            alice_keys
         ),
         invalid_permission
      );

      // 3) sysio CAN CREATE foo.ext
      set_authority(
         name("alice"),
         name("foo.ext"),
         authority(alice_ext_pub),
         name("active"),
         std::vector<permission_level>{{name("sysio"), name("active")}},
         sysio_keys
      );
      produce_blocks();

      // verify creation succeeded
      {
         auto obj = find<permission_object, by_owner>(
            boost::make_tuple(name("alice"), name("foo.ext"))
         );
         BOOST_TEST(obj != nullptr);
         BOOST_TEST(obj->name == name("foo.ext"));
      }

      // 4) non-sysio may NOT MODIFY foo.ext
      BOOST_CHECK_THROW(
         set_authority(
            name("alice"),
            name("foo.ext"),
            authority(alice_ext_pub),     // same pubkey, wrong actor
            name("active"),
            std::vector<permission_level>{{name("alice"), name("active")}},
            alice_keys
         ),
         invalid_permission
      );

      // 5) sysio CAN MODIFY foo.ext
      authority two_of_two;
      two_of_two.threshold = 2;
      two_of_two.keys.emplace_back(key_weight{alice_ext_pub, 1});
      two_of_two.accounts.emplace_back(permission_level_weight{{name("alice"), name("active")}, 1});

      set_authority(
         name("alice"),
         name("foo.ext"),
         two_of_two,
         name("active"),
         std::vector<permission_level>{{name("sysio"), name("active")}},
         sysio_keys
      );
      produce_blocks();

      // verify the threshold bump
      {
         auto obj  = get<permission_object, by_owner>(
            boost::make_tuple(name("alice"), name("foo.ext"))
         );
         auto auth = obj.auth.to_authority();
         BOOST_TEST(auth.threshold == 2u);
      }

} FC_LOG_AND_RETHROW() } // ext_permission_protection

BOOST_AUTO_TEST_SUITE_END()
