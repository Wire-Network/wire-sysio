#include <test_contracts.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include "sysio.system_tester.hpp"
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

using sysio::chain::make_kv_scoped_key;
using sysio::chain::compute_table_id;

constexpr account_name ROA = "sysio.roa"_n;
constexpr uint64_t NETWORK_GEN = 0;

class sysio_roa_tester : public tester {
public:

   sysio_roa_tester() {
      produce_blocks( 2 );

      create_accounts( { "alice"_n, "bob"_n, "carol"_n, "darcy"_n }, false, false, false, false );
      produce_blocks( 2 );

      produce_blocks();

      const auto* accnt = control->find_account_metadata( ROA );
      BOOST_REQUIRE( accnt != nullptr );
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
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
         vector<permission_level>{{signer, "sysio.payer"_n},{signer, "active"_n}},
         data);
   }

   fc::variant get_nodeowner( account_name acc )
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(static_cast<uint64_t>(NETWORK_GEN), acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id(name("nodeowners").to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            return abi_ser.binary_to_variant( "nodeowners", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
         }
      }
      return fc::variant();
   }

   fc::variant get_sponsorship( account_name acc, account_name nonce)
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(acc, nonce.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id("sponsors"_n.to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            return abi_ser.binary_to_variant( "sponsor", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
         }
      }
      return fc::variant();
   }

   uint64_t get_sponsor_count( account_name acc )
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(static_cast<uint64_t>(NETWORK_GEN), acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id(name("sponsorcount").to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            auto record = abi_ser.binary_to_variant("sponsorcount", data, abi_serializer::create_yield_function(abi_serializer_max_time));
            return record["count"].as<uint64_t>();
         }
      }
      return 0;
   }

   fc::variant get_reslimit( account_name acc )
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(ROA, acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id("reslimit"_n.to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            return abi_ser.binary_to_variant( "reslimit", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
         }
      }
      return fc::variant();
   }

   fc::variant get_policy( account_name acc, account_name owner )
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(owner, acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id("policies"_n.to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            return abi_ser.binary_to_variant( "policies", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
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
            ("new_time_block", new_time_block)
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

BOOST_FIXTURE_TEST_CASE( newuser_tld_test, sysio_roa_tester ) try {

   create_accounts( { "alice.com"_n, "bob.m"_n, "a.longonexxx"_n }, false, false, false, false );
   produce_blocks(1);
   auto result = regnodeowner("alice.com"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   result = regnodeowner("bob.m"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   result = regnodeowner("a.longonexxx"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   auto alice_owner = get_nodeowner("alice.com"_n);
   BOOST_REQUIRE_EQUAL(alice_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(alice_owner["tier"].as<uint32_t>(), 1);

   auto empty = get_sponsorship("alice.com"_n, "nonce1"_n);
   BOOST_REQUIRE_EQUAL(empty.is_null(), true);
   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("alice.com"_n));

   auto newuser_result = newuser("alice.com"_n, "nonce1"_n, get_public_key("alice.com"_n, "active"));
   BOOST_REQUIRE_EQUAL(2, newuser_result->action_traces.size());
   auto newuser_action_trace = newuser_result->action_traces[0];
   BOOST_REQUIRE_EQUAL(newuser_action_trace.act.name, "newuser"_n);
   BOOST_REQUIRE_EQUAL(newuser_action_trace.receiver, ROA);
   BOOST_REQUIRE_EQUAL(newuser_action_trace.act.account, ROA);
   auto new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
   BOOST_REQUIRE_NE(""_n, new_name);
   BOOST_TEST(new_name.suffix() == "alice.com"_n.suffix());
   BOOST_TEST(new_name.suffix() == "com"_n);

   auto newaccount_action_trace = newuser_result->action_traces[1];
   BOOST_REQUIRE_EQUAL(newaccount_action_trace.act.name, "newaccount"_n);
   BOOST_REQUIRE_EQUAL(newaccount_action_trace.receiver, "sysio"_n);
   BOOST_REQUIRE_EQUAL(newaccount_action_trace.act.account, "sysio"_n);
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("alice.com"_n));
   auto sponsorship = get_sponsorship("alice.com"_n, "nonce1"_n);
   BOOST_REQUIRE_EQUAL(sponsorship.is_null(), false);
   BOOST_REQUIRE_EQUAL(sponsorship["username"].as<name>(), new_name);

   newuser_result = newuser("bob.m"_n, "nonce1"_n, get_public_key("bob.m"_n, "active"));
   BOOST_REQUIRE_EQUAL(2, newuser_result->action_traces.size());
   newuser_action_trace = newuser_result->action_traces[0];
   new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
   BOOST_TEST(new_name.suffix() == "m"_n);

   newuser_result = newuser("a.longonexxx"_n, "nonce1"_n, get_public_key("a.longonexxx"_n, "active"));
   BOOST_REQUIRE_EQUAL(2, newuser_result->action_traces.size());
   newuser_action_trace = newuser_result->action_traces[0];
   new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
   BOOST_TEST(new_name.suffix() == "longonexxx"_n);

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
   BOOST_TEST(p["ram_weight"].as_string() == "0.0011 SYS");  // (newaccount_ram=1144 / bytes_per_unit=104) == 11

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
   BOOST_TEST(p["ram_weight"].as_string() == "0.0022 SYS");  // 2*(newaccount_ram=1144 / bytes_per_unit=104) == 22

   // Provide newuser a policy and verify resources
   add_roa_policy(node_owners[2], newuser, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram+(320000l*104)); // gifted ram plus the policy ram
   BOOST_TEST(net == 320000l);
   BOOST_TEST(cpu == 320000l);
   r = get_reslimit(newuser);
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram+(320000l*104));
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
   BOOST_TEST(p["ram_weight"].as_string() == "0.0022 SYS"); // 2*(newaccount_ram=1144 / bytes_per_unit=104) == 22 (nothing for policy)

   // Expand policy and verify resources:     net           cpu            ram
   expand_roa_policy(node_owners[2], newuser, "5.0000 SYS", "10.0000 SYS", "15.0000 SYS", 0);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram+(320000l*104)+(150000l*104)); // gifted ram plus the policy ram
   BOOST_TEST(net == 320000+50000);
   BOOST_TEST(cpu == 320000+100000);
   r = get_reslimit(newuser);
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram+(320000l*104)+(150000l*104));
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
   BOOST_TEST(p["ram_weight"].as_string() == "0.0022 SYS"); // 2*(newaccount_ram=1144 / bytes_per_unit=104) == 22 (nothing for policy)

   // Add policy from another node owner:     net           cpu            ram
   add_roa_policy(node_owners[3], newuser, "1.0000 SYS", "2.0000 SYS", "3.0000 SYS", 0, 0);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram+(320000l*104)+(150000l*104)+(30000l*104)); // gifted ram plus the policy ram
   BOOST_TEST(net == 320000l+50000l+10000l);
   BOOST_TEST(cpu == 320000l+100000l+20000l);
   r = get_reslimit(newuser); // reflects both policies from node_owner 2 & 3
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram+(320000l*104)+(150000l*104)+(30000l*104));
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
   BOOST_TEST(p["ram_weight"].as_string() == "0.0022 SYS"); // 2*(newaccount_ram=1144 / bytes_per_unit=104) == 22 (nothing for policy)

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

    BOOST_CHECK_EXCEPTION(reduce_roa_policy("alice"_n, "sysio"_n, "0.0000 SYS", "0.0000 SYS", "500.0000 SYS", 0),
                          sysio_assert_message_exception,
                          sysio_assert_message_is("Cannot reduce policy before time_block"));

    expand_roa_policy("alice"_n, "sysio"_n, "0.0000 SYS", "0.0000 SYS", "500.0000 SYS", 0);

    p = get_policy("sysio"_n, "alice"_n);
    BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
    BOOST_TEST(p["ram_weight"].as_string() == "801.9840 SYS");

} FC_LOG_AND_RETHROW()

