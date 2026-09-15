#include "sysio.system_tester.hpp"
#include "finalizer_test_keys.hpp"

#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/opp/opp.hpp>
#include <boost/test/unit_test.hpp>

using namespace sysio_system;
using namespace sysio_test;

struct finalizer_key_tester : sysio_system_tester {

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
         BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(p, key_pairs[i].pub_key, key_pairs[i].pop));
         ++i;

         if ( i  == num_keys_to_register ) {
            break;
         }
      }
   }

   // sysio.system now schedules a producer only if it is an ACTIVE
   // OPERATOR_TYPE_PRODUCER operator in sysio.opreg. activate_producers() alone no
   // longer yields a schedulable set, so this deploys sysio.opreg (once) and
   // registers each activated producer as a bootstrapped producer operator --
   // ACTIVE-by-fiat, bypassing the collateral requirement -- before returning.
   std::vector<name> activate_producers_with_operators( uint32_t count = 21 ) {
      std::vector<name> producer_names = activate_producers(count);
      if (!opreg_deployed) {
         create_account("sysio.opreg"_n, config::system_account_name, false, false, false, true);
         // opreg is not privileged yet (setpriv requires setcode first). Give it
         // RAM for the ~800KB wasm and NET/CPU to sign regoperator; a sysio.*
         // account is created with none by default.
         push_action(config::system_account_name, "setacctram"_n, mvo()
            ("account", "sysio.opreg"_n)("ram_bytes", int64_t(2'000'000)));
         push_action(config::system_account_name, "setacctnet"_n, mvo()
            ("account", "sysio.opreg"_n)("net_weight", int64_t(1'000'000)));
         push_action(config::system_account_name, "setacctcpu"_n, mvo()
            ("account", "sysio.opreg"_n)("cpu_weight", int64_t(1'000'000)));
         produce_block();
         set_code("sysio.opreg"_n, contracts::opreg_wasm());
         set_abi ("sysio.opreg"_n, contracts::opreg_abi().data());
         set_privileged("sysio.opreg"_n);
         produce_block();
         opreg_deployed = true;
      }
      for (const auto& p : producer_names) {
         base_tester::push_action("sysio.opreg"_n, "regoperator"_n, "sysio.opreg"_n, mvo()
            ("account", p)
            ("type", sysio::opp::types::OperatorType::OPERATOR_TYPE_PRODUCER)
            ("is_bootstrapped", true));
      }
      produce_block();
      return producer_names;
   }
   bool opreg_deployed = false;

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


BOOST_AUTO_TEST_SUITE(sysio_system_finalizer_key_tests)

const name alice = "alice1111111"_n;
const name bob   = "bob111111111"_n;

const std::string finalizer_key_1 = "PUB_BLS_6j4Y3LfsRiBxY-DgvqrZNMCttHftBQPIWwDiN2CMhHWULjN1nGwM1O_nEEJefqwAG4X09n4Kdt4a1mfZ1ES1cLGjQo6uLLSloiVW4i9BUhMHU2nVujP1_U_9ihdI3egZ17N-iA";
const std::string finalizer_key_2 = "PUB_BLS_gtaOjOTa0NzDt8etBDqLoZKlfKTpTalcdfmbTJknLUJB2Fu4Cv-uoa8unF3bJ5kFewaCzf3tjYUyNE6CDSrwvYP5Nw47Y9oE9x4nqJWfJykMOoaI0kJz-GDrGN2nZdUAp5tWEg";
const std::string finalizer_key_3 = "PUB_BLS_CT8khvZYYZdObeIV9aTnd8fZ8bdaCL1UpSRyqNLZZM5sdrOSpOjDTAY2drTYGvQPVS21BhtD8acLJhqGyTfjqrWjyY5FTfqLdcligofSpa2lrG3FqKVNeUULR5QgcIMYga4vkQ";
const std::string finalizer_key_4 = "PUB_BLS_hJYC9REVk4Pzgt3NMycIaCRpRqTX8IIEAB8xhWg6pOYsV7n9gJnTTUOzPGH8VE4FPhxvzJuvrb5TNeR5MHwIjPMMKVPYHI-dDFwl5Oqj0yH9uoKRcMqjEaFZ5VYX3zMJuA1jQQ";

