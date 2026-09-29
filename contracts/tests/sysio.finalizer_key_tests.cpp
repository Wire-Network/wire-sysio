#include "sysio.system_tester.hpp"
#include "finalizer_test_keys.hpp"
#include "finalizer_registration_vector.hpp"

#include <sysio/chain/kv_table_objects.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <fc/crypto/bls_signature.hpp>
#include <sysio/opp/opp.hpp>
#include <boost/test/unit_test.hpp>

using namespace sysio_system;
using namespace sysio_test;

struct finalizer_key_tester : sysio_system_tester {
   // Derive keys at fixture construction, after the BLS library's static initialization.
   const std::vector<key_pair_t>& key_pairs = get_finalizer_test_keys();
   const key_pair_t fixture_key_1{41};
   const key_pair_t fixture_key_2{42};
   const key_pair_t fixture_key_3{43};
   const key_pair_t fixture_key_4{44};
   const std::string finalizer_key_1 = fixture_key_1.pub_key;
   const std::string finalizer_key_2 = fixture_key_2.pub_key;
   const std::string finalizer_key_3 = fixture_key_3.pub_key;
   const std::string finalizer_key_4 = fixture_key_4.pub_key;
   const std::string finalizer_key_binary_1 = fixture_key_1.binary_hex();
   const std::string finalizer_key_binary_2 = fixture_key_2.binary_hex();
   const std::string finalizer_key_binary_3 = fixture_key_3.binary_hex();
   const std::string finalizer_key_binary_4 = fixture_key_4.binary_hex();

   finalizer_key_tester() {
      // Finalizer tests use Alice and Bob as producers directly. Producer registration now
      // requires an ACTIVE producer operator, so model them as genesis fixtures up front.
      deploy_opreg_once();
      register_producer_operators({"alice1111111"_n, "bob111111111"_n});
   }

   fc::variant get_finalizer_key_info( uint64_t id ) {
      vector<char> data = get_row_by_id( config::system_account_name, config::system_account_name, "finkeys"_n, id );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "finalizer_key_info", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
   }

   fc::variant get_finalizer_info( const account_name& act ) {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, "finalizers"_n, act );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "finalizer_info", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
   }

   std::vector<finalizer_auth_info> get_last_prop_finalizers_info() {
      return sysio_test::get_last_prop_finalizers(*this, abi_ser);
   };

   std::unordered_set<uint64_t> get_last_prop_fin_ids() {
      return sysio_test::get_last_prop_fin_ids(*this, abi_ser);
   };

   action_result register_finalizer_key( const account_name& act, const std::string& finalizer_key, const std::string& pop  ) {
      return push_action( act, "regfinkey"_n, mvo()
                          ("finalizer_name", act)
                          ("finalizer_key", finalizer_key)
                          ("proof_of_possession", pop) );
   }

   action_result activate_finalizer_key( const account_name& act, const std::string& finalizer_key ) {
      return push_action( act, "actfinkey"_n, mvo()
                          ("finalizer_name",  act)
                          ("finalizer_key", finalizer_key) );
   }

   action_result delete_finalizer_key( const account_name& act, const std::string& finalizer_key ) {
      return push_action( act, "delfinkey"_n, mvo()
                          ("finalizer_name",  act)
                          ("finalizer_key", finalizer_key) );
   }

   void register_finalizer_keys(const std::vector<name>& producer_names, uint32_t num_keys_to_register) {
      uint32_t i = 0;
      for (const auto& p: producer_names) {
         BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(p, key_pairs[i].pub_key, key_pairs[i].proof(p)));
         ++i;

         if ( i  == num_keys_to_register ) {
            break;
         }
      }
   }


   // Verify finalizers_table and last_prop_fins_table match
   void verify_last_proposed_finalizers(const std::vector<name>& producer_names) {
      auto last_finalizers = get_last_prop_finalizers_info();
      BOOST_REQUIRE_EQUAL( 21, last_finalizers.size() );
      for( auto& p : producer_names ) {
         auto finalizer_info = get_finalizer_info(p);
         uint64_t active_key_id = finalizer_info["active_key_id"].as_uint64();
         auto itr = std::find_if(last_finalizers.begin(), last_finalizers.end(), [&active_key_id](const finalizer_auth_info& f) { return f.key_id == active_key_id; });
         // finalizer's active key id is in last proposed finalizers table
         BOOST_REQUIRE_EQUAL( true, itr != last_finalizers.end() );
         // finalizer's active key matches one in last proposed finalizers table
         BOOST_REQUIRE_EQUAL( true, itr->fin_authority.public_key == finalizer_info["active_key_binary"].as<std::vector<char>>() );
      }
   }
};


const std::vector<sysio_test::key_pair_t>& sysio_test::get_finalizer_test_keys() {
   static const auto keys = [] {
      std::vector<key_pair_t> result;
      for (uint8_t seed = 1; seed <= 23; ++seed)
         result.emplace_back(seed);
      return result;
   }();
   return keys;
}

