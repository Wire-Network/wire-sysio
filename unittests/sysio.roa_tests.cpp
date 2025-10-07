#include <sysio/testing/tester.hpp>
#include "sysio_system_tester.hpp"

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/resource_limits.hpp>

#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>
#include <string>
#include <type_traits>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;
using namespace std;

using mvo = fc::mutable_variant_object;

constexpr account_name ROA = "sysio.roa"_n;
constexpr uint64_t NETWORK_GEN = 0;

class sysio_roa_tester : public tester {
public:

   sysio_roa_tester() {
      produce_blocks( 2 );

      create_accounts( { "alice"_n, "bob"_n, "carol"_n, "darcy"_n }, false, false, false, false );
      produce_blocks( 2 );

      // set_code( ROA, contracts::sysio_roa_wasm() );
      // set_abi( ROA, contracts::sysio_roa_abi().data() );

      produce_blocks();

      const auto& accnt = control->db().get<account_object,by_name>( ROA );
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, abi), true);
      abi_ser.set_abi(abi, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_action( const account_name& signer, const action_name &name, const variant_object &data ) {
      string action_type_name = abi_ser.get_action_type(name);

      action act;
      act.account = ROA;
      act.name    = name;
      act.data    = abi_ser.variant_to_binary( action_type_name, data, abi_serializer::create_yield_function(abi_serializer_max_time) );

      return base_tester::push_action( std::move(act), signer.to_uint64_t() );
   }

   transaction_trace_ptr push_paid_action( const account_name& signer, const action_name &name, const variant_object &data ) {
      return base_tester::push_action( ROA, name,
         vector<permission_level>{{signer, "active"_n},{signer, "sysio.payer"_n}},
         data);
   }

   fc::variant get_nodeowner( account_name acc )
   {
      const auto& db = control->db();
      if (const auto* table = db.find<table_id_object, by_code_scope_table>(
             boost::make_tuple(ROA, static_cast<scope_name>(NETWORK_GEN), "nodeowners"_n))) {
         if (auto* obj = db.find<key_value_object, by_scope_primary>(boost::make_tuple(table->id, acc.to_uint64_t()))) {
            const vector<char> data(obj->value.data(), obj->value.data() + obj->value.size());
            if (!data.empty()) {
               return abi_ser.binary_to_variant( "nodeowners", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
            }
         }
      }
      return fc::variant();
   }

   fc::variant get_sponsorship( account_name acc, account_name nonce)
   {
      const auto& db = control->db();
      if (const auto* table = db.find<table_id_object, by_code_scope_table>(
             boost::make_tuple(ROA, acc, "sponsors"_n))) {
         if (auto* obj = db.find<key_value_object, by_scope_primary>(boost::make_tuple(table->id, nonce.to_uint64_t()))) {
            const vector<char> data(obj->value.data(), obj->value.data() + obj->value.size());
            if (!data.empty()) {
               return abi_ser.binary_to_variant( "sponsor", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
            }
         }
      }
      return fc::variant();
   }

   uint64_t get_sponsor_count( account_name acc )
   {
      const auto& db = control->db();
      if (const auto* table = db.find<table_id_object, by_code_scope_table>(
             boost::make_tuple(ROA, static_cast<scope_name>(NETWORK_GEN), "sponsorcount"_n))) {
         if (auto *obj = db.find<key_value_object, by_scope_primary>(boost::make_tuple(table->id, acc.to_uint64_t()))) {
            const vector<char> data(obj->value.data(), obj->value.data() + obj->value.size());
            if (!data.empty()) {
               auto record = abi_ser.binary_to_variant("sponsorcount", data, abi_serializer::create_yield_function(abi_serializer_max_time));
               return record["count"].as<uint64_t>();
            }
         }
      }
      return 0;
   }

   fc::variant get_reslimit( account_name acc )
   {
      const auto& db = control->db();
      if (const auto* table = db.find<table_id_object, by_code_scope_table>(
             boost::make_tuple(ROA, ROA, "reslimit"_n))) {
         if (auto* obj = db.find<key_value_object, by_scope_primary>(boost::make_tuple(table->id, acc.to_uint64_t()))) {
            const vector<char> data(obj->value.data(), obj->value.data() + obj->value.size());
            if (!data.empty()) {
               return abi_ser.binary_to_variant( "reslimit", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
            }
         }
      }
      return fc::variant();
   }

   fc::variant get_policy( account_name acc, account_name owner )
   {
      const auto& db = control->db();
      if (const auto* table = db.find<table_id_object, by_code_scope_table>(
             boost::make_tuple(ROA, owner, "policies"_n))) {
         if (auto* obj = db.find<key_value_object, by_scope_primary>(boost::make_tuple(table->id, acc.to_uint64_t()))) {
            const vector<char> data(obj->value.data(), obj->value.data() + obj->value.size());
            if (!data.empty()) {
               return abi_ser.binary_to_variant( "policies", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
            }
         }
      }
      return fc::variant();
   }

   transaction_trace_ptr newuser( account_name creator, name nonce, fc::crypto::public_key pubkey)
   {
      return push_paid_action( creator, "newuser"_n, mvo()
           ( "creator", creator)
           ( "nonce", nonce )
           ( "pubkey", pubkey )
      );
   }

   account_name create_newuser(account_name creator) {
      static name nonce{1u};
      nonce = name{nonce.to_uint64_t() + 1};

      auto newuser_result = newuser(creator, nonce, get_public_key(creator, "active"));
      BOOST_REQUIRE(newuser_result && newuser_result->action_traces.size() > 0);
      auto newuser_action_trace = newuser_result->action_traces[0];
      auto new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
      return new_name;
   }

   action_result regnodeowner( account_name owner, uint8_t tier )
   {
      return push_action(ROA, "forcereg"_n, mvo()
           ("owner", owner)
           ("tier", tier)
      );
   }

   action_result extend_policy( account_name owner, account_name issuer, uint32_t new_time_block ) {
       return push_action(issuer, "extendpolicy"_n, mvo()
            ("owner", owner)
            ("issuer", issuer)
            ("newTimeBlock", new_time_block)
       );
   }

   abi_serializer abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_roa_tests)

BOOST_FIXTURE_TEST_CASE( newuser_happycase_test, sysio_roa_tester ) try {

   auto result = regnodeowner("alice"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   auto alice_owner = get_nodeowner("alice"_n);
   BOOST_REQUIRE_EQUAL(alice_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(alice_owner["tier"].as<uint32_t>(), 1);

   auto empty = get_sponsorship("alice"_n, "nonce1"_n);
   BOOST_REQUIRE_EQUAL(empty.is_null(), true);
   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("alice"_n));

   auto newuser_result = newuser("alice"_n, "nonce1"_n, get_public_key("alice"_n, "active"));
   BOOST_REQUIRE_EQUAL(2, newuser_result->action_traces.size());
   auto newuser_action_trace = newuser_result->action_traces[0];
   BOOST_REQUIRE_EQUAL(newuser_action_trace.act.name, "newuser"_n);
   BOOST_REQUIRE_EQUAL(newuser_action_trace.receiver, ROA);
   BOOST_REQUIRE_EQUAL(newuser_action_trace.act.account, ROA);
   auto new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
   BOOST_REQUIRE_NE(""_n, new_name);

   auto newaccount_action_trace = newuser_result->action_traces[1];
   BOOST_REQUIRE_EQUAL(newaccount_action_trace.act.name, "newaccount"_n);
   BOOST_REQUIRE_EQUAL(newaccount_action_trace.receiver, "sysio"_n);
   BOOST_REQUIRE_EQUAL(newaccount_action_trace.act.account, "sysio"_n);
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("alice"_n));
   auto sponsorship = get_sponsorship("alice"_n, "nonce1"_n);
   BOOST_REQUIRE_EQUAL(sponsorship.is_null(), false);
   BOOST_REQUIRE_NE(""_n, sponsorship["username"].as<name>());
   BOOST_REQUIRE_EQUAL(sponsorship["username"].as<name>(), new_name);

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( newuser_twice_test, sysio_roa_tester ) try {
   auto result = regnodeowner("alice"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   auto alice_owner = get_nodeowner("alice"_n);
   BOOST_REQUIRE_EQUAL(alice_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(alice_owner["tier"].as<uint32_t>(), 1);
   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("alice"_n));

   auto newuser_result = newuser("alice"_n, "nonce1"_n, get_public_key("alice"_n, "active"));
   BOOST_REQUIRE_EQUAL(2, newuser_result->action_traces.size());
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("alice"_n));

   // second attempt with same nonce should fail
   BOOST_REQUIRE_EXCEPTION(
      newuser("alice"_n, "nonce1"_n, get_public_key("alice"_n, "active")),
      sysio_assert_message_exception, sysio_assert_message_is("Sponsor entry for this nonce already exists"));

   // but with a different nonce, it creates a new unique account
   auto newuser_result2 = newuser("alice"_n, "nonce2"_n, get_public_key("alice"_n, "active"));
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL(2, get_sponsor_count("alice"_n));

   auto newuser_action_trace = newuser_result->action_traces[0];
   auto newuser_action_trace2 = newuser_result2->action_traces[0];
   auto new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
   auto new_name2 = fc::raw::unpack<name>(newuser_action_trace2.return_value);
   BOOST_REQUIRE_NE(new_name, new_name2);

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( newuser_creator_permission_test, sysio_roa_tester ) try {
   auto result = regnodeowner("alice"_n, 2);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   // bob is not a node owner, so he cannot create users
   BOOST_REQUIRE_EXCEPTION(
           newuser("bob"_n, "nonce1"_n, get_public_key("alice"_n, "active")),
           sysio_assert_message_exception, sysio_assert_message_is("Creator is not a registered tier-1 node owner"));

   // alice is only a tier 2 owner, so she cannot create users
   BOOST_REQUIRE_EXCEPTION(
           newuser("alice"_n, "nonce1"_n, get_public_key("alice"_n, "active")),
           sysio_assert_message_exception, sysio_assert_message_is("Creator is not a registered tier-1 node owner"));

   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("alice"_n));
   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("bob"_n));

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( newuser_multiple_creators_test, sysio_roa_tester ) try {
   auto result = regnodeowner("alice"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   result = regnodeowner("bob"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   auto alice_owner = get_nodeowner("alice"_n);
   BOOST_REQUIRE_EQUAL(alice_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(alice_owner["tier"].as<uint32_t>(), 1);

   auto bob_owner = get_nodeowner("bob"_n);
   BOOST_REQUIRE_EQUAL(bob_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(bob_owner["tier"].as<uint32_t>(), 1);

   auto same_nonce = "anynonce"_n;
   auto newuser_result = newuser("alice"_n, same_nonce, get_public_key("alice"_n, "active"));
   auto newuser_result2 = newuser("bob"_n, same_nonce, get_public_key("bob"_n, "active"));
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("alice"_n));
   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("bob"_n));

   auto newuser_action_trace = newuser_result->action_traces[0];
   auto newuser_action_trace2 = newuser_result2->action_traces[0];
   auto new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
   auto new_name2 = fc::raw::unpack<name>(newuser_action_trace2.return_value);
   BOOST_REQUIRE_NE(new_name, new_name2);

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( newuser_nonce_collision, sysio_roa_tester ) try {
   auto result = regnodeowner("alice"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   result = regnodeowner("bob"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   result = regnodeowner("carol"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   result = regnodeowner("darcy"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   auto alice_owner = get_nodeowner("alice"_n);
   BOOST_REQUIRE_EQUAL(alice_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(alice_owner["tier"].as<uint32_t>(), 1);
   auto bob_owner = get_nodeowner("bob"_n);
   BOOST_REQUIRE_EQUAL(bob_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(bob_owner["tier"].as<uint32_t>(), 1);
   auto carol_owner = get_nodeowner("carol"_n);
   BOOST_REQUIRE_EQUAL(carol_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(carol_owner["tier"].as<uint32_t>(), 1);
   auto darcy_owner = get_nodeowner("darcy"_n);
   BOOST_REQUIRE_EQUAL(darcy_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(darcy_owner["tier"].as<uint32_t>(), 1);


   // First three will succeed, but then we run out of collision attempts
   auto same_nonce = "inauspicious"_n;
   auto newuser_result = newuser("alice"_n, same_nonce, get_public_key("alice"_n, "active"));
   auto newuser_result2 = newuser("bob"_n, same_nonce, get_public_key("bob"_n, "active"));
   auto newuser_result3 = newuser("carol"_n, same_nonce, get_public_key("carol"_n, "active"));

   BOOST_REQUIRE_EXCEPTION(
        newuser("darcy"_n, same_nonce, get_public_key("darcy"_n, "active")),
        sysio_assert_message_exception,
        sysio_assert_message_is("Failed to generate a unique account name after 3 attempts"));

   produce_blocks(1);

   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("alice"_n));
   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("bob"_n));
   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("carol"_n));
   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("darcy"_n));

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( verify_ram, sysio_roa_tester ) try {
   // load system contract for newaccount functionality
   set_code( config::system_account_name, test_contracts::sysio_system_wasm() );
   set_abi( config::system_account_name, test_contracts::sysio_system_abi() );

   base_tester::push_action(config::system_account_name, "init"_n,
                            config::system_account_name, mutable_variant_object()
                            ("version", 0)
                            ("core", symbol(CORE_SYMBOL).to_string()));
   produce_block();

   // roa has been activated with NODE_DADDY as a node owner
   // Accounts already created with ROA policy { "alice"_n, "bob"_n, "carol"_n, "darcy"_n }
   int64_t ram; int64_t net; int64_t cpu;
   control->get_resource_limits_manager().get_account_limits( "sysio"_n, ram, net, cpu );
   const int64_t initial_sysio_ram = ram;
   auto r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == ram);
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // just itself
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram); // sysio.acct itself

   // create all node owners
   std::array<account_name, 21> node_owners = { NODE_DADDY };
   for (size_t i = 1 ; i < node_owners.size() ; i++) {
      std::string n = std::string("nodeowner").append(1, 'a' + i);
      node_owners[i] = account_name(n);
      create_accounts({node_owners[i]}, false, false, false, false);
      register_node_owner(node_owners[i], 1);
      control->get_resource_limits_manager().get_account_limits( node_owners[i], ram, net, cpu );
      BOOST_TEST(ram == 8320); // values set by contract, verify they don't change for this test
      BOOST_TEST(net == 500);
      BOOST_TEST(cpu == 500);
   }
   produce_block();

   // verify initial conditions of ROA accounts
   control->get_resource_limits_manager().get_account_limits( "sysio"_n, ram, net, cpu );
   const int64_t roa_sysio_ram = initial_sysio_ram + 6281267200;
   BOOST_TEST(ram == roa_sysio_ram); // ram after all tier-1 nodeowners
   control->get_resource_limits_manager().get_account_limits( "sysio.roa"_n, ram, net, cpu );
   BOOST_TEST(ram == 157021280); // ram of roa itself
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // just itself

   // create a roa::newuser and verify resources
   auto newuser = create_newuser(node_owners[2]);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // newuser has gifted amount of ram
   BOOST_TEST(net == 0);
   BOOST_TEST(cpu == 0);
   r = get_reslimit(newuser);
   BOOST_TEST(r.is_null()); // no reslimit for newuser
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 2*newaccount_ram); // newuser and sysio.acct itself
   auto p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for newuser
   p = get_policy(newuser, node_owners[2]);
   BOOST_TEST(p.is_null()); // no policy for newuser
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-newaccount_ram); // sysio ram gifted to newuser; reflected in sysio.acct
   p = get_policy("sysio.acct"_n, "sysio"_n);
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0027 SYS");  // newaccount_ram (2808 / bytesPerUnit 104) == 27

   // create another roa::newuser and verify resources
   auto newuser2 = create_newuser(node_owners[2]);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser2, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // newuser has gifted amount of ram
   BOOST_TEST(net == 0);
   BOOST_TEST(cpu == 0);
   r = get_reslimit(newuser2);
   BOOST_TEST(r.is_null()); // no reslimit for newuser
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 3*newaccount_ram); // newuser, newuser2, and sysio.acct itself
   p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser2, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for newuser
   p = get_policy(newuser2, node_owners[2]);
   BOOST_TEST(p.is_null()); // no policy for newuser
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-2*newaccount_ram); // sysio ram gifted to newuser & newuser2; reflected in sysio.acct
   p = get_policy("sysio.acct"_n, "sysio"_n);
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0054 SYS");  // 2*newaccount_ram 2*(2808 / bytesPerUnit 104) == 54

   // Provide newuser a policy and verify resources
   add_roa_policy(node_owners[2], newuser, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram+(320000*104)); // gifted ram plus the policy ram
   BOOST_TEST(net == 320000);
   BOOST_TEST(cpu == 320000);
   r = get_reslimit(newuser);
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram+(320000*104));
   BOOST_TEST(r["net_weight"].as_string() == "32.0000 SYS");
   BOOST_TEST(r["cpu_weight"].as_string() == "32.0000 SYS");
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 3*newaccount_ram); // newuser, newuser2, and sysio.acct itself (nothing for policy)
   p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser, "sysio"_n);
   BOOST_TEST(p.is_null()); // policy provided by node_owners[2], not sysio
   p = get_policy(newuser, node_owners[2]);
   BOOST_TEST(p["net_weight"].as_string() == "32.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "32.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "32.0000 SYS"); // new policy does not include the gifted RAM from sysio
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-2*newaccount_ram); // sysio ram not changed for a policy, same as before
   p = get_policy("sysio.acct"_n, "sysio"_n); // sysio.acct policy not changed for a user policy
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0054 SYS"); // 2*newaccount_ram 2*(2808 / bytesPerUnit 104) == 54 (nothing for policy)

   // Expand policy and verify resources:     net           cpu            ram
   expand_roa_policy(node_owners[2], newuser, "5.0000 SYS", "10.0000 SYS", "15.0000 SYS", 0);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram+(320000*104)+(150000*104)); // gifted ram plus the policy ram
   BOOST_TEST(net == 320000+50000);
   BOOST_TEST(cpu == 320000+100000);
   r = get_reslimit(newuser);
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram+(320000*104)+(150000*104));
   BOOST_TEST(r["net_weight"].as_string() == "37.0000 SYS");
   BOOST_TEST(r["cpu_weight"].as_string() == "42.0000 SYS");
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 3*newaccount_ram); // newuser, newuser2, and sysio.acct itself (nothing for policy)
   p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser, "sysio"_n);
   BOOST_TEST(p.is_null()); // policy provided by node_owners[2], not sysio
   p = get_policy(newuser, node_owners[2]);
   BOOST_TEST(p["net_weight"].as_string() == "37.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "42.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "47.0000 SYS"); // policy does not include the gifted RAM from sysio
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-2*newaccount_ram); // sysio ram not changed for a policy, same as before
   p = get_policy("sysio.acct"_n, "sysio"_n); // sysio.acct policy not changed for a user policy
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0054 SYS"); // 2*newaccount_ram 2*(2808 / bytesPerUnit 104) == 54 (nothing for policy)

   // Add policy from another node owner:     net           cpu            ram
   add_roa_policy(node_owners[3], newuser, "1.0000 SYS", "2.0000 SYS", "3.0000 SYS", 0, 0);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram+(320000*104)+(150000*104)+(30000*104)); // gifted ram plus the policy ram
   BOOST_TEST(net == 320000+50000+10000);
   BOOST_TEST(cpu == 320000+100000+20000);
   r = get_reslimit(newuser); // reflects both policies from node_owner 2 & 3
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram+(320000*104)+(150000*104)+(30000*104));
   BOOST_TEST(r["net_weight"].as_string() == "38.0000 SYS");
   BOOST_TEST(r["cpu_weight"].as_string() == "44.0000 SYS");
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 3*newaccount_ram); // newuser, newuser2, and sysio.acct itself (nothing for policy)
   p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser, "sysio"_n);
   BOOST_TEST(p.is_null()); // policy provided by node_owners[2], not sysio
   p = get_policy(newuser, node_owners[2]); // node owner 2
   BOOST_TEST(p["net_weight"].as_string() == "37.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "42.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "47.0000 SYS");
   p = get_policy(newuser, node_owners[3]); // node owner 3
   BOOST_TEST(p["net_weight"].as_string() == "1.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "2.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "3.0000 SYS");
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-2*newaccount_ram); // sysio ram not changed for a policy, same as before
   p = get_policy("sysio.acct"_n, "sysio"_n); // sysio.acct policy not changed for a user policy
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0054 SYS"); // 2*newaccount_ram 2*(2808 / bytesPerUnit 104) == 54 (nothing for policy)

   produce_block();

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( extend_policy_test, sysio_roa_tester ) try {
    auto result = regnodeowner("alice"_n, 1);
    BOOST_REQUIRE_EQUAL(success(), result);
    produce_blocks(1);

    result = regnodeowner("bob"_n, 1);
    BOOST_REQUIRE_EQUAL(success(), result);
    produce_blocks(1);

    auto p = get_policy("alice"_n, "alice"_n);
    BOOST_TEST(p["owner"].as_string() == "alice");
    BOOST_TEST(p["issuer"].as_string() == "alice");
    BOOST_TEST(p["bytes_per_unit"].as_string() == "104");
    BOOST_TEST(p["time_block"].as_string() == "1");
    p = get_policy("sysio"_n, "alice"_n);
    BOOST_TEST(p["owner"].as_string() == "sysio");
    BOOST_TEST(p["issuer"].as_string() == "alice");
    BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
    BOOST_TEST(p["time_block"].as_string() == "4294967295");
    BOOST_TEST(p["ram_weight"].as_string() == "301.9840 SYS");

    // extend policy
    extend_policy("alice"_n, "alice"_n, 42);

    p = get_policy("alice"_n, "alice"_n);
    BOOST_TEST(p["owner"].as_string() == "alice");
    BOOST_TEST(p["issuer"].as_string() == "alice");
    BOOST_TEST(p["time_block"].as_string() == "42");
    p = get_policy("sysio"_n, "alice"_n);
    BOOST_TEST(p["owner"].as_string() == "sysio");
    BOOST_TEST(p["issuer"].as_string() == "alice");
    BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
    BOOST_TEST(p["time_block"].as_string() == "4294967295");

    result = extend_policy("sysio"_n, "alice"_n, 42);
    BOOST_REQUIRE_EQUAL(error("assertion failure with message: Cannot reduce a policies existing time_block"), result);

    expand_roa_policy("alice"_n, "sysio"_n, "0.0000 SYS", "0.0000 SYS", "500.0000 SYS", 0);

    p = get_policy("sysio"_n, "alice"_n);
    BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
    BOOST_TEST(p["ram_weight"].as_string() == "801.9840 SYS");

} FC_LOG_AND_RETHROW()



BOOST_AUTO_TEST_SUITE_END()