// Verifies that reducepolicy correctly decreases RAM bytes in the reslimit table.
BOOST_FIXTURE_TEST_CASE( reducepolicy_ram_accounting, sysio_roa_tester ) try {
   // Load system contract for newaccount + addpolicy/reducepolicy
   set_code( config::system_account_name, test_contracts::sysio_system_wasm() );
   set_abi( config::system_account_name, test_contracts::sysio_system_abi() );
   base_tester::push_action(config::system_account_name, "init"_n,
                            config::system_account_name, mutable_variant_object()
                            ("version", 0)
                            ("core", symbol(CORE_SYMBOL).to_string()));
   produce_block();

   // Register node owners (need 21 for ROA activation)
   std::array<account_name, 21> node_owners = { NODE_DADDY };
   for (size_t i = 1; i < node_owners.size(); i++) {
      std::string n = std::string("nodeowner").append(1, 'a' + i);
      node_owners[i] = account_name(n);
      create_accounts({node_owners[i]}, false, false, false, false);
      register_node_owner(node_owners[i], 1);
   }
   produce_block();

   // Create a user and add a policy with time_block=0 (immediately reducible)
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   produce_block();

   // Record pre-reduce state
   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   auto r = get_reslimit(user);
   int64_t pre_reslimit_ram = r["ram_bytes"].as_int64();

   // Verify initial state: newaccount_ram + policy RAM (320000 * 104 = 33,280,000)
   BOOST_TEST(pre_reslimit_ram == ram);
   BOOST_TEST(ram == (int64_t)(newaccount_ram + 320000l * 104));
   BOOST_TEST(net == 320000);
   BOOST_TEST(cpu == 320000);

   // Reduce the policy by half
   reduce_roa_policy(node_owners[2], user, "16.0000 SYS", "16.0000 SYS", "16.0000 SYS", 0);
   produce_block();

   // Verify post-reduce state
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   r = get_reslimit(user);
   int64_t post_reslimit_ram = r["ram_bytes"].as_int64();

   // C4 fix: reslimit ram_bytes should decrease, NOT nearly double
   // Expected: pre_reslimit_ram - (160000 * 104)
   int64_t expected_ram = pre_reslimit_ram - 160000l * 104;
   BOOST_TEST(post_reslimit_ram == expected_ram);
   BOOST_TEST(post_reslimit_ram == ram);  // reslimit table matches system limits
   BOOST_TEST(post_reslimit_ram < pre_reslimit_ram);

   // Verify NET/CPU decreased correctly
   BOOST_TEST(net == 160000);
   BOOST_TEST(cpu == 160000);

   // Verify policy updated
   auto p = get_policy(user, node_owners[2]);
   BOOST_TEST(p["net_weight"].as_string() == "16.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "16.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "16.0000 SYS");

} FC_LOG_AND_RETHROW()