BOOST_AUTO_TEST_SUITE(sysio_system_finalizer_key_tests)

const name alice = "alice1111111"_n;
const name bob   = "bob111111111"_n;

BOOST_FIXTURE_TEST_CASE(register_finalizer_key_failure_tests, finalizer_key_tester) try {
   {  // bob111111111 does not have Alice's authority
      BOOST_REQUIRE_EQUAL( error( "missing authority of bob111111111" ),
                           push_action(alice, "regfinkey"_n, mvo()
                              ("finalizer_name",  "bob111111111")
                              ("finalizer_key", finalizer_key_1 )
                              ("proof_of_possession", fixture_key_1.proof(alice) )
                           ) );
   }

   { // attempt to register finalizer_key for an unregistered producer
      BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer alice1111111 is not a registered producer" ),
                           register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)));
   }

   // Now alice1111111 registers as a producer
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );


   {  // finalizer key does not start with PUB_BLS
      BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer key does not start with PUB_BLS: UB_BLS_6j4Y3LfsRiBxY-DgvqrZNMCttHftBQPIWwDiN2CMhHWULjN1nGwM1O_nEEJefqwAG4X09n4Kdt4a1mfZ1ES1cLGjQo6uLLSloiVW4i9BUhMHU2nVujP1_U_9ihdI3egZ17N-iA"),
                           push_action(alice, "regfinkey"_n, mvo()
                              ("finalizer_name",  "alice1111111")
                              ("finalizer_key", "UB_BLS_6j4Y3LfsRiBxY-DgvqrZNMCttHftBQPIWwDiN2CMhHWULjN1nGwM1O_nEEJefqwAG4X09n4Kdt4a1mfZ1ES1cLGjQo6uLLSloiVW4i9BUhMHU2nVujP1_U_9ihdI3egZ17N-iA" )
                              ("proof_of_possession", fixture_key_1.proof(alice) )
                           ) );
   }

   {  // proof_of_possession does not start with SIG_BLS
      BOOST_REQUIRE_EQUAL( wasm_assert_msg( "invalid finalizer registration proof format" ),
                           push_action(alice, "regfinkey"_n, mvo()
                              ("finalizer_name",  "alice1111111")
                              ("finalizer_key", finalizer_key_1)
                              ("proof_of_possession", "XIG_BLS_N5r73_i50OVkydasCVVBOqqAqM4XQo_-DHgNawK77bcf06Bx0_rh5TNn9iZewNMZ6ecyEjs_sEkwjAXplhqyqf7S9FqSt8mfRxO7pE3bUZS0Z-Fxitsh9X0l_-kj3Z8VD8IwsaUwBLacudzShIXA-5E47cEqYoV3bGhANerKuDhZ4Pesm2xotAScK0pcNp0LbTNj0MZpVr0u6kJh169IoeG4ngCvD6uE2EicNrzyvDhu0u925Q1cm5z_bVha-DsANq3zcA")
                           ) );
   }

   {  // proof_of_possession fails
      BOOST_REQUIRE_EQUAL( wasm_assert_msg( "proof of possession check failed" ),
                           push_action(alice, "regfinkey"_n, mvo()
                              ("finalizer_name",  "alice1111111")
                              // use a valid formatted finalizer_key for another signature
                              ("finalizer_key", finalizer_key_1)
                              ("proof_of_possession", fixture_key_2.proof(alice))
                           ) );
   }
} // register_finalizer_key_invalid_key_tests
FC_LOG_AND_RETHROW()

/// C++, TypeScript, and the contract agree on the fixed registration bytes and both signatures.
BOOST_FIXTURE_TEST_CASE(register_finalizer_key_cross_language_vector, finalizer_key_tester) try {
   const fc::crypto::bls::private_key key(registration_vector_private_key);
   const auto message = sysio::finalizer_registration::message(alice.to_uint64_t(), key.get_public_key().serialize());
   BOOST_REQUIRE_EQUAL(key.get_public_key().to_string(), registration_vector_public_key);
   BOOST_REQUIRE_EQUAL(fc::to_hex(message.data(), message.size()), registration_vector_message_hex);
   BOOST_REQUIRE_EQUAL(make_finalizer_registration_proof(alice.to_uint64_t(), key), registration_vector_proof);
   BOOST_REQUIRE_EQUAL(success(), regproducer(alice));
   BOOST_REQUIRE_EQUAL(success(), register_finalizer_key(alice, registration_vector_public_key, registration_vector_proof));
} FC_LOG_AND_RETHROW()

