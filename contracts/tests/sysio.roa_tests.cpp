#include <boost/test/unit_test.hpp>
#include <string>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include "sysio.system_tester.hpp"

#include <fc/variant_object.hpp>
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

      create_accounts( { "alice"_n, "bob"_n, "carol"_n, "darcy"_n } );
      produce_blocks( 2 );

      set_code( ROA, contracts::roa_wasm() );
      set_abi( ROA, contracts::roa_abi().data() );

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

   transaction_trace_ptr newuser( account_name creator, name nonce, fc::crypto::public_key pubkey)
   {
      return push_paid_action( creator, "newuser"_n, mvo()
           ( "creator", creator)
           ( "nonce", nonce )
           ( "pubkey", pubkey )
      );
   }


   action_result regnodeowner( account_name owner, uint8_t tier )
   {
      return push_action(ROA, "forcereg"_n, mvo()
           ("owner", owner)
           ("tier", tier)
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

BOOST_AUTO_TEST_SUITE_END()