const std::string pop_1 = "SIG_BLS_N5r73_i50OVkydasCVVBOqqAqM4XQo_-DHgNawK77bcf06Bx0_rh5TNn9iZewNMZ6ecyEjs_sEkwjAXplhqyqf7S9FqSt8mfRxO7pE3bUZS0Z-Fxitsh9X0l_-kj3Z8VD8IwsaUwBLacudzShIXA-5E47cEqYoV3bGhANerKuDhZ4Pesm2xotAScK0pcNp0LbTNj0MZpVr0u6kJh169IoeG4ngCvD6uE2EicNrzyvDhu0u925Q1cm5z_bVha-DsANq3zcA";
const std::string pop_2 = "SIG_BLS_9e1SzM60bWLdxwz4lYNQMNzMGeFuzFgJDYy7WykmynRVRQeIx2O2xnyzwv1WXvgYHLyMYZ4wK0Y_kU6jl330WazkBsw-_GzvIOGy8fnBnt5AyMaj9X5bhDbvB5MZc0QQz4-P2Z4SltTY17ZItGeekkjX_fgQ9kegM4qnuGU-2iqFj5i3Qf322L77b2SHjFoLmxdFOsfGpz7LyImSP8GcZH39W30cj5bmxfsp_90tGdAkz-7DG9nhSHYxFq6qTqMGijVPGg";
const std::string pop_3 = "SIG_BLS_cJTQMGv1isqpcHEfhogLhlU56bKpGgo-Svi3Z4NXvWcly5TJo8hDChodIV-aEHgMqr06LuZftR7WFvgGkbOSdmwdO4t58R3RYOMSK-jjif2z-fEwCl7jsxUutASIRwIYtTI7h6NLCjARiKNi5BkES33xY8wYMWf-JkgbpsD2cGsZW4hkMW7T2j_1w89HmNwCn4V_hjlPM_kgz45RoYpKq4w2QEaLdCYTJ6xYOfc9Occc15c76dd1jjty_yT2RMAKO0mfUA";
const std::string pop_4 = "SIG_BLS_GNlwGxjL-LCVDApTHernv8Hj6EqlsxWlZBUzOu6DcJmNNuHsfetXK14RPJ-L63wVnhPRL9aNrQAURy2wYJ1__rNiGk-nUMZ5RDTO7tO2EPTiyySq9cbzgn43vKG8FgsA4gbNlqVFeTCbo5CgGj8m9vXNV4-Cv68WW-ivcwJzDtnNA3O9PPIpRY6_HhbmwTUVrHL2v7X_arNCyAf29nucAYNOsCM-br6F6HwpSjqSxi4-KqcFfQCWbAbn_SgJNVAA4yx5fQ";

const std::string finalizer_key_binary_1 = "ea3e18dcb7ec46207163e0e0beaad934c0adb477ed0503c85b00e237608c8475942e33759c6c0cd4efe710425e7eac001b85f4f67e0a76de1ad667d9d444b570b1a3428eae2cb4a5a22556e22f415213075369d5ba33f5fd4ffd8a1748dde819";
const std::string finalizer_key_binary_2 = "82d68e8ce4dad0dcc3b7c7ad043a8ba192a57ca4e94da95c75f99b4c99272d4241d85bb80affaea1af2e9c5ddb2799057b0682cdfded8d8532344e820d2af0bd83f9370e3b63da04f71e27a8959f27290c3a8688d24273f860eb18dda765d500";
const std::string finalizer_key_binary_3 = "093f2486f65861974e6de215f5a4e777c7d9f1b75a08bd54a52472a8d2d964ce6c76b392a4e8c34c063676b4d81af40f552db5061b43f1a70b261a86c937e3aab5a3c98e454dfa8b75c9628287d2a5ada5ac6dc5a8a54d79450b479420708318";
const std::string finalizer_key_binary_4 = "849602f511159383f382ddcd33270868246946a4d7f08204001f3185683aa4e62c57b9fd8099d34d43b33c61fc544e053e1c6fcc9bafadbe5335e479307c088cf30c2953d81c8f9d0c5c25e4eaa3d321fdba829170caa311a159e55617df3309";