/// A copied pending proof cannot reserve another producer's key, even before its owner registers.
BOOST_FIXTURE_TEST_CASE(register_finalizer_key_account_binding, finalizer_key_tester) try {
   BOOST_REQUIRE_EQUAL(success(), regproducer(alice));
   BOOST_REQUIRE_EQUAL(success(), regproducer(bob));
   const auto proof = fixture_key_1.proof(alice);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("finalizer registration signature check failed"),
                       register_finalizer_key(bob, finalizer_key_1, proof));
   BOOST_REQUIRE(get_finalizer_info(bob).is_null());
   BOOST_REQUIRE_EQUAL(success(), register_finalizer_key(alice, finalizer_key_1, proof));
   BOOST_REQUIRE_EQUAL(success(), delete_finalizer_key(alice, finalizer_key_1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("finalizer registration signature check failed"),
                       register_finalizer_key(bob, finalizer_key_1, proof));
   // The owner can reuse its proof after deletion. A different owner needs a fresh signature.
   BOOST_REQUIRE_EQUAL(success(), register_finalizer_key(alice, finalizer_key_1, proof));
   BOOST_REQUIRE_EQUAL(success(), delete_finalizer_key(alice, finalizer_key_1));
   BOOST_REQUIRE_EQUAL(success(), register_finalizer_key(bob, finalizer_key_1, fixture_key_1.proof(bob)));
} FC_LOG_AND_RETHROW()

/// Both cryptographic halves and the exact versioned framing are mandatory.
BOOST_FIXTURE_TEST_CASE(register_finalizer_key_proof_validation, finalizer_key_tester) try {
   BOOST_REQUIRE_EQUAL(success(), regproducer(alice));
   namespace registration = sysio::finalizer_registration;
   const auto proof = fixture_key_1.proof(alice);
   const auto other = fixture_key_2.proof(alice);
   const auto signature_offset = registration::proof_prefix.size() + registration::signature_text_size + 1;
   auto bad_pop = proof;
   bad_pop.replace(registration::proof_prefix.size(), registration::signature_text_size,
                   other.substr(registration::proof_prefix.size(), registration::signature_text_size));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("proof of possession check failed"),
                       register_finalizer_key(alice, finalizer_key_1, bad_pop));
   // A valid ordinary signature must never substitute for the standard PoP.
   bad_pop.replace(registration::proof_prefix.size(), registration::signature_text_size,
                   proof.substr(signature_offset));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("proof of possession check failed"),
                       register_finalizer_key(alice, finalizer_key_1, bad_pop));
   const auto bad_signature = proof.substr(0, signature_offset) + other.substr(signature_offset);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("finalizer registration signature check failed"),
                       register_finalizer_key(alice, finalizer_key_1, bad_signature));
   auto wrong_version = proof;
   wrong_version[9] = '2';
   for (const auto& malformed : {std::string{}, fixture_key_1.private_key.proof_of_possession().to_string(),
                                proof.substr(1), proof + ":", wrong_version}) {
      BOOST_REQUIRE_EQUAL(wasm_assert_msg("invalid finalizer registration proof format"),
                          register_finalizer_key(alice, finalizer_key_1, malformed));
   }
   auto bad_checksum = proof;
   bad_checksum[registration::proof_prefix.size() + registration::signature_prefix.size()] = '!';
   BOOST_REQUIRE_NE(success(), register_finalizer_key(alice, finalizer_key_1, bad_checksum));
   BOOST_REQUIRE(get_finalizer_info(alice).is_null());
   BOOST_REQUIRE_EQUAL(success(), register_finalizer_key(alice, finalizer_key_1, proof));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(register_finalizer_key_by_same_finalizer_tests, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );

   // Register first finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );

   // Make sure binary format is correct, which is important
   auto alice_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( "alice1111111", alice_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( 1, alice_info["finalizer_key_count"].as_uint64() );
   BOOST_REQUIRE_EQUAL( finalizer_key_binary_1, alice_info["active_key_binary"].as_string() );

   // Cross check finalizer keys table
   uint64_t active_key_id = alice_info["active_key_id"].as_uint64();
   auto fin_key_info = get_finalizer_key_info(active_key_id);
   BOOST_REQUIRE_EQUAL( "alice1111111", fin_key_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( finalizer_key_1, fin_key_info["finalizer_key"].as_string() );

   // Register second finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_2, fixture_key_2.proof(alice)));

   alice_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( 2, alice_info["finalizer_key_count"].as_uint64() ); // count incremented by 1
   BOOST_REQUIRE_EQUAL( active_key_id, alice_info["active_key_id"].as_uint64() ); // active key should not change
}
FC_LOG_AND_RETHROW() // register_finalizer_key_by_same_finalizer_tests

BOOST_FIXTURE_TEST_CASE(register_finalizer_key_bills_system_ram, finalizer_key_tester) try {
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );

   auto& resource_limits = control->get_mutable_resource_limits_manager();
   const int64_t alice_before = resource_limits.get_account_ram_usage(alice);
   const int64_t sysio_before = resource_limits.get_account_ram_usage(config::system_account_name);
   resource_limits.set_account_limits(alice, alice_before, -1, -1, false);

   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );

   BOOST_REQUIRE_EQUAL(alice_before, resource_limits.get_account_ram_usage(alice));
   BOOST_REQUIRE_GT(resource_limits.get_account_ram_usage(config::system_account_name), sysio_before);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(register_finalizer_key_caps_retained_keys, finalizer_key_tester) try {
   constexpr uint32_t max_retained_finalizer_keys = 5;
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );
   BOOST_REQUIRE_GE(key_pairs.size(), max_retained_finalizer_keys + 1);

   for (uint32_t i = 0; i < max_retained_finalizer_keys; ++i) {
      BOOST_REQUIRE_EQUAL(
         success(), register_finalizer_key(alice, key_pairs[i].pub_key, key_pairs[i].proof(alice)));
   }
   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("finalizer cannot register more than 5 keys"),
      register_finalizer_key(
         alice, key_pairs[max_retained_finalizer_keys].pub_key,
         key_pairs[max_retained_finalizer_keys].proof(alice)));

   // Rotation needs only two keys; deleting an inactive key immediately frees one bounded slot.
   BOOST_REQUIRE_EQUAL(
      success(), delete_finalizer_key(
         alice, key_pairs[max_retained_finalizer_keys - 1].pub_key));
   BOOST_REQUIRE_EQUAL(
      success(), register_finalizer_key(
         alice, key_pairs[max_retained_finalizer_keys].pub_key,
         key_pairs[max_retained_finalizer_keys].proof(alice)));
   BOOST_REQUIRE_EQUAL(
      max_retained_finalizer_keys,
      get_finalizer_info(alice)["finalizer_key_count"].as_uint64());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(register_finalizer_key_duplicate_key_tests, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );

   // The first finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );

   auto alice_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( "alice1111111", alice_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( 1, alice_info["finalizer_key_count"].as_uint64() );

   // Tries to register the same finalizer key
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "duplicate finalizer key: " + finalizer_key_1 ),
                        register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );

   // finalizer key count still 1
   alice_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( 1, alice_info["finalizer_key_count"].as_uint64() );
}
FC_LOG_AND_RETHROW() // register_finalizer_key_duplicate_key_tests