// Verifies that reducing an entire policy to zero correctly removes it and
// that a second reducepolicy on a different policy also works correctly.
BOOST_FIXTURE_TEST_CASE( reducepolicy_full_then_second, sysio_roa_tester ) try {
   set_code( config::system_account_name, test_contracts::sysio_system_wasm() );
   set_abi( config::system_account_name, test_contracts::sysio_system_abi() );
   base_tester::push_action(config::system_account_name, "init"_n,
                            config::system_account_name, mutable_variant_object()
                            ("version", 0)
                            ("core", symbol(CORE_SYMBOL).to_string()));
   produce_block();

   std::array<account_name, 21> node_owners = { NODE_DADDY };
   for (size_t i = 1; i < node_owners.size(); i++) {
      std::string n = std::string("nodeowner").append(1, 'a' + i);
      node_owners[i] = account_name(n);
      create_accounts({node_owners[i]}, false, false, false, false);
      register_node_owner(node_owners[i], 1);
   }
   produce_block();

   auto user = create_newuser(node_owners[2]);
   produce_block();

   // Add two policies from different node owners
   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   add_roa_policy(node_owners[3], user, "20.0000 SYS", "20.0000 SYS", "20.0000 SYS", 0, 0);
   produce_block();

   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   // newaccount_ram + (100000+200000)*104
   BOOST_TEST(ram == (int64_t)(newaccount_ram + 100000l * 104 + 200000l * 104));
   BOOST_TEST(net == 100000 + 200000);
   BOOST_TEST(cpu == 100000 + 200000);

   // Fully reduce the first policy
   reduce_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   auto r = get_reslimit(user);

   // Only the second policy's RAM remains plus newaccount_ram
   BOOST_TEST(ram == (int64_t)(newaccount_ram + 200000l * 104));
   BOOST_TEST(r["ram_bytes"].as_int64() == ram);
   BOOST_TEST(net == 200000);
   BOOST_TEST(cpu == 200000);

   // First policy should be removed
   auto p = get_policy(user, node_owners[2]);
   BOOST_TEST(p.is_null());

   // Second policy still intact
   p = get_policy(user, node_owners[3]);
   BOOST_TEST(p["net_weight"].as_string() == "20.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "20.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "20.0000 SYS");

   // Now reduce the second policy partially
   reduce_roa_policy(node_owners[3], user, "5.0000 SYS", "5.0000 SYS", "5.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   r = get_reslimit(user);

   BOOST_TEST(ram == (int64_t)(newaccount_ram + 150000l * 104));
   BOOST_TEST(r["ram_bytes"].as_int64() == ram);
   BOOST_TEST(net == 150000);
   BOOST_TEST(cpu == 150000);

} FC_LOG_AND_RETHROW()

// Verifies that creating multiple users correctly decreases sysio's reslimit RAM.
BOOST_FIXTURE_TEST_CASE( newuser_sysio_ram_decreases, sysio_roa_tester ) try {
   set_code( config::system_account_name, test_contracts::sysio_system_wasm() );
   set_abi( config::system_account_name, test_contracts::sysio_system_abi() );
   base_tester::push_action(config::system_account_name, "init"_n,
                            config::system_account_name, mutable_variant_object()
                            ("version", 0)
                            ("core", symbol(CORE_SYMBOL).to_string()));
   produce_block();

   std::array<account_name, 21> node_owners = { NODE_DADDY };
   for (size_t i = 1; i < node_owners.size(); i++) {
      std::string n = std::string("nodeowner").append(1, 'a' + i);
      node_owners[i] = account_name(n);
      create_accounts({node_owners[i]}, false, false, false, false);
      register_node_owner(node_owners[i], 1);
   }
   produce_block();

   // Record sysio's initial RAM
   auto r = get_reslimit("sysio"_n);
   int64_t sysio_ram_before = r["ram_bytes"].as_int64();
   BOOST_TEST(sysio_ram_before > 0);

   // Create 5 users - each should subtract newaccount_ram from sysio
   for (int i = 0; i < 5; i++) {
      create_newuser(node_owners[2]);
      produce_block();
   }

   r = get_reslimit("sysio"_n);
   int64_t sysio_ram_after = r["ram_bytes"].as_int64();

   // Sysio's reslimit should have decreased by exactly 5 * newaccount_ram
   BOOST_TEST(sysio_ram_after == sysio_ram_before - 5 * (int64_t)newaccount_ram);
   BOOST_TEST(sysio_ram_after > 0);  // should still be positive

   // sysio.acct's reslimit should have increased by 5 * newaccount_ram (plus its own)
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == (int64_t)((5 + 1) * newaccount_ram));  // 5 users + sysio.acct itself

} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Helper fixture: sysio_roa_tester with system contract + 21 node owners
// ---------------------------------------------------------------------------
class sysio_roa_full_tester : public sysio_roa_tester {
public:
   std::array<account_name, 21> node_owners;