BOOST_FIXTURE_TEST_CASE(register_finalizer_key_failure_tests, finalizer_key_tester) try {
   {  // bob111111111 does not have Alice's authority
      BOOST_REQUIRE_EQUAL( error( "missing authority of bob111111111" ),
                           push_action(alice, "regfinkey"_n, mvo()
                              ("finalizer_name",  "bob111111111")
                              ("finalizer_key", finalizer_key_1 )
                              ("proof_of_possession", pop_1 )
                           ) );
   }

   { // attempt to register finalizer_key for an unregistered producer
      BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer alice1111111 is not a registered producer" ),
                           register_finalizer_key(alice, finalizer_key_1, pop_1));
   }

   // Now alice1111111 registers as a producer
   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );


   {  // finalizer key does not start with PUB_BLS
      BOOST_REQUIRE_EQUAL( wasm_assert_msg( "finalizer key does not start with PUB_BLS: UB_BLS_6j4Y3LfsRiBxY-DgvqrZNMCttHftBQPIWwDiN2CMhHWULjN1nGwM1O_nEEJefqwAG4X09n4Kdt4a1mfZ1ES1cLGjQo6uLLSloiVW4i9BUhMHU2nVujP1_U_9ihdI3egZ17N-iA"),
                           push_action(alice, "regfinkey"_n, mvo()
                              ("finalizer_name",  "alice1111111")
                              ("finalizer_key", "UB_BLS_6j4Y3LfsRiBxY-DgvqrZNMCttHftBQPIWwDiN2CMhHWULjN1nGwM1O_nEEJefqwAG4X09n4Kdt4a1mfZ1ES1cLGjQo6uLLSloiVW4i9BUhMHU2nVujP1_U_9ihdI3egZ17N-iA" )
                              ("proof_of_possession", pop_1 )
                           ) );
   }

   {  // proof_of_possession does not start with SIG_BLS
      BOOST_REQUIRE_EQUAL( wasm_assert_msg( "proof of possession signature does not start with SIG_BLS: XIG_BLS_N5r73_i50OVkydasCVVBOqqAqM4XQo_-DHgNawK77bcf06Bx0_rh5TNn9iZewNMZ6ecyEjs_sEkwjAXplhqyqf7S9FqSt8mfRxO7pE3bUZS0Z-Fxitsh9X0l_-kj3Z8VD8IwsaUwBLacudzShIXA-5E47cEqYoV3bGhANerKuDhZ4Pesm2xotAScK0pcNp0LbTNj0MZpVr0u6kJh169IoeG4ngCvD6uE2EicNrzyvDhu0u925Q1cm5z_bVha-DsANq3zcA" ),
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
                              ("proof_of_possession", pop_2)
                           ) );
   }
} // register_finalizer_key_invalid_key_tests
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(register_finalizer_key_by_same_finalizer_tests, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );

   // Register first finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, pop_1) );

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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_2, pop_2 ));

   alice_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( 2, alice_info["finalizer_key_count"].as_uint64() ); // count incremented by 1
   BOOST_REQUIRE_EQUAL( active_key_id, alice_info["active_key_id"].as_uint64() ); // active key should not change
}
FC_LOG_AND_RETHROW() // register_finalizer_key_by_same_finalizer_tests