BOOST_FIXTURE_TEST_CASE(register_finalizer_key_by_different_finalizers_tests, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   add_roa_policy(NODE_DADDY, bob, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   // register 2 producers
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );
   BOOST_REQUIRE_EQUAL( success(), regproducer(bob) );

   // alice1111111 registers a finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_2, fixture_key_2.proof(alice)) );

   auto alice_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( "alice1111111", alice_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( finalizer_key_binary_1, alice_info["active_key_binary"].as_string() );
   BOOST_REQUIRE_EQUAL( 2, alice_info["finalizer_key_count"].as_uint64() );

   // bob111111111 registers another finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_3, fixture_key_3.proof(bob)) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_4, fixture_key_4.proof(bob)) );

   auto bob_info = get_finalizer_info(bob);
   BOOST_REQUIRE_EQUAL( 2, bob_info["finalizer_key_count"].as_uint64() );
   BOOST_REQUIRE_EQUAL( finalizer_key_binary_3, bob_info["active_key_binary"].as_string() );
}
FC_LOG_AND_RETHROW() // register_finalizer_key_by_different_finalizers_tests


BOOST_FIXTURE_TEST_CASE(register_duplicate_key_from_different_finalizers_tests, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   add_roa_policy(NODE_DADDY, bob, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );
   BOOST_REQUIRE_EQUAL( success(), regproducer(bob) );

   // The first finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );

   auto fin_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( "alice1111111", fin_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( 1, fin_info["finalizer_key_count"].as_uint64() );

   // bob111111111 tries to register the same finalizer key as the first one
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "duplicate finalizer key: " + finalizer_key_1 ),
                        register_finalizer_key(bob, finalizer_key_1, fixture_key_1.proof(bob)) );
}
FC_LOG_AND_RETHROW() // register_duplicate_key_from_different_finalizers_tests

BOOST_FIXTURE_TEST_CASE(activate_finalizer_key_failure_tests, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   add_roa_policy(NODE_DADDY, bob, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   // bob111111111 does not have Alice's authority
   BOOST_REQUIRE_EQUAL( error( "missing authority of bob111111111" ),
                        push_action(alice, "actfinkey"_n, mvo()
                           ("finalizer_name", "bob111111111")
                           ("finalizer_key",  finalizer_key_1 )
                        ) );

   // Register producers
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );
   BOOST_REQUIRE_EQUAL( success(), regproducer(bob) );

   // finalizer has not registered any finalizer keys yet.
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer alice1111111 has not registered any finalizer keys" ),
                        activate_finalizer_key(alice, finalizer_key_1) );

   // Alice registers a finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_2, fixture_key_2.proof(bob)) );

   // Activate a finalizer key not registered by anyone
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer key was not registered: " + finalizer_key_3 ),
                        activate_finalizer_key(alice, finalizer_key_3) );

   // Activate a finalizer key not registered by Alice
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer key was not registered by the finalizer: " + finalizer_key_2 ),
                        activate_finalizer_key(alice, finalizer_key_2) );

   // Activate a finalizer key that is already active (the first key registered is
   // automatically set to active
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer key was already active: " + finalizer_key_1 ),
                        activate_finalizer_key(alice, finalizer_key_1) );
}
FC_LOG_AND_RETHROW() // activate_finalizer_key_failure_tests

