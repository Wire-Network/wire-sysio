#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include "fork_test_utilities.hpp"

using namespace sysio::testing;
using namespace sysio::chain;
using mvo = fc::mutable_variant_object;

BOOST_AUTO_TEST_SUITE(producer_schedule_tests)

BOOST_AUTO_TEST_CASE(verify_producers) try {
   savanna_tester chain;

   vector<account_name> valid_producers = {
      "inita"_n, "initb"_n, "initc"_n, "initd"_n, "inite"_n, "initf"_n, "initg"_n,
      "inith"_n, "initi"_n, "initj"_n, "initk"_n, "initl"_n, "initm"_n, "initn"_n,
      "inito"_n, "initp"_n, "initq"_n, "initr"_n, "inits"_n, "initt"_n, "initu"_n
   };
   chain.create_accounts(valid_producers);
   chain.set_producers(valid_producers);

   // account initz does not exist
   vector<account_name> nonexisting_producer = { "initz"_n };
   BOOST_CHECK_THROW(chain.set_producers(nonexisting_producer), wasm_execution_error);

   // replace initg with inita, inita is now duplicate
   vector<account_name> invalid_producers = {
      "inita"_n, "initb"_n, "initc"_n, "initd"_n, "inite"_n, "initf"_n, "inita"_n,
      "inith"_n, "initi"_n, "initj"_n, "initk"_n, "initl"_n, "initm"_n, "initn"_n,
      "inito"_n, "initp"_n, "initq"_n, "initr"_n, "inits"_n, "initt"_n, "initu"_n
   };

   BOOST_CHECK_THROW(chain.set_producers(invalid_producers), wasm_execution_error);

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( switch_producers_test ) try {
   validating_tester chain;

   const std::vector<account_name> accounts = { "aliceaccount"_n, "bobbyaccount"_n, "carolaccount"_n, "emilyaccount"_n };
   chain.create_accounts( accounts );
   chain.produce_block();

   chain.set_producers( accounts );
   chain.produce_block();

   // looping less than 20 did not reproduce the `producer_double_confirm: Producer is double confirming known range` error
   for (size_t i = 0; i < 20; ++i) {
      chain.set_producers( { "aliceaccount"_n, "bobbyaccount"_n } );
      chain.produce_block();

      chain.set_producers( { "bobbyaccount"_n, "aliceaccount"_n } );
      chain.produce_block();
      chain.produce_block( fc::hours(1) );

      chain.set_producers( accounts );
      chain.produce_block();
      chain.produce_block( fc::hours(1) );

      chain.set_producers( { "carolaccount"_n } );
      chain.produce_block();
      chain.produce_block( fc::hours(1) );
   }

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(producer_one_of_n_test) try {
   savanna_tester chain;

   chain.create_accounts( {"alice"_n,"bob"_n} );
   chain.produce_block();

   vector<producer_authority> sch1 = {
                                 producer_authority{"alice"_n, block_signing_authority_v0{1, {{get_public_key("alice"_n, "bs1"), 1}, {get_public_key("alice"_n, "bs2"), 1}}}},
                                 producer_authority{"bob"_n,   block_signing_authority_v0{1, {{get_public_key("bob"_n,   "bs1"), 1}, {get_public_key("bob"_n,   "bs2"), 1}}}}
                               };

   auto res = chain.set_producer_schedule( sch1 );
   chain.block_signing_private_keys.emplace(get_public_key("alice"_n, "bs1"), get_private_key("alice"_n, "bs1"));
   chain.block_signing_private_keys.emplace(get_public_key("bob"_n,   "bs1"), get_private_key("bob"_n,   "bs1"));

   BOOST_REQUIRE(produce_until_blocks_from(chain, {"alice"_n, "bob"_n}, 300));

   BOOST_REQUIRE_EQUAL( chain.validate(), true );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(producer_m_of_n_test) try {
   savanna_tester chain;

   chain.create_accounts( {"alice"_n,"bob"_n} );
   chain.produce_block();


   vector<producer_authority> sch1 = {
                                 producer_authority{"alice"_n, block_signing_authority_v0{2, {{get_public_key("alice"_n, "bs1"), 1}, {get_public_key("alice"_n, "bs2"), 1}}}},
                                 producer_authority{"bob"_n,   block_signing_authority_v0{2, {{get_public_key("bob"_n,   "bs1"), 1}, {get_public_key("bob"_n,   "bs2"), 1}}}}
                               };

   auto res = chain.set_producer_schedule( sch1 );
   chain.block_signing_private_keys.emplace(get_public_key("alice"_n, "bs1"), get_private_key("alice"_n, "bs1"));
   chain.block_signing_private_keys.emplace(get_public_key("alice"_n, "bs2"), get_private_key("alice"_n, "bs2"));
   chain.block_signing_private_keys.emplace(get_public_key("bob"_n,   "bs1"), get_private_key("bob"_n,   "bs1"));
   chain.block_signing_private_keys.emplace(get_public_key("bob"_n,   "bs2"), get_private_key("bob"_n,   "bs2"));

   BOOST_REQUIRE(produce_until_blocks_from(chain, {"alice"_n, "bob"_n}, 300));

   BOOST_REQUIRE_EQUAL( chain.validate(), true );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(satisfiable_msig_test) try {
   savanna_tester chain;
   chain.create_accounts( {"alice"_n,"bob"_n} );
   chain.produce_block();

   vector<producer_authority> sch1 = {
           producer_authority{"alice"_n, block_signing_authority_v0{2, {{get_public_key("alice"_n, "bs1"), 1}}}}
   };

   // ensure that the entries in a wtmsig schedule are rejected if not satisfiable
   BOOST_REQUIRE_EXCEPTION(
      chain.set_producer_schedule( sch1 ), wasm_execution_error,
      fc_exception_message_is( "producer schedule includes an unsatisfiable authority for alice" )
   );

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(duplicate_producers_test) try {
   savanna_tester chain;

   chain.create_accounts( {"alice"_n} );
   chain.produce_block();

   vector<producer_authority> sch1 = {
           producer_authority{"alice"_n, block_signing_authority_v0{1, {{get_public_key("alice"_n, "bs1"), 1}}}},
           producer_authority{"alice"_n, block_signing_authority_v0{1, {{get_public_key("alice"_n, "bs2"), 1}}}}
   };

   // ensure that the schedule is rejected if it has duplicate producers in it
   BOOST_REQUIRE_EXCEPTION(
      chain.set_producer_schedule( sch1 ), wasm_execution_error,
      fc_exception_message_is( "duplicate producer name in producer schedule" )
   );

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( duplicate_keys_test, legacy_validating_tester ) try {
   create_accounts( {"alice"_n,"bob"_n} );
   produce_block();

   vector<producer_authority> sch1 = {
           producer_authority{"alice"_n, block_signing_authority_v0{2, {{get_public_key("alice"_n, "bs1"), 1}, {get_public_key("alice"_n, "bs1"), 1}}}}
   };

   // ensure that the schedule is rejected if it has duplicate keys for a single producer in it
   BOOST_REQUIRE_EXCEPTION(
      set_producer_schedule( sch1 ), wasm_execution_error,
      fc_exception_message_is( "producer schedule includes a duplicated key for alice" )
   );

   // ensure that multiple producers are allowed to share keys
   vector<producer_authority> sch2 = {
           producer_authority{"alice"_n, block_signing_authority_v0{1, {{get_public_key("alice"_n, "bs1"), 1}}}},
           producer_authority{"bob"_n,   block_signing_authority_v0{1, {{get_public_key("alice"_n, "bs1"), 1}}}}
   };

   set_producer_schedule( sch2 );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( large_authority_overflow_test ) try {

   block_signing_authority_v0 auth;
   { // create a large authority that should overflow
      const size_t pre_overflow_count = 65'537UL; // enough for weights of 0xFFFF to add up to 0xFFFFFFFF
      auth.keys.reserve(pre_overflow_count + 1);

      for (size_t i = 0; i < pre_overflow_count; i++) {
         auto key_str = std::to_string(i) + "_bsk";
         auth.keys.emplace_back(key_weight{get_public_key("alice"_n, key_str), 0xFFFFU});
      }

      // reduce the last weight by 1 so that its unsatisfiable
      auth.keys.back().weight = 0xFFFEU;

      // add one last key with a weight of 2 so that its only satisfiable with values that sum to an overflow of 32bit uint
      auth.keys.emplace_back(key_weight{get_public_key("alice"_n, std::to_string(pre_overflow_count) + "_bsk"), 0x0002U});

      auth.threshold = 0xFFFFFFFFUL;
   }

   std::set<public_key_type> provided_keys;
   { // construct a set of all keys to provide
      for( const auto& kw: auth.keys) {
         provided_keys.emplace(kw.key);
      }
   }

   { // prove the naive accumulation overflows
      uint32_t total = 0;
      for( const auto& kw: auth.keys) {
         total += kw.weight;
      }
      BOOST_REQUIRE_EQUAL(total, 0x0UL);
   }

   auto res = auth.keys_satisfy_and_relevant(provided_keys);

   BOOST_REQUIRE_EQUAL(res.first, true);
   BOOST_REQUIRE_EQUAL(res.second, provided_keys.size());
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( extra_signatures_test ) try {
   savanna_tester main;

   main.create_accounts( {"alice"_n} );
   main.produce_block();

   vector<producer_authority> sch1 = {
      producer_authority{"alice"_n, block_signing_authority_v0{1,  {
                                                                     {get_public_key("alice"_n, "bs1"), 1},
                                                                     {get_public_key("alice"_n, "bs2"), 1},
                                                                     {get_public_key("alice"_n, "bs3"), 1},
                                                                  }
                                                             }
                        }
   };

   main.set_producer_schedule( sch1 );

   main.block_signing_private_keys.emplace(get_public_key("alice"_n, "bs1"), get_private_key("alice"_n, "bs1"));
   main.block_signing_private_keys.emplace(get_public_key("alice"_n, "bs2"), get_private_key("alice"_n, "bs2"));

   BOOST_REQUIRE( main.control->pending_block_producer() == "sysio"_n );
   main.produce_blocks(24);
   BOOST_REQUIRE( main.control->pending_block_producer() == "alice"_n );

   mutable_block_ptr b;

   // Generate a valid block and then corrupt it by adding an extra signature.
   {
      tester remote(setup_policy::none);
      push_blocks(main, remote);

      remote.block_signing_private_keys.emplace(get_public_key("alice"_n, "bs1"), get_private_key("alice"_n, "bs1"));
      remote.block_signing_private_keys.emplace(get_public_key("alice"_n, "bs2"), get_private_key("alice"_n, "bs2"));

      // Generate the block that will be corrupted.
      auto valid_block = remote.produce_block();

      BOOST_REQUIRE( valid_block->producer == "alice"_n );

      // Make a copy of pointer to the valid block.
      b = valid_block->clone();
      BOOST_REQUIRE_EQUAL( b->block_extensions.size(), 1u );

      // Extract the existing signatures.
      constexpr auto additional_sigs_eid = additional_block_signatures_extension::extension_id();
      auto exts = b->validate_and_extract_extensions();
      BOOST_REQUIRE_EQUAL( exts.count( additional_sigs_eid ), 1u );
      auto additional_sigs = std::get<additional_block_signatures_extension>(exts.lower_bound( additional_sigs_eid )->second).signatures;
      BOOST_REQUIRE_EQUAL( additional_sigs.size(), 1u );

      // Generate the extra signature and add to additonal_sigs.
      additional_sigs.emplace_back( remote.get_private_key("alice"_n, "bs3").sign(b->calculate_id()) );
      additional_sigs.emplace_back( remote.get_private_key("alice"_n, "bs4").sign(b->calculate_id()) );

      // Serialize the augmented additional signatures back into the block extensions.
      b->block_extensions.clear();
      emplace_extension(b->block_extensions,
                        additional_sigs_eid, fc::raw::pack( additional_sigs ));
   }

   // Push block with extra signature to the main chain.
   auto sb = signed_block::create_signed_block(std::move(b));
   BOOST_REQUIRE_EXCEPTION( main.push_block(sb), wrong_signing_key, fc_exception_message_starts_with("number of block signatures") );

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