BOOST_FIXTURE_TEST_CASE(register_finalizer_key_duplicate_key_tests, finalizer_key_tester) try {
   add_roa_policy(NODE_DADDY, alice, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);

   BOOST_REQUIRE_EQUAL( success(), regproducer(alice) );

   // The first finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, pop_1) );

   auto alice_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( "alice1111111", alice_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( 1, alice_info["finalizer_key_count"].as_uint64() );

   // Tries to register the same finalizer key
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "duplicate finalizer key: " + finalizer_key_1 ),
                        register_finalizer_key(alice, finalizer_key_1, pop_1) );

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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, pop_1) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_2, pop_2) );

   auto alice_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( "alice1111111", alice_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( finalizer_key_binary_1, alice_info["active_key_binary"].as_string() );
   BOOST_REQUIRE_EQUAL( 2, alice_info["finalizer_key_count"].as_uint64() );

   // bob111111111 registers another finalizer key
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_3, pop_3) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_4, pop_4) );

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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, pop_1) );

   auto fin_info = get_finalizer_info(alice);
   BOOST_REQUIRE_EQUAL( "alice1111111", fin_info["finalizer_name"].as_string() );
   BOOST_REQUIRE_EQUAL( 1, fin_info["finalizer_key_count"].as_uint64() );

   // bob111111111 tries to register the same finalizer key as the first one
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "duplicate finalizer key: " + finalizer_key_1 ),
                        register_finalizer_key(bob, finalizer_key_1, pop_1) );
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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, pop_1) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_2, pop_2) );

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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, pop_1) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_2, pop_2) );

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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, pop_1) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_2, pop_2) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(bob, finalizer_key_3, pop_3) );

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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, pop_1) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_2, pop_2) );

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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(alice, finalizer_key_1, pop_1) );

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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(producera, finalizer_key_1, pop_1) );
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(producera, finalizer_key_2, pop_2) );

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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(test_producer, finalizer_key_1, pop_1) );
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
   BOOST_REQUIRE_EQUAL( success(), register_finalizer_key(producerv_name, finalizer_key_1, pop_1) );
   auto producerv_info = get_finalizer_info(producerv_name);
   uint64_t producerv_id = producerv_info["active_key_id"].as_uint64();

   // Use setrank to promote defproducerv into top 21
   // and demote defproducera out
   BOOST_REQUIRE_EQUAL( success(), setrank("defproducerv"_n, 1) );
   BOOST_REQUIRE_EQUAL( success(), setrank("defproducera"_n, 22) );

   // Wait for update_ranked_producers to pick up new ranking
   produce_block( fc::minutes(2) );

   // find new last_finkey_ids
   auto last_finkey_ids_2 = get_last_prop_fin_ids();
   BOOST_REQUIRE_EQUAL( 21, last_finkey_ids_2.size() );
   // Make sure producerv's key is now in the finalizer set
   BOOST_REQUIRE_EQUAL( true, last_finkey_ids_2.contains(producerv_id) );
}
FC_LOG_AND_RETHROW()

// Test that setrank correctly assigns individual producer rank
BOOST_FIXTURE_TEST_CASE(setrank_test, finalizer_key_tester) try {
   auto producer_names = activate_producers();

   // Check initial ranks are assigned (1..21)
   auto prod_info = get_producer_info("defproducera");
   BOOST_REQUIRE_EQUAL( 1, prod_info["rank"].as<uint32_t>() );

   prod_info = get_producer_info("defproduceru");
   BOOST_REQUIRE_EQUAL( 21, prod_info["rank"].as<uint32_t>() );

   // setrank requires system authority
   BOOST_REQUIRE_EQUAL( error( "missing authority of sysio" ),
                        push_action( alice, "setrank"_n, mvo()
                           ("producer", "defproducera")
                           ("rank", 5)
                        ) );

   // setrank with rank=0 should fail
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "rank must be positive" ),
                        setrank("defproducera"_n, 0) );

   // setrank with nonexistent producer should fail
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "producer not found" ),
                        setrank("nonexistent1"_n, 1) );

   // Promote defproduceru to rank 1, demote defproducera to rank 22
   BOOST_REQUIRE_EQUAL( success(), setrank("defproduceru"_n, 1) );
   BOOST_REQUIRE_EQUAL( success(), setrank("defproducera"_n, 22) );

   prod_info = get_producer_info("defproduceru");
   BOOST_REQUIRE_EQUAL( 1, prod_info["rank"].as<uint32_t>() );

   prod_info = get_producer_info("defproducera");
   BOOST_REQUIRE_EQUAL( 22, prod_info["rank"].as<uint32_t>() );
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

BOOST_AUTO_TEST_SUITE_END()