BOOST_FIXTURE_TEST_CASE(activate_finalizer_key_success_tests, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   // Alice registers as a producer
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );

   // Alice registers two finalizer keys. The first key is active
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_2, fixture_key_2.proof(alice)) );

   // Check finalizer_key_1 is the active key
   auto alice_info = get_finalizer_info(alice);
   uint64_t active_key_id = alice_info["active_key_id"].as_uint64();
   auto finalizer_key_info = get_finalizer_key_info(active_key_id);
   BOOST_REQUIRE_EQUAL( "alice1111111", finalizer_key_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( finalizer_key_1, finalizer_key_info["finalizer_key"].as_string() );

   // Activate the second key
   BOOST_REQUIRE_EQUAL( success(), activate_finalizer_key(alice, finalizer_key_2) );

   // Check finalizer_key_2 is the active key
   alice_info = get_finalizer_info(alice);
   active_key_id = alice_info["active_key_id"].as_uint64();
   finalizer_key_info = get_finalizer_key_info(active_key_id);
   BOOST_REQUIRE_EQUAL( "alice1111111", finalizer_key_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( finalizer_key_2, finalizer_key_info["finalizer_key"].as_string() );

   // Make sure active_key_binary is correct. This test is important.
   BOOST_REQUIRE_EQUAL( finalizer_key_binary_2, alice_info["active_key_binary"].as_string() );
}
FC_LOG_AND_RETHROW() // activate_finalizer_key_success_tests

BOOST_FIXTURE_TEST_CASE(delete_finalizer_key_failure_tests, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   add_roa_policy(NODE_DADDY, bob, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   // bob111111111 does not have Alice's authority
   BOOST_REQUIRE_EQUAL( error( "missing authority of bob111111111" ),
                        push_action(alice, "delfinkey"_n, mvo()
                           ("finalizer_name", "bob111111111")
                           ("finalizer_key",  finalizer_key_1 )
                        ) );

   // Register producers
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );
   BOOST_REQUIRE_EQUAL( success(), regproducer(bob) );

   // finalizer has not registered any finalizer keys yet.
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer alice1111111 has not registered any finalizer keys" ),
                        delete_finalizer_key(alice, finalizer_key_1) );

   // Alice and Bob register finalizer keys
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_2, fixture_key_2.proof(bob)) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_3, fixture_key_3.proof(bob)) );

   // Alice tries to delete  a finalizer key not registered by anyone
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer key was not registered: " + finalizer_key_4 ),
                        delete_finalizer_key(alice, finalizer_key_4) );

   // Alice tries to delete a finalizer key registered by Bob
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer key " + finalizer_key_2 + " was not registered by the finalizer alice1111111" ),
                        delete_finalizer_key(alice, finalizer_key_2) );

   // Make sure finalizer_key_2 is Bob's active finalizer key and Bob has 2 keys
   auto bob_info = get_finalizer_info(bob);
   uint64_t active_key_id = bob_info["active_key_id"].as_uint64();
   auto finalizer_key_info = get_finalizer_key_info(active_key_id);
   BOOST_REQUIRE_EQUAL( finalizer_key_2, finalizer_key_info["finalizer_key"].as_string() );
   BOOST_REQUIRE_EQUAL( 2, bob_info["finalizer_key_count"].as_uint64() );

   // Bob tries to delete his active finalizer key but he has 2 keys
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "cannot delete an active key unless it is the last registered finalizer key, has 2 keys" ),
                        delete_finalizer_key(bob, finalizer_key_2) );

}
FC_LOG_AND_RETHROW() // delete_finalizer_key_failure_tests

BOOST_FIXTURE_TEST_CASE(delete_finalizer_key_success_test, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   // Alice registers as a producer
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );

   // Alice registers two keys and the first key is active 
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_2, fixture_key_2.proof(alice)) );

   // Check finalizer_key_1 is the active key
   auto alice_info = get_finalizer_info(alice);
   uint64_t active_key_id = alice_info["active_key_id"].as_uint64();
   auto finalizer_key_count_before = alice_info["finalizer_key_count"].as_uint64();
   auto finalizer_key_info = get_finalizer_key_info(active_key_id);
   BOOST_REQUIRE_EQUAL( "alice1111111", finalizer_key_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( finalizer_key_1, finalizer_key_info["finalizer_key"].as_string() );

   // Delete the non-active key
   BOOST_REQUIRE_EQUAL( success(), delete_finalizer_key(alice, finalizer_key_2) );

   alice_info = get_finalizer_info(alice);
   auto finalizer_key_count_after = alice_info["finalizer_key_count"].as_uint64();
   BOOST_REQUIRE_EQUAL( finalizer_key_count_before - 1, finalizer_key_count_after );
}
FC_LOG_AND_RETHROW() // delete_finalizer_key_success_test