   sysio_roa_full_tester() {
      set_code( config::system_account_name, test_contracts::sysio_system_wasm() );
      set_abi( config::system_account_name, test_contracts::sysio_system_abi() );
      base_tester::push_action(config::system_account_name, "init"_n,
                               config::system_account_name, mutable_variant_object()
                               ("version", 0)
                               ("core", symbol(CORE_SYMBOL).to_string()));
      produce_block();

      node_owners[0] = NODE_DADDY;
      for (size_t i = 1; i < node_owners.size(); i++) {
         std::string n = std::string("nodeowner").append(1, 'a' + i);
         node_owners[i] = account_name(n);
         create_accounts({node_owners[i]}, false, false, false, false);
         register_node_owner(node_owners[i], 1);
      }
      produce_block();
   }
};

// ===== 1. addpolicy validation =====

// Non-node-owner cannot issue a policy
BOOST_FIXTURE_TEST_CASE( addpolicy_non_nodeowner, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // bob is not a node owner, so addpolicy should fail
   BOOST_CHECK_EXCEPTION(
      add_roa_policy("bob"_n, user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Only Node Owners can issue policies for this generation."));
} FC_LOG_AND_RETHROW()

// Duplicate policy from same issuer should fail
BOOST_FIXTURE_TEST_CASE( addpolicy_duplicate, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("A policy for this owner already exists from this issuer. Use expandpolicy instead."));
} FC_LOG_AND_RETHROW()

// All-zero allocation should fail
BOOST_FIXTURE_TEST_CASE( addpolicy_all_zero, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], user, "0.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("At least one of NET, CPU, or RAM must be allocated."));
} FC_LOG_AND_RETHROW()

