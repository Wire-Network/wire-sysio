#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/proposer_policy.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <variant>

#include "fork_test_utilities.hpp"

using namespace sysio::testing;
using namespace sysio::chain;
using mvo = fc::mutable_variant_object;

namespace {

/// A key no block signature can ever produce: a well-formed compressed-point prefix over an x
/// coordinate above the R1 field prime. o2i_ECPublicKey rejects it, so R1 key validity answers
/// false for it -- and, until the shim absorbed the decode failure, threw instead.
public_key_type undecodable_r1_key() {
   fc::crypto::r1::public_key_data data{};
   data[0] = 0x02;
   std::fill( data.begin() + 1, data.end(), '\xff' );
   return public_key_type{ fc::crypto::public_key::storage_type{ std::in_place_index<1>,
                                                                 fc::crypto::r1::public_key_shim{ data } } };
}

/// The all-zero K1 key -- what a producer that registered without setting a signing key holds,
/// and what the chain's own K1 validity test rejects.
public_key_type zero_k1_key() { return public_key_type{}; }

/// A BLS key whose payload is absent. fc reflects the shim's shared_ptr behind a presence flag,
/// so clearing that flag unpacks to a null pointer. Built by packing a real key and dropping the
/// payload, rather than by hand, so it stays correct if the encoding changes.
std::vector<char> strip_bls_payload( const std::vector<char>& packed ) {
   const auto bls_index = static_cast<char>( fc::crypto::public_key::key_type::bls );
   for( size_t i = 0; i + 1 < packed.size(); ++i ) {
      if( packed[i] == bls_index && packed[i + 1] == 1 ) {
         std::vector<char> stripped( packed.begin(), packed.begin() + i + 1 );
         stripped.push_back( 0 );                                      // payload absent
         const auto rest = i + 2 + fc::crypto::bls::public_key_data_size;
         stripped.insert( stripped.end(), packed.begin() + rest, packed.end() );
         return stripped;
      }
   }
   BOOST_FAIL( "no BLS payload found in packed schedule" );
   return {};
}

/// A real BLS public key, used only as a carrier for the payload-stripping above.
public_key_type bls_key() {
   return public_key_type::from_string(
      "PUB_BLS_sGOyYNtpmmjfsNbQaiGJrPxeSg9sdx0nRtfhI_KnWoACXLL53FIf1HjpcN8wX0cYQyOE60NLSI9iPY8mIlT4GkiFMT3ez7j2IbBBzR0D1MthC0B_fYlgYWwjcbqCOowSaH48KA" );
}

/// A WebAuthn key: well-formed, and of a type the chain rejects when it recovers a key from a
/// block signature, so a producer holding one could never sign.
public_key_type webauthn_key() {
   return public_key_type::from_string(
      "PUB_WA_WdCPfafVNxVMiW5ybdNs83oWjenQXvSt1F49fg9mv7qrCiRwHj5b38U3ponCFWxQTkDsMC" );
}

} // namespace

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

   // Threshold (2) exceeds sum of key weights (1) — rejected by proposer_policy::validate()
   // (via set_proposed_producers_common). Error message prefix is "proposer policy version N:".
   BOOST_REQUIRE_EXCEPTION(
      chain.set_producer_schedule( sch1 ), wasm_execution_error,
      fc_exception_message_contains( "not satisfiable by sum of key weights" )
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

   // Duplicate producer name rejected by proposer_policy::validate().
   BOOST_REQUIRE_EXCEPTION(
      chain.set_producer_schedule( sch1 ), wasm_execution_error,
      fc_exception_message_contains( "duplicate producer name" )
   );

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( duplicate_keys_test, validating_tester ) try {
   create_accounts( {"alice"_n,"bob"_n} );
   produce_block();

   vector<producer_authority> sch1 = {
           producer_authority{"alice"_n, block_signing_authority_v0{2, {{get_public_key("alice"_n, "bs1"), 1}, {get_public_key("alice"_n, "bs1"), 1}}}}
   };

   // Duplicate keys within a single producer's authority rejected by proposer_policy::validate().
   BOOST_REQUIRE_EXCEPTION(
      set_producer_schedule( sch1 ), wasm_execution_error,
      fc_exception_message_contains( "authority has duplicate key" )
   );

   // Multiple producers are allowed to share keys (key-uniqueness is per-authority, not global).
   vector<producer_authority> sch2 = {
           producer_authority{"alice"_n, block_signing_authority_v0{1, {{get_public_key("alice"_n, "bs1"), 1}}}},
           producer_authority{"bob"_n,   block_signing_authority_v0{1, {{get_public_key("alice"_n, "bs1"), 1}}}}
   };

   set_producer_schedule( sch2 );
} FC_LOG_AND_RETHROW()

// Empty producer schedule is rejected by proposer_policy::validate().
// Note: the transaction_context::set_proposed_producers() early-return-on-empty is
// behind this check so it never fires when the intrinsic is called via setprods.
BOOST_AUTO_TEST_CASE(empty_producer_schedule_test) try {
   savanna_tester chain;
   chain.produce_block();

   vector<producer_authority> empty_sch;

   BOOST_REQUIRE_EXCEPTION(
      chain.set_producer_schedule( empty_sch ), wasm_execution_error,
      fc_exception_message_contains( "producer schedule must not be empty" )
   );
} FC_LOG_AND_RETHROW()

// Happy-path direct test for proposer_policy::validate(). Guards against the
// method ever accidentally rejecting legitimate input — complements the negative
// tests below. The set_producer_schedule tests give indirect coverage, but a
// direct call pins the contract cheaply.
BOOST_AUTO_TEST_CASE(validate_accepts_well_formed_policy) try {
   proposer_policy pol;
   pol.proposer_schedule.version = 1;
   pol.proposer_schedule.producers = {
      producer_authority{ "alice"_n, block_signing_authority_v0{ 1, {{ get_public_key("alice"_n, "bs1"), 1 }} } },
      producer_authority{ "bob"_n,   block_signing_authority_v0{ 2, {{ get_public_key("bob"_n,   "bs1"), 1 },
                                                                    { get_public_key("bob"_n,   "bs2"), 1 }} } }
   };
   BOOST_CHECK_NO_THROW(pol.validate());
} FC_LOG_AND_RETHROW()

// proposer_policy::validate() caps producer count at config::max_producers.
// Tested directly against validate() rather than through set_producer_schedule
// because creating > max_producers accounts in one test exhausts block CPU
// budget. The intrinsic wiring is already exercised by the other tests in this
// suite; this test verifies the specific branch in validate().
BOOST_AUTO_TEST_CASE(validate_rejects_too_many_producers) try {
   proposer_policy pol;
   const size_t n = config::max_producers + 1;
   pol.proposer_schedule.version = 1;
   pol.proposer_schedule.producers.reserve(n);
   // Build deterministic valid account names ("paa", "pab", ..., "pzz", ...).
   for (size_t i = 0; i < n; ++i) {
      std::string nm = "p";
      for (size_t v = i; nm.size() < 12; ) {
         nm.push_back(static_cast<char>('a' + (v % 26)));
         v /= 26;
         if (v == 0) break;
      }
      pol.proposer_schedule.producers.push_back(
         producer_authority{ account_name{nm},
                             block_signing_authority_v0{ 1, {{ get_public_key(account_name{nm}, "bs1"), 1 }} } });
   }
   BOOST_CHECK_EXCEPTION(pol.validate(), producer_schedule_exception,
      fc_exception_message_contains("exceeds max"));
} FC_LOG_AND_RETHROW()

// One block may carry a signature for every key named by its producer authority. Bound that
// per-producer work at the policy layer so snapshots and every proposal path enforce the same cap.
BOOST_AUTO_TEST_CASE(validate_enforces_authority_key_limit) try {
   proposer_policy pol;
   block_signing_authority_v0 authority;
   authority.threshold = 1;
   for (size_t i = 0; i < proposer_policy::max_authority_keys; ++i) {
      authority.keys.push_back({get_public_key("alice"_n, "bs" + std::to_string(i)), 1});
   }
   pol.proposer_schedule.version = 1;
   pol.proposer_schedule.producers = {
      producer_authority{"alice"_n, std::move(authority)}
   };
   BOOST_CHECK_NO_THROW(pol.validate());

   auto& stored_authority = std::get<block_signing_authority_v0>(
      pol.proposer_schedule.producers.front().authority);
   stored_authority.keys.push_back(
      {get_public_key("alice"_n, "bs" + std::to_string(proposer_policy::max_authority_keys)), 1});
   BOOST_CHECK_EXCEPTION(pol.validate(), producer_schedule_exception,
      fc_exception_message_contains("authority key count (6) exceeds max (5)"));
} FC_LOG_AND_RETHROW()

// proposer_policy::validate() rejects a per-authority threshold of zero.
BOOST_AUTO_TEST_CASE(authority_threshold_zero_test) try {
   savanna_tester chain;
   chain.create_accounts( {"alice"_n} );
   chain.produce_block();

   vector<producer_authority> sch = {
      producer_authority{ "alice"_n, block_signing_authority_v0{ 0, {{ get_public_key("alice"_n, "bs1"), 1 }} } }
   };

   BOOST_REQUIRE_EXCEPTION(
      chain.set_producer_schedule( sch ), wasm_execution_error,
      fc_exception_message_contains( "authority threshold must be positive" )
   );
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

      // Add extra bogus signatures to producer_signatures.
      b->producer_signatures.emplace_back( remote.get_private_key("alice"_n, "bs3").sign(b->calculate_id()) );
      b->producer_signatures.emplace_back( remote.get_private_key("alice"_n, "bs4").sign(b->calculate_id()) );
   }

   // Push block with extra signature to the main chain.
   auto sb = signed_block::create_signed_block(std::move(b));
   BOOST_REQUIRE_EXCEPTION( main.push_block(sb), wrong_signing_key, fc_exception_message_starts_with("number of block signatures") );

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(schedule_admits_unsignable_keys) try {
   savanna_tester chain;
   chain.create_accounts( {"alice"_n, "bobby"_n, "carol"_n} );
   chain.produce_block();

   // None of these keys can be produced by recovering a key from a block signature, so none of
   // these producers can sign. The schedule must still publish: the system contract rebuilds one
   // from its own producer table inside onblock, and a rejection there rolls back the rebuild
   // timestamp along with it, so the rebuild re-fires -- and fails again -- on every block that
   // follows, permanently.
   vector<producer_authority> sch = {
      producer_authority{ "alice"_n, block_signing_authority_v0{ 1, {{ undecodable_r1_key(), 1 }} } },
      producer_authority{ "bobby"_n, block_signing_authority_v0{ 1, {{ zero_k1_key(), 1 }} } },
      producer_authority{ "carol"_n, block_signing_authority_v0{ 1, {{ webauthn_key(), 1 }} } }
   };

   auto trace = chain.set_producer_schedule( sch );
   BOOST_REQUIRE( !trace->except );
   BOOST_REQUIRE( trace->receipt );

   // Accepting the action is not the claim. The policy is assembled, logged and diffed when the
   // block is finalized, which is where an unusable key would be dereferenced or rejected, so the
   // block has to be produced and the proposal observed.
   auto block = chain.produce_block();
   BOOST_REQUIRE( block->new_proposer_policy_diff );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(legacy_format_admits_unsignable_keys) try {
   savanna_tester chain;
   chain.create_accounts( {"alice"_n} );
   chain.produce_block();

   // The legacy producer_key format is lenient on the same terms. Upstream kept a key check here
   // only to avoid a consensus change on an already-live intrinsic; it never established that a
   // key could sign, since a curve point whose private key nobody holds passes it just the same.
   for( const auto& key : { zero_k1_key(), undecodable_r1_key(), webauthn_key() } ) {
      vector<legacy::producer_key> sched = {{ "alice"_n, key }};
      auto trace = chain.push_action( config::system_account_name, "setprodkeys"_n,
                                      config::system_account_name, mvo()("schedule", sched) );
      BOOST_REQUIRE( !trace->except );
      BOOST_REQUIRE( trace->receipt );

      // As above: the proposal is only assembled when the block is finalized.
      auto block = chain.produce_block();
      BOOST_REQUIRE( block->new_proposer_policy_diff );
   }
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(absent_bls_payload_is_rejected_on_both_formats) try {
   // The schedule path no longer screens key types, so a BLS key reaches it. Its shim holds the
   // payload behind a shared_ptr that fc lets deserialize as absent, and every accessor -- the
   // to_string a node performs when it logs a schedule, among them -- would dereference null.
   // A one-key authority slips past proposer_policy::validate untouched, because the first
   // insertion into the uniqueness set compares nothing. Deserialization has to reject it.

   // Authority format, as set_proposed_producers_ex(1) unpacks it.
   vector<producer_authority> authority_schedule = {
      producer_authority{ "alice"_n, block_signing_authority_v0{ 1, {{ bls_key(), 1 }} } }
   };
   const auto unpack_bytes = []( const std::vector<char>& bytes, auto& out ) {
      fc::datastream<const char*> ds( bytes.data(), bytes.size() );
      fc::raw::unpack( ds, out );
   };

   auto authority_bytes = strip_bls_payload( fc::raw::pack( authority_schedule ) );
   vector<producer_authority> unpacked_authority;
   BOOST_CHECK_THROW( unpack_bytes( authority_bytes, unpacked_authority ), fc::exception );

   // Legacy format, as set_proposed_producers unpacks it.
   vector<legacy::producer_key> legacy_schedule = {{ "alice"_n, bls_key() }};
   auto legacy_bytes = strip_bls_payload( fc::raw::pack( legacy_schedule ) );
   vector<legacy::producer_key> unpacked_legacy;
   BOOST_CHECK_THROW( unpack_bytes( legacy_bytes, unpacked_legacy ), fc::exception );

   // The unmodified bytes still round-trip, so the guard rejects only the absent payload.
   BOOST_CHECK_NO_THROW( unpack_bytes( fc::raw::pack( authority_schedule ), unpacked_authority ) );
   BOOST_CHECK_NO_THROW( unpack_bytes( fc::raw::pack( legacy_schedule ), unpacked_legacy ) );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(unsignable_key_never_satisfies_authority) try {
   // Why admitting those keys costs nothing: the presented set is built by recovering keys from
   // the block's signatures, so a key that no signature yields is never in it, and the authority
   // is never satisfied. The producer burns its rounds; the chain keeps updating schedules.
   block_signing_authority_v0 auth{ 1, {{ undecodable_r1_key(), 1 }, { zero_k1_key(), 1 }} };

   std::set<public_key_type> presented = { get_public_key("alice"_n, "bs1"),
                                           get_public_key("bobby"_n, "bs1") };

   auto [satisfied, relevant] = auth.keys_satisfy_and_relevant( presented );
   BOOST_CHECK( !satisfied );
   BOOST_CHECK_EQUAL( relevant, 0u );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( block_signed_with_non_k1_r1_key_test ) try {
   savanna_tester main;

   main.create_accounts( {"alice"_n} );
   main.produce_block();

   vector<producer_authority> sch1 = {
      producer_authority{"alice"_n, block_signing_authority_v0{1, {{get_public_key("alice"_n, "bs1"), 1}}}}
   };
   main.set_producer_schedule( sch1 );
   main.block_signing_private_keys.emplace(get_public_key("alice"_n, "bs1"), get_private_key("alice"_n, "bs1"));

   BOOST_REQUIRE( main.control->pending_block_producer() == "sysio"_n );
   main.produce_blocks(24);
   BOOST_REQUIRE( main.control->pending_block_producer() == "alice"_n );

   mutable_block_ptr b;

   // Generate a valid block, then re-sign it with a key of a type no producer may sign with.
   {
      tester remote(setup_policy::none);
      push_blocks(main, remote);

      remote.block_signing_private_keys.emplace(get_public_key("alice"_n, "bs1"), get_private_key("alice"_n, "bs1"));

      auto valid_block = remote.produce_block();
      BOOST_REQUIRE( valid_block->producer == "alice"_n );

      b = valid_block->clone();

      // The block id excludes producer_signatures, so replacing them does not move it.
      b->producer_signatures.clear();
      b->producer_signatures.emplace_back(
         fc::crypto::private_key::generate( fc::crypto::private_key::key_type::em ).sign( b->calculate_id() ) );
   }

   // This is where the K1/R1 rule decides something, and the reason a proposed schedule does not
   // need to repeat it: the key type is screened on every key recovered from a block signature.
   auto sb = signed_block::create_signed_block(std::move(b));
   BOOST_REQUIRE_EXCEPTION( main.push_block(sb), unactivated_key_type,
                            fc_exception_message_contains("Block signed with invalid key type") );

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