BOOST_FIXTURE_TEST_CASE(delete_last_finalizer_key_test, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   // Alice registers as a producer
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );

   // Alice registers one key and it is active
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, fixture_key_1.proof(alice)) );

   // Check finalizer_key_1 is the active key
   auto alice_info = get_finalizer_info(alice);
   uint64_t active_key_id = alice_info["active_key_id"].as_uint64();
   auto finalizer_key_info = get_finalizer_key_info(active_key_id);
   BOOST_REQUIRE_EQUAL( "alice1111111", finalizer_key_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( finalizer_key_1, finalizer_key_info["finalizer_key"].as_string() );

   // Delete it
   BOOST_REQUIRE_EQUAL( success(), delete_finalizer_key(alice, finalizer_key_1) );

   // Both finalizer_key_1 and alice should be removed from finalizers and finalizer_keys tables
   BOOST_REQUIRE_EQUAL( true, get_finalizer_key_info(active_key_id).is_null() );
   BOOST_REQUIRE_EQUAL( true, get_finalizer_info(alice).is_null() );
}
FC_LOG_AND_RETHROW() // delete_last_finalizer_key_test

// After registering keys and waiting for update_ranked_producers, test key activation
BOOST_FIXTURE_TEST_CASE(multi_activation_tests, finalizer_key_tester) try {
   auto producer_names = activate_producers_with_operators();
   // Register 21 finalizer keys for the first 21 producers
   register_finalizer_keys(producer_names, 21);

   // Wait for update_ranked_producers to run (triggers every ~60s in onblock)
   produce_block( fc::minutes(2) );

   // Verify finalizers_table and last_prop_fins_table match
   verify_last_proposed_finalizers(producer_names);

   // Register two more keys for defproducera
   account_name producera = "defproducera"_n;
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(producera, finalizer_key_1, fixture_key_1.proof(producera)) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(producera, finalizer_key_2, fixture_key_2.proof(producera)) );

   auto producera_info = get_finalizer_info(producera);
   auto active_key_id_before = producera_info["active_key_id"].as_uint64();
   auto last_prop_fin_ids_before = get_last_prop_fin_ids();
   BOOST_REQUIRE_EQUAL( 21, last_prop_fin_ids_before.size() );

   // Activate finalizer_key_1
   BOOST_REQUIRE_EQUAL( success(), activate_finalizer_key(producera, finalizer_key_1) );
   produce_block();

   // Make sure last proposed finalizers set has changed
   producera_info = get_finalizer_info(producera);
   auto active_key_id_after = producera_info["active_key_id"].as_uint64();
   auto last_prop_fin_ids_after = get_last_prop_fin_ids();
   BOOST_REQUIRE_EQUAL( 21, last_prop_fin_ids_after.size() );

   last_prop_fin_ids_before.erase(active_key_id_before);
   last_prop_fin_ids_before.insert(active_key_id_after);
   BOOST_REQUIRE_EQUAL( true, last_prop_fin_ids_before == last_prop_fin_ids_after );

   // Activate finalizer_key_2
   last_prop_fin_ids_before = last_prop_fin_ids_after;
   active_key_id_before = active_key_id_after;

   BOOST_REQUIRE_EQUAL( success(), activate_finalizer_key(producera, finalizer_key_2) );
   produce_block();

   // Make sure last proposed finalizers set has changed
   producera_info = get_finalizer_info(producera);
   active_key_id_after = producera_info["active_key_id"].as_uint64();
   last_prop_fin_ids_after = get_last_prop_fin_ids();
   BOOST_REQUIRE_EQUAL( 21, last_prop_fin_ids_after.size() );

   last_prop_fin_ids_before.erase(active_key_id_before);
   last_prop_fin_ids_before.insert(active_key_id_after);
   BOOST_REQUIRE_EQUAL( true, last_prop_fin_ids_before == last_prop_fin_ids_after );
}
FC_LOG_AND_RETHROW()

// Finalizers are not changed in current schedule rounds
BOOST_FIXTURE_TEST_CASE(update_ranked_producers_no_finalizers_changed_test, finalizer_key_tester) try {
   auto producer_names = activate_producers_with_operators();
   register_finalizer_keys(producer_names, 21);

   // Wait for update_ranked_producers to run
   produce_block( fc::minutes(2) );

   // Verify finalizers_table and last_prop_fins_table match
   verify_last_proposed_finalizers(producer_names);

   auto last_finkey_ids = get_last_prop_fin_ids();

   // Produce for another round
   produce_block( fc::minutes(2) );

   // Since finalizer keys have not changed, last_finkey_ids should be the same
   auto last_finkey_ids_2 = get_last_prop_fin_ids();
   BOOST_REQUIRE_EQUAL( 21, last_finkey_ids_2.size() );
   BOOST_REQUIRE_EQUAL( true, last_finkey_ids == last_finkey_ids_2 );
}
FC_LOG_AND_RETHROW()