// Exceeding available SYS should fail
BOOST_FIXTURE_TEST_CASE( addpolicy_insufficient_sys, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // Each T1 node owner gets ~4% of total SYS. Try to allocate way more than that.
   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], user, "900000000.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Not enough unallocated SYS for this policy."));
} FC_LOG_AND_RETHROW()

// Cannot allocate CPU/NET to sysio.* accounts
BOOST_FIXTURE_TEST_CASE( addpolicy_sysio_no_bw, sysio_roa_full_tester ) try {
   create_accounts({"sysio.test1"_n}, false, false, false, false);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], "sysio.test1"_n, "1.0000 SYS", "0.0000 SYS", "1.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot allocate CPU/NET to sysio accounts."));
} FC_LOG_AND_RETHROW()

// RAM-only policy for sysio.* accounts should succeed
BOOST_FIXTURE_TEST_CASE( addpolicy_sysio_ram_only, sysio_roa_full_tester ) try {
   create_accounts({"sysio.test2"_n}, false, false, false, false);
   produce_block();

   add_roa_policy(node_owners[2], "sysio.test2"_n, "0.0000 SYS", "0.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   auto p = get_policy("sysio.test2"_n, node_owners[2]);
   BOOST_TEST(!p.is_null());
   BOOST_TEST(p["ram_weight"].as_string() == "1.0000 SYS");
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
} FC_LOG_AND_RETHROW()

// ===== 2. expandpolicy validation =====

// Expand non-existent policy should fail
BOOST_FIXTURE_TEST_CASE( expandpolicy_no_policy, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // No policy exists from node_owners[3] for this user
   BOOST_CHECK_EXCEPTION(
      expand_roa_policy(node_owners[3], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("You have no policy for this owner."));
} FC_LOG_AND_RETHROW()

// Non-node-owner cannot expand
BOOST_FIXTURE_TEST_CASE( expandpolicy_non_nodeowner, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      expand_roa_policy("bob"_n, user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Only Node Owners can manage policies."));
} FC_LOG_AND_RETHROW()

// All-zero expand should fail
BOOST_FIXTURE_TEST_CASE( expandpolicy_all_zero, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      expand_roa_policy(node_owners[2], user, "0.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("At least one of NET, CPU, or RAM must be increased."));
} FC_LOG_AND_RETHROW()

// Expanding beyond available SYS should fail
BOOST_FIXTURE_TEST_CASE( expandpolicy_insufficient_sys, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      expand_roa_policy(node_owners[2], user, "900000000.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Issuer does not have enough unallocated SYS for this policy expansion."));
} FC_LOG_AND_RETHROW()

// Cannot expand CPU/NET for sysio.* accounts
BOOST_FIXTURE_TEST_CASE( expandpolicy_sysio_no_bw, sysio_roa_full_tester ) try {
   create_accounts({"sysio.test3"_n}, false, false, false, false);
   produce_block();

   // First add a RAM-only policy
   add_roa_policy(node_owners[2], "sysio.test3"_n, "0.0000 SYS", "0.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      expand_roa_policy(node_owners[2], "sysio.test3"_n, "1.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot allocate CPU/NET to sysio accounts."));
} FC_LOG_AND_RETHROW()

// ===== 3. reducepolicy validation =====

// Reduce non-existent policy should fail
BOOST_FIXTURE_TEST_CASE( reducepolicy_no_policy, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[3], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("You have no policy for this owner."));
} FC_LOG_AND_RETHROW()

// Non-node-owner cannot reduce
BOOST_FIXTURE_TEST_CASE( reducepolicy_non_nodeowner, sysio_roa_full_tester ) try {
   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy("bob"_n, "alice"_n, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Only Node Owners can manage policies."));
} FC_LOG_AND_RETHROW()

// Cannot reduce NET below zero
BOOST_FIXTURE_TEST_CASE( reducepolicy_below_zero, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   produce_block();

   // Try to reduce more NET than allocated
   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "11.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot reduce NET below zero"));
} FC_LOG_AND_RETHROW()

// Cannot reduce before time_block
BOOST_FIXTURE_TEST_CASE( reducepolicy_before_timeblock, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // Set time_block far in the future
   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 999999999, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "5.0000 SYS", "5.0000 SYS", "5.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot reduce policy before time_block"));
} FC_LOG_AND_RETHROW()