// An active finalizer activates another key. The change takes effect immediately.
BOOST_FIXTURE_TEST_CASE(update_ranked_producers_finalizers_changed_test, finalizer_key_tester) try {
   auto producer_names = activate_producers_with_operators();
   register_finalizer_keys(producer_names, 21);

   // Wait for update_ranked_producers to populate lastpropfins
   produce_block( fc::minutes(2) );

   // Verify finalizers_table and last_prop_fins_table match
   verify_last_proposed_finalizers(producer_names);
   auto last_finkey_ids = get_last_prop_fin_ids();

   // Pick a producer
   name test_producer = producer_names.back();

   // Take a note of old active_key_id
   auto p_info = get_finalizer_info(test_producer);
   uint64_t old_id = p_info["active_key_id"].as_uint64();

   // Register and activate a new finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(test_producer, finalizer_key_1, fixture_key_1.proof(test_producer)) );
   BOOST_REQUIRE_EQUAL( success(), activate_finalizer_key(test_producer, finalizer_key_1));

   // Since producer is an active producer, the finalizer key change takes effect immediately.
   auto last_finkey_ids_2 = get_last_prop_fin_ids();
   BOOST_REQUIRE_EQUAL( 21, last_finkey_ids_2.size() );

   // Take a note of new active_key_id
   auto p_info_2 = get_finalizer_info(test_producer);
   uint64_t new_id = p_info_2["active_key_id"].as_uint64();

   // After replacing old_id with new_id in last_finkey_ids,
   // last_finkey_ids should be the same as last_finkey_ids_2
   last_finkey_ids.erase(old_id);
   last_finkey_ids.insert(new_id);
   BOOST_REQUIRE_EQUAL( true, last_finkey_ids == last_finkey_ids_2 );
}
FC_LOG_AND_RETHROW()

// An active finalizer deletes its only key. It is replaced by another finalizer in next round.
BOOST_FIXTURE_TEST_CASE(update_ranked_producers_finalizers_replaced_test, finalizer_key_tester) try {
   // Create 26 producers (first 21 in schedule, 5 standby)
   auto producer_names = activate_producers_with_operators(26);

   // Only the first 21 producers register finalizer keys at the beginning
   register_finalizer_keys(producer_names, 21);

   // Wait for update_ranked_producers to populate lastpropfins
   produce_block( fc::minutes(2) );

   auto last_finkey_ids = get_last_prop_fin_ids();
   BOOST_REQUIRE_EQUAL( 21, last_finkey_ids.size() );

   // Verify finalizers_table and last_prop_fins_table match
   std::vector<name> producer_names_first_21(producer_names.begin(), producer_names.begin() + 21);
   verify_last_proposed_finalizers(producer_names_first_21);

   // defproducerv registers its first finalizer key and is marked active
   account_name producerv_name = "defproducerv"_n;
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(producerv_name, finalizer_key_1, fixture_key_1.proof(producerv_name)) );
   auto producerv_info = get_finalizer_info(producerv_name);
   uint64_t producerv_id = producerv_info["active_key_id"].as_uint64();

   // Rank is POSITION in the score-ordered index -- governance no longer assigns it. Removing the
   // producer holding position 1 shifts every later producer up by one, which promotes
   // defproducerv from position 22 into the top 21.
   BOOST_REQUIRE_EQUAL( success(), push_action("defproducera"_n, "unregprod"_n,
      mvo()("producer", "defproducera"_n)) );

   // Wait for update_ranked_producers to pick up new ranking
   produce_block( fc::minutes(2) );

   // find new last_finkey_ids
   auto last_finkey_ids_2 = get_last_prop_fin_ids();
   BOOST_REQUIRE_EQUAL( 21, last_finkey_ids_2.size() );
   // Make sure producerv's key is now in the finalizer set
   BOOST_REQUIRE_EQUAL( true, last_finkey_ids_2.contains(producerv_id) );
}
FC_LOG_AND_RETHROW()

// Verify that update_ranked_producers correctly populates the controller's
// active producer schedule and proposes the correct finalizer policy.
//
// Note: In a single-node test, the 21-finalizer policy can only become
// "pending" at the controller level because the test node lacks the BLS
// keys needed for the 21 finalizers to vote and reach quorum. We therefore
// verify the producer schedule via head_active_producers() and the finalizer
// policy via the contract's lastpropfins table + head_pending_finalizer_policy().
BOOST_FIXTURE_TEST_CASE(verify_controller_schedule_and_policy_test, finalizer_key_tester) try {
   auto producer_names = activate_producers_with_operators();

   // Register 21 finalizer keys for the first 21 producers
   register_finalizer_keys(producer_names, 21);

   // Wait for update_ranked_producers to propose the new schedule and finalizer policy
   produce_block( fc::minutes(2) );

   // Produce enough blocks for the new producer schedule to become active
   produce_blocks(504);

   // --- Verify active producer schedule via controller ---
   const auto& active_schedule = control->head_active_producers();
   BOOST_REQUIRE_EQUAL( 21u, active_schedule.producers.size() );

   // Build a sorted set of expected producer names
   std::set<account_name> expected_producers(producer_names.begin(), producer_names.end());
   std::set<account_name> actual_producers;
   for( const auto& p : active_schedule.producers ) {
      actual_producers.insert(p.producer_name);
   }
   BOOST_REQUIRE_EQUAL( true, expected_producers == actual_producers );

   // Verify schedule is sorted by producer name (deterministic ordering)
   for( size_t i = 1; i < active_schedule.producers.size(); ++i ) {
      BOOST_REQUIRE( active_schedule.producers[i-1].producer_name < active_schedule.producers[i].producer_name );
   }

   // --- Verify finalizer policy was proposed (contract-level) ---
   // The lastpropfins table should contain 21 finalizers
   auto last_proposed = get_last_prop_finalizers_info();
   BOOST_REQUIRE_EQUAL( 21u, last_proposed.size() );

   // Verify the contract table entries match the finalizers table
   verify_last_proposed_finalizers(producer_names);

   // --- Verify finalizer policy at controller level ---
   // The active finalizer policy is still the genesis policy (1 finalizer)
   // because the 21-key pending policy requires quorum from 21 BLS keys
   // which aren't loaded in the single-node test environment.
   auto active_fin_policy = control->head_active_finalizer_policy();
   BOOST_REQUIRE( active_fin_policy != nullptr );

   // The pending finalizer policy should contain our 21 finalizers.
   // It was proposed by set_finalizers() and became pending when the proposing
   // block reached finality under the genesis (1-finalizer) active policy.
   auto pending_fin_policy = control->head_pending_finalizer_policy();
   BOOST_REQUIRE( pending_fin_policy != nullptr );
   BOOST_REQUIRE_EQUAL( 21u, pending_fin_policy->finalizers.size() );

   // Threshold should be 2/3 + 1 of total weight (each finalizer has weight 1)
   BOOST_REQUIRE_EQUAL( (21u * 2) / 3 + 1, pending_fin_policy->threshold );

   // Each finalizer's description should be a producer name, with weight 1
   std::set<std::string> finalizer_descriptions;
   for( const auto& f : pending_fin_policy->finalizers ) {
      BOOST_REQUIRE_EQUAL( 1u, f.weight );
      finalizer_descriptions.insert(f.description);
   }
   for( const auto& p : producer_names ) {
      BOOST_REQUIRE_EQUAL( true, finalizer_descriptions.contains(p.to_string()) );
   }
}
FC_LOG_AND_RETHROW()


// A finalizer key that no proof of possession can screen out: the pairing bls_pop_verify computes
// is e(-g1, sig) * e(pk, H(pk)), and both terms are 1 when the points are the identity.
BOOST_FIXTURE_TEST_CASE(reject_identity_finalizer_key, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );

   const std::string identity_key = fc::crypto::bls::public_key::to_string(fc::crypto::bls::public_key_data{});
   const auto identity_signature = fc::crypto::bls::signature::to_string(fc::crypto::bls::signature_data{});
   const std::string identity_pop = std::string(sysio::finalizer_registration::proof_prefix) +
                                    identity_signature + ":" + identity_signature;

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("finalizer key must not be the identity point"),
                        register_finalizer_key(alice, identity_key, identity_pop) );

   // Bytes with a valid encoding that are not a point on the curve. set_finalizers raises while
   // deserializing one, before the policy is even validated.
   fc::crypto::bls::public_key_data off_curve;
   off_curve.fill(0xff);
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("finalizer key is not a valid G1 point"),
                        register_finalizer_key(alice, fc::crypto::bls::public_key::to_string(off_curve), identity_pop) );

   // Affine (0, 2) is canonical, on the curve, and not the identity, but its order is 3. That is
   // coprime to r, so it pairs to one against any G2 point and the proof of possession above
   // accepts it -- only a subgroup test rejects it.
   fc::crypto::bls::public_key_data small_order{};
   small_order[48] = 2;
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("finalizer key is not in the r-order subgroup"),
                        register_finalizer_key(alice, fc::crypto::bls::public_key::to_string(small_order),
                                               identity_pop) );

   // An honest key is unaffected.
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, key_pairs[0].pub_key, key_pairs[0].proof(alice)) );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