// ===== 8. Multi-node-owner policy interactions =====

// Expand after reduce on the same policy
BOOST_FIXTURE_TEST_CASE( expand_after_reduce, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "20.0000 SYS", "20.0000 SYS", "20.0000 SYS", 0, 0);
   produce_block();

   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   int64_t ram_after_add = ram;

   // Reduce by half
   reduce_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(net == 100000);
   BOOST_TEST(cpu == 100000);

   // Expand back
   expand_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(ram == ram_after_add);
   BOOST_TEST(net == 200000);
   BOOST_TEST(cpu == 200000);

   auto p = get_policy(user, node_owners[2]);
   BOOST_TEST(p["net_weight"].as_string() == "20.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "20.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "20.0000 SYS");
} FC_LOG_AND_RETHROW()

// Reduce one issuer's policy doesn't affect another issuer's policy
BOOST_FIXTURE_TEST_CASE( reduce_one_issuer_isolates_other, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // Two policies from different issuers
   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   add_roa_policy(node_owners[3], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   produce_block();

   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(net == 200000);
   BOOST_TEST(cpu == 200000);

   // Reduce only node_owners[2]'s policy completely
   reduce_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0);
   produce_block();

   // node_owners[3]'s policy should be unchanged
   auto p = get_policy(user, node_owners[3]);
   BOOST_TEST(p["net_weight"].as_string() == "10.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "10.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "10.0000 SYS");

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(net == 100000);
   BOOST_TEST(cpu == 100000);

   // Can still expand node_owners[3]'s policy
   expand_roa_policy(node_owners[3], user, "5.0000 SYS", "5.0000 SYS", "5.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(net == 150000);
   BOOST_TEST(cpu == 150000);
} FC_LOG_AND_RETHROW()

// ===== 9. newuser edge cases =====

// Creating many users depletes sysio RAM predictably
BOOST_FIXTURE_TEST_CASE( newuser_ram_depletion_tracking, sysio_roa_full_tester ) try {
   auto r = get_reslimit("sysio"_n);
   int64_t sysio_ram_start = r["ram_bytes"].as_int64();

   // Create users from multiple node owners and verify accounting
   auto user1 = create_newuser(node_owners[2]);
   auto user2 = create_newuser(node_owners[3]);
   auto user3 = create_newuser(node_owners[4]);
   produce_block();

   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == sysio_ram_start - 3 * (int64_t)newaccount_ram);

   // Each new user should have exactly newaccount_ram
   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user1, ram, net, cpu);
   BOOST_TEST(ram == (int64_t)newaccount_ram);
   control->get_resource_limits_manager().get_account_limits(user2, ram, net, cpu);
   BOOST_TEST(ram == (int64_t)newaccount_ram);
   control->get_resource_limits_manager().get_account_limits(user3, ram, net, cpu);
   BOOST_TEST(ram == (int64_t)newaccount_ram);

   // sysio.acct reslimit tracks all gifted RAM
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == (int64_t)((3 + 1) * newaccount_ram));
} FC_LOG_AND_RETHROW()

// Non-tier-1 node owner cannot create users
BOOST_FIXTURE_TEST_CASE( newuser_tier2_fails, sysio_roa_full_tester ) try {
   create_accounts({"tier2owner"_n}, false, false, false, false);
   register_node_owner("tier2owner"_n, 2);
   produce_block();

   BOOST_REQUIRE_EXCEPTION(
      newuser("tier2owner"_n, "nonce1"_n, get_public_key("tier2owner"_n, "active")),
      sysio_assert_message_exception,
      sysio_assert_message_is("Creator is not a registered tier-1 node owner"));
} FC_LOG_AND_RETHROW()

// newuser correctly populates both policies table and reslimit for sysio.acct
BOOST_FIXTURE_TEST_CASE( newuser_sysio_acct_policy_tracking, sysio_roa_full_tester ) try {
   auto p = get_policy("sysio.acct"_n, "sysio"_n);
   int64_t initial_ram_weight = p["ram_weight"].as<asset>().get_amount();

   create_newuser(node_owners[2]);
   create_newuser(node_owners[2]);
   produce_block();

   p = get_policy("sysio.acct"_n, "sysio"_n);
   int64_t updated_ram_weight = p["ram_weight"].as<asset>().get_amount();

   // Each newuser adds newaccount_ram / bytes_per_unit to the sysio.acct policy ram_weight
   int64_t ram_weight_per_user = (int64_t)newaccount_ram / 104;  // bytes_per_unit = 104
   BOOST_TEST(updated_ram_weight == initial_ram_weight + 2 * ram_weight_per_user);
} FC_LOG_AND_RETHROW()

// ===== 10. extendpolicy validation =====

// Extend non-existent policy should fail
BOOST_FIXTURE_TEST_CASE( extendpolicy_no_policy, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // No policy from node_owners[3] for this user
   auto result = extend_policy(user, node_owners[3], 100);
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: Policy does not exist under this issuer for this owner"), result);
} FC_LOG_AND_RETHROW()

// Cannot reduce a policy's time_block via extend
BOOST_FIXTURE_TEST_CASE( extendpolicy_cannot_reduce_timeblock, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 1000, 0);
   produce_block();

   // Try to set a lower time_block
   auto result = extend_policy(user, node_owners[2], 500);
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: Cannot reduce a policies existing time_block"), result);
} FC_LOG_AND_RETHROW()

// Cannot set time_block lower than current block
BOOST_FIXTURE_TEST_CASE( extendpolicy_past_timeblock, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // time_block=0 means already expired
   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   produce_block();

   // Try to extend to block 1 which is in the past
   auto result = extend_policy(user, node_owners[2], 1);
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: You cannot set a time_block lower than the current block"), result);
} FC_LOG_AND_RETHROW()

// Successful extend followed by reduce respects new time_block
BOOST_FIXTURE_TEST_CASE( extendpolicy_blocks_reduce, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   produce_block();

   // Extend to far future
   extend_policy(user, node_owners[2], 999999999);

   // Now reduce should fail because of the new time_block
   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "5.0000 SYS", "5.0000 SYS", "5.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot reduce policy before time_block"));

   // But expand should still work
   BOOST_REQUIRE_NO_THROW(
      expand_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
