#include <boost/test/unit_test.hpp>

#include <sysio/chain/subjective_billing.hpp>
#include <sysio/testing/tester.hpp>
#include <test_common.hpp>
#include <test_contracts.hpp>
#include <fc/time.hpp>

namespace {

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(subjective_billing_test)

BOOST_AUTO_TEST_CASE( subjective_bill_test ) {

   fc::logger log;

   transaction_id_type id1 = sha256::hash( "1" );
   transaction_id_type id2 = sha256::hash( "2" );
   transaction_id_type id3 = sha256::hash( "3" );
   account_name a = "a"_n;
   account_name b = "b"_n;
   account_name c = "c"_n; // not used

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing timing_sub_bill;
   const auto halftime = now + fc::milliseconds(timing_sub_bill.get_expired_accumulator_average_window() * subjective_billing::subjective_time_interval_ms / 2);
   const auto endtime = now + fc::milliseconds(timing_sub_bill.get_expired_accumulator_average_window() * subjective_billing::subjective_time_interval_ms);


   {  // Failed transactions remain until expired in subjective billing.
      subjective_billing sub_bill;

      sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 13}}}, {} );
      sub_bill.subjective_bill( id2, now_sec, {{a, {.cpu_usage_us = 11}}}, {} );
      sub_bill.subjective_bill( id3, now_sec, {{b, {.cpu_usage_us = 9}}}, {} );

      BOOST_CHECK_EQUAL( 13+11, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 9, sub_bill.get_subjective_bill(b, now).count() );

      sub_bill.on_block(log, {}, now);

      BOOST_CHECK_EQUAL( 13+11, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 9, sub_bill.get_subjective_bill(b, now).count() );

      // expires transactions but leaves them in the decay at full value
      sub_bill.remove_expired( log, now + fc::microseconds(1), now, [](){ return false; } );

      BOOST_CHECK_EQUAL( 13+11, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 9, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(c, now).count() ); // c not used

      // ensure that the value decays away at the window
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(b, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(c, endtime).count() ); // c not used
   }
   {  // db_read_mode HEAD mode, so transactions are immediately reverted
      subjective_billing sub_bill;

      sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 23}}}, {} );
      sub_bill.subjective_bill( id2, now_sec, {{a, {.cpu_usage_us = 19}}}, {} );
      sub_bill.subjective_bill( id3, now_sec, {{b, {.cpu_usage_us = 7}}}, {} );

      BOOST_CHECK_EQUAL( 23+19, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 7, sub_bill.get_subjective_bill(b, now).count() );

      sub_bill.on_block(log, {}, now); // have not seen any of the transactions come back yet

      BOOST_CHECK_EQUAL( 23+19, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 7, sub_bill.get_subjective_bill(b, now).count() );

      sub_bill.on_block(log, {}, now);
      sub_bill.remove_subjective_billing( id1, 0 ); // simulate seeing id1 come back in block (this is what on_block would do)

      BOOST_CHECK_EQUAL( 19, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 7, sub_bill.get_subjective_bill(b, now).count() );
   }
   { // failed handling logic, decay with repeated failures should be exponential, single failures should be linear
      subjective_billing sub_bill;

      sub_bill.subjective_bill_failure({{a, {.cpu_usage_us = 1024}}}, {}, now);
      sub_bill.subjective_bill_failure({{b, {.cpu_usage_us = 1024}}}, {}, now);
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(b, now).count() );

      sub_bill.subjective_bill_failure({{a, {.cpu_usage_us = 1024}}}, {}, halftime);
      BOOST_CHECK_EQUAL( 512 + 1024, sub_bill.get_subjective_bill(a, halftime).count() );
      BOOST_CHECK_EQUAL( 512, sub_bill.get_subjective_bill(b, halftime).count() );

      sub_bill.subjective_bill_failure({{a, {.cpu_usage_us = 1024}}}, {}, endtime);
      BOOST_CHECK_EQUAL( 256 + 512 + 1024, sub_bill.get_subjective_bill(a, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(b, endtime).count() );
   }

   { // expired handling logic, full billing until expiration then failed/decay logic
      subjective_billing sub_bill;

      sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 1024}}}, {} );
      sub_bill.subjective_bill( id2, fc::time_point_sec{now + fc::seconds(1)}, {{a, {.cpu_usage_us = 1024}}}, {} );
      sub_bill.subjective_bill( id3, now_sec, {{b, {.cpu_usage_us = 1024}}}, {} );
      BOOST_CHECK_EQUAL( 1024 + 1024, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(b, now).count() );

      sub_bill.remove_expired( log, now, now, [](){ return false; } );
      BOOST_CHECK_EQUAL( 1024 + 1024, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(b, now).count() );

      BOOST_CHECK_EQUAL( 512 + 1024, sub_bill.get_subjective_bill(a, halftime).count() );
      BOOST_CHECK_EQUAL( 512, sub_bill.get_subjective_bill(b, halftime).count() );

      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(a, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(b, endtime).count() );

      sub_bill.remove_expired( log, now + fc::seconds(1), now, [](){ return false; } );
      BOOST_CHECK_EQUAL( 1024 + 1024, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(b, now).count() );

      BOOST_CHECK_EQUAL( 512 + 512, sub_bill.get_subjective_bill(a, halftime).count() );
      BOOST_CHECK_EQUAL( 512, sub_bill.get_subjective_bill(b, halftime).count() );

      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(b, endtime).count() );
   }
}

BOOST_AUTO_TEST_CASE( subjective_bill_multiple_accounts_test ) {

   fc::logger log;

   transaction_id_type id1 = sha256::hash( "1" );
   transaction_id_type id2 = sha256::hash( "2" );
   transaction_id_type id3 = sha256::hash( "3" );
   account_name a = "a"_n;
   account_name b = "b"_n;
   account_name c = "c"_n;
   account_name x = "x"_n; // not used

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing timing_sub_bill;
   const auto halftime = now + fc::milliseconds(timing_sub_bill.get_expired_accumulator_average_window() * subjective_billing::subjective_time_interval_ms / 2);
   const auto endtime = now + fc::milliseconds(timing_sub_bill.get_expired_accumulator_average_window() * subjective_billing::subjective_time_interval_ms);

   {  // Failed transactions remain until expired in subjective billing.
      subjective_billing sub_bill;

      sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 13}},{c, {.cpu_usage_us = 42}}}, {} );
      sub_bill.subjective_bill( id2, now_sec, {{a, {.cpu_usage_us = 11}},{c, {.cpu_usage_us = 23}}}, {} );
      sub_bill.subjective_bill( id3, now_sec, {{b, {.cpu_usage_us = 9}}}, {{c, {fc::microseconds(7)}}} );

      BOOST_CHECK_EQUAL( 13+11, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 9, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 42+23+7, sub_bill.get_subjective_bill(c, now).count() );

      sub_bill.on_block(log, {}, now);

      BOOST_CHECK_EQUAL( 13+11, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 9, sub_bill.get_subjective_bill(b, now).count() );

      // expires transactions but leaves them in the decay at full value
      sub_bill.remove_expired( log, now + fc::microseconds(1), now, [](){ return false; } );

      BOOST_CHECK_EQUAL( 13+11, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 9, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 42+23+7, sub_bill.get_subjective_bill(c, now).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(x, now).count() );

      // ensure that the value decays away at the window
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(b, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(c, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(x, endtime).count() );
   }
   {  // db_read_mode HEAD mode, so transactions are immediately reverted
      subjective_billing sub_bill;

      sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 23}},{c, {.cpu_usage_us = 11}}}, {} );
      sub_bill.subjective_bill( id2, now_sec, {{c, {.cpu_usage_us = 3}}}, {{a, {fc::microseconds(19)}}} );
      sub_bill.subjective_bill( id3, now_sec, {{b, {.cpu_usage_us = 7}},{c, {.cpu_usage_us = 1}}}, {} );

      BOOST_CHECK_EQUAL( 23+19, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 7, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 11+3+1, sub_bill.get_subjective_bill(c, now).count() );

      sub_bill.on_block(log, {}, now); // have not seen any of the transactions come back yet

      BOOST_CHECK_EQUAL( 23+19, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 7, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 11+3+1, sub_bill.get_subjective_bill(c, now).count() );

      sub_bill.on_block(log, {}, now);
      sub_bill.remove_subjective_billing( id1, 0 ); // simulate seeing id1 come back in block (this is what on_block would do)

      BOOST_CHECK_EQUAL( 19, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 7, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 3+1, sub_bill.get_subjective_bill(c, now).count() );
   }
   { // failed handling logic, decay with repeated failures should be exponential, single failures should be linear
      subjective_billing sub_bill;

      sub_bill.subjective_bill_failure({{a, {.cpu_usage_us = 1024}},{c, {.cpu_usage_us = 2048}}}, {}, now);
      sub_bill.subjective_bill_failure({{b, {.cpu_usage_us = 1024}},{c, {.cpu_usage_us = 512}}}, {}, now);
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 2048+512, sub_bill.get_subjective_bill(c, now).count() );

      sub_bill.subjective_bill_failure({{a, {.cpu_usage_us = 1024}},{c, {.cpu_usage_us = 256}}}, {}, halftime);
      BOOST_CHECK_EQUAL( 512 + 1024, sub_bill.get_subjective_bill(a, halftime).count() );
      BOOST_CHECK_EQUAL( 512, sub_bill.get_subjective_bill(b, halftime).count() );
      BOOST_CHECK_EQUAL( (2048+512)/2+256, sub_bill.get_subjective_bill(c, halftime).count() );

      sub_bill.subjective_bill_failure({{a, {.cpu_usage_us = 1024}},{c, {.cpu_usage_us = 24}}}, {}, endtime);
      BOOST_CHECK_EQUAL( 256 + 512 + 1024, sub_bill.get_subjective_bill(a, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(b, endtime).count() );
      BOOST_CHECK_EQUAL( (2048+512)/2/2+256/2+24, sub_bill.get_subjective_bill(c, endtime).count() );
   }

   { // expired handling logic, full billing until expiration then failed/decay logic
      subjective_billing sub_bill;

      sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 1024}},{c, {.cpu_usage_us = 256}}}, {} );
      sub_bill.subjective_bill( id2, fc::time_point_sec{now + fc::seconds(1)}, {{a, {.cpu_usage_us = 1024}},{c, {.cpu_usage_us = 128}}}, {} );
      sub_bill.subjective_bill( id3, now_sec, {{b, {.cpu_usage_us = 1024}},{c, {.cpu_usage_us = 64}}}, {} );
      BOOST_CHECK_EQUAL( 1024 + 1024, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 256+128+64, sub_bill.get_subjective_bill(c, now).count() );

      // still billed for expired trxs
      auto num_removed = sub_bill.remove_expired( log, now, now, [](){ return false; } ).second;
      BOOST_CHECK_EQUAL( num_removed, 2 ); // id1, id3
      BOOST_CHECK_EQUAL( 1024 + 1024, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 256+128+64, sub_bill.get_subjective_bill(c, now).count() );

      BOOST_CHECK_EQUAL( 512 + 1024, sub_bill.get_subjective_bill(a, halftime).count() );
      BOOST_CHECK_EQUAL( 512, sub_bill.get_subjective_bill(b, halftime).count() );
      BOOST_CHECK_EQUAL( 128 + 128 + 32, sub_bill.get_subjective_bill(c, halftime).count() );

      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(a, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(b, endtime).count() );
      BOOST_CHECK_EQUAL( 128, sub_bill.get_subjective_bill(c, endtime).count() );

      num_removed = sub_bill.remove_expired( log, now + fc::seconds(1), now, [](){ return false; } ).second;
      BOOST_CHECK_EQUAL( num_removed, 1 ); // id2
      BOOST_CHECK_EQUAL( 1024 + 1024, sub_bill.get_subjective_bill(a, now).count() );
      BOOST_CHECK_EQUAL( 1024, sub_bill.get_subjective_bill(b, now).count() );
      BOOST_CHECK_EQUAL( 256+128+64, sub_bill.get_subjective_bill(c, now).count() );

      BOOST_CHECK_EQUAL( 512 + 512, sub_bill.get_subjective_bill(a, halftime).count() );
      BOOST_CHECK_EQUAL( 512, sub_bill.get_subjective_bill(b, halftime).count() );
      BOOST_CHECK_EQUAL( (256+128+64)/2, sub_bill.get_subjective_bill(c, halftime).count() );

      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(b, endtime).count() );
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(c, endtime).count() );
   }
}

BOOST_AUTO_TEST_CASE( subjective_bill_account_in_both_maps_test ) {
   // Regression: when the same account appears in both accounts_billing (as a payer)
   // and auth_cpu (as a non-payer first_authorizer), subjective_bill must track the
   // sum of both contributions in the per-trx entry. Otherwise the cache accumulator
   // leaks the earlier contribution permanently when the trx is later removed.
   //
   // This models a multi-action trx where account 'a' is the explicit payer of one
   // action and the first_authorizer of another action whose payer is a different
   // account or the contract.

   transaction_id_type id1 = sha256::hash( "1" );
   account_name a = "a"_n;

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing sub_bill;

   sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 10}}}, {{a, fc::microseconds(5)}} );

   BOOST_CHECK_EQUAL( 10 + 5, sub_bill.get_subjective_bill(a, now).count() );
   BOOST_CHECK_EQUAL( 1u, sub_bill.get_account_cache_size() );

   // Simulate id1 landing in a block.
   sub_bill.remove_subjective_billing( id1, 0 );

   // Full contribution must be cleared; with the overlap bug only 5 would be
   // subtracted, leaving 10 stuck in the cache.
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
   // Empty entry should also be evicted from the per-account cache.
   BOOST_CHECK_EQUAL( 0u, sub_bill.get_account_cache_size() );
}

BOOST_AUTO_TEST_CASE( disable_payer_billing_test ) {
   // When payer billing is disabled, subjective_bill must skip the accounts_billing
   // loop entirely; only auth_cpu contributions accumulate.

   transaction_id_type id1 = sha256::hash( "1" );
   account_name a = "a"_n;
   account_name b = "b"_n;

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing sub_bill;
   BOOST_CHECK( !sub_bill.is_payer_billing_disabled() );

   sub_bill.disable_payer_billing( true );
   BOOST_CHECK( sub_bill.is_payer_billing_disabled() );

   sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 100}}}, {{b, fc::microseconds(5)}} );

   // 'a' from accounts_billing is skipped; 'b' from auth_cpu accumulates.
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
   BOOST_CHECK_EQUAL( 5, sub_bill.get_subjective_bill(b, now).count() );

   // Same for the failure path.
   sub_bill.subjective_bill_failure( {{a, {.cpu_usage_us = 100}}}, {{b, fc::microseconds(5)}}, now );
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
   BOOST_CHECK_EQUAL( 10, sub_bill.get_subjective_bill(b, now).count() );

   // Re-enable payer billing; future bills do accumulate 'a'.
   sub_bill.disable_payer_billing( false );
   BOOST_CHECK( !sub_bill.is_payer_billing_disabled() );
   transaction_id_type id2 = sha256::hash( "2" );
   sub_bill.subjective_bill( id2, now_sec, {{a, {.cpu_usage_us = 7}}}, {} );
   BOOST_CHECK_EQUAL( 7, sub_bill.get_subjective_bill(a, now).count() );
}

BOOST_AUTO_TEST_CASE( disable_account_test ) {
   // disable_account should exclude an account from all subjective billing
   // accumulation and report disabled status via getters.

   transaction_id_type id1 = sha256::hash( "1" );
   account_name a = "a"_n;
   account_name b = "b"_n;
   account_name c = "c"_n;

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing sub_bill;
   BOOST_CHECK( !sub_bill.is_account_disabled( a ) );

   sub_bill.disable_account( a );
   BOOST_CHECK( sub_bill.is_account_disabled( a ) );
   BOOST_CHECK( !sub_bill.is_account_disabled( b ) );

   // is_any_account_disabled is true if any listed account is disabled.
   action_payers_t has_a{a, b};
   action_payers_t no_a{b, c};
   BOOST_CHECK( sub_bill.is_any_account_disabled( has_a ) );
   BOOST_CHECK( !sub_bill.is_any_account_disabled( no_a ) );

   // Disabled account is skipped in subjective_bill (both maps).
   sub_bill.subjective_bill( id1, now_sec,
                             {{a, {.cpu_usage_us = 100}}, {b, {.cpu_usage_us = 11}}},
                             {{a, fc::microseconds(50)}, {c, fc::microseconds(3)}} );
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
   BOOST_CHECK_EQUAL( 11, sub_bill.get_subjective_bill(b, now).count() );
   BOOST_CHECK_EQUAL( 3, sub_bill.get_subjective_bill(c, now).count() );

   // Disabled account is skipped in subjective_bill_failure as well.
   sub_bill.subjective_bill_failure( {{a, {.cpu_usage_us = 100}}},
                                     {{a, fc::microseconds(50)}}, now );
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );

   // get_subjective_bill for a disabled account is always zero, even if the cache
   // happened to contain an entry from before the account was disabled.
   transaction_id_type id2 = sha256::hash( "2" );
   subjective_billing sub_bill2;
   sub_bill2.subjective_bill( id2, now_sec, {{a, {.cpu_usage_us = 9}}}, {} );
   BOOST_CHECK_EQUAL( 9, sub_bill2.get_subjective_bill(a, now).count() );
   sub_bill2.disable_account( a );
   BOOST_CHECK_EQUAL( 0, sub_bill2.get_subjective_bill(a, now).count() );
}

BOOST_AUTO_TEST_CASE( set_disabled_test ) {
   // When globally disabled, all billing operations are no-ops and getters return
   // neutral values.

   fc::logger log;
   transaction_id_type id1 = sha256::hash( "1" );
   account_name a = "a"_n;

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing sub_bill;
   BOOST_CHECK( !sub_bill.is_disabled() );

   sub_bill.set_disabled( true );
   BOOST_CHECK( sub_bill.is_disabled() );
   BOOST_CHECK( sub_bill.is_account_disabled( a ) );          // globally disabled implies per-account disabled
   BOOST_CHECK( sub_bill.is_any_account_disabled( {a} ) );

   sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 100}}}, {} );
   sub_bill.subjective_bill_failure( {{a, {.cpu_usage_us = 100}}}, {}, now );
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
   BOOST_CHECK_EQUAL( 0, sub_bill.get_account_cache_size() );

   // on_block / remove_expired are no-ops.
   sub_bill.on_block( log, {}, now );
   auto [completed, num_expired] = sub_bill.remove_expired( log, now, now, [](){ return false; } );
   BOOST_CHECK( completed );
   BOOST_CHECK_EQUAL( 0u, num_expired );

   // Re-enabling does not resurrect the skipped bills (they were never recorded).
   sub_bill.set_disabled( false );
   BOOST_CHECK( !sub_bill.is_disabled() );
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
}

BOOST_AUTO_TEST_CASE( subjective_bill_duplicate_id_test ) {
   // Calling subjective_bill twice with the same id must be idempotent; the second
   // call is silently ignored so the trx is not double-billed.

   transaction_id_type id1 = sha256::hash( "1" );
   account_name a = "a"_n;

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing sub_bill;

   sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 13}}}, {} );
   BOOST_CHECK_EQUAL( 13, sub_bill.get_subjective_bill(a, now).count() );

   // Second call with same id - different amount; must be ignored.
   sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 500}}}, {} );
   BOOST_CHECK_EQUAL( 13, sub_bill.get_subjective_bill(a, now).count() );

   // Single removal clears it entirely.
   sub_bill.remove_subjective_billing( id1, 0 );
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
   BOOST_CHECK_EQUAL( 0u, sub_bill.get_account_cache_size() );
}

BOOST_AUTO_TEST_CASE( remove_nonexistent_trx_test ) {
   // Removing a transaction id that is not in the cache must be a silent no-op,
   // both on an empty cache and when other unrelated entries exist. This is what
   // makes on_block safe to invoke for blocks that may contain trxs the local node
   // never speculatively executed.

   transaction_id_type id1 = sha256::hash( "1" );
   transaction_id_type id2 = sha256::hash( "2" );
   account_name a = "a"_n;

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing sub_bill;

   // Empty cache: no-op, no exception.
   sub_bill.remove_subjective_billing( id1, 0 );
   BOOST_CHECK_EQUAL( 0u, sub_bill.get_account_cache_size() );
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );

   // Populated cache, removing a different id: existing state preserved.
   sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 21}}}, {} );
   BOOST_CHECK_EQUAL( 21, sub_bill.get_subjective_bill(a, now).count() );

   sub_bill.remove_subjective_billing( id2, 0 );
   BOOST_CHECK_EQUAL( 21, sub_bill.get_subjective_bill(a, now).count() );
   BOOST_CHECK_EQUAL( 1u, sub_bill.get_account_cache_size() );
}

BOOST_AUTO_TEST_CASE( remove_expired_yield_exhausted_test ) {
   // remove_expired must honor the yield predicate: when yield() returns true
   // mid-iteration, the function returns (completed=false, num_expired=partial)
   // and leaves the un-processed expired trxs in the cache for a follow-up call.

   fc::logger log;
   account_name a = "a"_n;

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing sub_bill;

   // Bill several trxs with the same expiry so all are eligible for expiration.
   constexpr size_t total = 5;
   for( size_t i = 0; i < total; ++i ) {
      transaction_id_type id = sha256::hash( std::to_string(i) );
      sub_bill.subjective_bill( id, now_sec, {{a, {.cpu_usage_us = 10}}}, {} );
   }
   BOOST_CHECK_EQUAL( static_cast<int64_t>(10 * total), sub_bill.get_subjective_bill(a, now).count() );

   // Yield after the second iteration. remove_expired checks yield BEFORE each
   // entry, so we let the first 2 through then bail.
   size_t calls = 0;
   auto [completed, num_expired] = sub_bill.remove_expired(
      log, now + fc::microseconds(1), now,
      [&calls]() { return ++calls > 2; } );

   BOOST_CHECK( !completed );           // exhausted, not done
   BOOST_CHECK_EQUAL( 2u, num_expired ); // exactly two were processed

   // The remaining 3 are still in the trx cache (their pending_cpu_us is intact).
   // Total = 2 transitioned to expired_accumulator + 3 still pending = 50, all
   // visible at `now` (decay window has not elapsed).
   BOOST_CHECK_EQUAL( static_cast<int64_t>(10 * total), sub_bill.get_subjective_bill(a, now).count() );

   // A follow-up call without yielding finishes the work.
   auto [completed2, num_expired2] = sub_bill.remove_expired(
      log, now + fc::microseconds(1), now, []() { return false; } );
   BOOST_CHECK( completed2 );
   BOOST_CHECK_EQUAL( total - 2, num_expired2 );
}

BOOST_AUTO_TEST_CASE( on_block_nullptr_test ) {
   // on_block with a null block is a no-op; existing state is preserved.

   fc::logger log;
   transaction_id_type id1 = sha256::hash( "1" );
   account_name a = "a"_n;

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing sub_bill;
   sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 17}}}, {} );
   BOOST_CHECK_EQUAL( 17, sub_bill.get_subjective_bill(a, now).count() );

   sub_bill.on_block( log, {}, now );  // signed_block_ptr default is null
   BOOST_CHECK_EQUAL( 17, sub_bill.get_subjective_bill(a, now).count() );
   BOOST_CHECK_EQUAL( 1u, sub_bill.get_account_cache_size() );
}

BOOST_AUTO_TEST_CASE( is_any_account_disabled_test ) {
   account_name a = "a"_n;
   account_name b = "b"_n;
   account_name c = "c"_n;

   subjective_billing sub_bill;
   BOOST_CHECK( !sub_bill.is_any_account_disabled( {a, b, c} ) );
   BOOST_CHECK( !sub_bill.is_any_account_disabled( {} ) );

   sub_bill.disable_account( b );
   BOOST_CHECK( !sub_bill.is_any_account_disabled( {a, c} ) );
   BOOST_CHECK( sub_bill.is_any_account_disabled( {a, b} ) );
   BOOST_CHECK( sub_bill.is_any_account_disabled( {b} ) );
   BOOST_CHECK( !sub_bill.is_any_account_disabled( {} ) );

   // Global disable flips it on regardless of account set.
   sub_bill.set_disabled( true );
   BOOST_CHECK( sub_bill.is_any_account_disabled( {a, c} ) );
   BOOST_CHECK( sub_bill.is_any_account_disabled( {} ) );
}

BOOST_AUTO_TEST_CASE( reset_clears_all_state_test ) {
   // reset() must clear every piece of persistent state so the instance is
   // indistinguishable from a freshly-constructed one.

   transaction_id_type id1 = sha256::hash( "1" );
   account_name a = "a"_n;

   const auto now = time_point::now();
   const fc::time_point_sec now_sec{now};

   subjective_billing sub_bill;
   const auto default_allowed = sub_bill.get_subjective_account_cpu_allowed();
   const auto default_window = sub_bill.get_expired_accumulator_average_window();

   // Populate every field reset() is supposed to clear.
   sub_bill.set_disabled( true );
   sub_bill.disable_payer_billing( true );
   sub_bill.disable_account( a );
   sub_bill.set_subjective_account_cpu_allowed( fc::microseconds{42} );
   sub_bill.set_expired_accumulator_average_window( fc::microseconds{30 * 1000 * 1000} ); // 30s worth
   // Re-enable briefly to record a bill, then flip the disable back on.
   sub_bill.set_disabled( false );
   sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 17}}}, {} );
   sub_bill.set_disabled( true );

   sub_bill.reset();

   BOOST_CHECK( !sub_bill.is_disabled() );
   BOOST_CHECK( !sub_bill.is_payer_billing_disabled() );
   BOOST_CHECK( !sub_bill.is_account_disabled( a ) );
   BOOST_CHECK_EQUAL( default_allowed.count(), sub_bill.get_subjective_account_cpu_allowed().count() );
   BOOST_CHECK_EQUAL( default_window, sub_bill.get_expired_accumulator_average_window() );
   BOOST_CHECK_EQUAL( 0u, sub_bill.get_account_cache_size() );
   BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
}

BOOST_AUTO_TEST_CASE( subjective_billing_integration_test ) {
   tester chain( setup_policy::full, db_read_mode::SPECULATIVE );

   fc::logger log;
   account_name acc = "asserter"_n;
   account_name user = "user"_n;
   account_name other = "other"_n;
   chain.create_accounts( {acc, user} );
   chain.create_account( other, config::system_account_name, false, false, false, true );
   chain.set_contract(acc, test_contracts::asserter_wasm(), test_contracts::asserter_abi());
   auto b = chain.produce_block();

   chain.push_action( config::system_account_name, "setalimits"_n, config::system_account_name, fc::mutable_variant_object()
         ("account", other)
         ("ram_bytes", base_tester::newaccount_ram)
         ("net_weight", 0) // no NET
         ("cpu_weight", 0) // no CPU
   );

   // assertdef_v values of 1 indicate it does not assert, 0 means it asserts
   auto create_trx = [&](auto trx_max_ms, std::array<signed char, 4> assertdef_v) {
      signed_transaction trx;
      trx.actions.emplace_back( vector<permission_level>{{acc, config::active_name}}, // paid by contract
                                assertdef {assertdef_v[0], std::to_string(assertdef_v[0])} );
      trx.actions.emplace_back( vector<permission_level>{{user, config::active_name}}, // also paid by contract
                                assertdef {assertdef_v[1], std::to_string(assertdef_v[1])} );
      trx.actions.emplace_back( vector<permission_level>{{user, config::sysio_payer_name},{user, config::active_name}}, // paid by user
                                assertdef {assertdef_v[2], std::to_string(assertdef_v[2])} );
      trx.actions.emplace_back( vector<permission_level>{{other, config::active_name}}, // paid by contract
                                assertdef {assertdef_v[3], std::to_string(assertdef_v[3])} );
      static int num_secs = 1;
      chain.set_transaction_headers( trx, ++num_secs ); // num_secs provides nonce
      trx.max_cpu_usage_ms = trx_max_ms;
      trx.sign( tester::get_private_key( acc, "active" ), chain.control->get_chain_id() );
      trx.sign( tester::get_private_key( user, "active" ), chain.control->get_chain_id() );
      trx.sign( tester::get_private_key( other, "active" ), chain.control->get_chain_id() );
      auto ptrx = std::make_shared<packed_transaction>(trx);
      auto fut = transaction_metadata::start_recover_keys( ptrx, chain.control->get_thread_pool(), chain.control->get_chain_id(), fc::microseconds::maximum(), transaction_metadata::trx_type::input );
      return fut.get();
   };

   auto push_trx = [&]( const transaction_metadata_ptr& trx, fc::time_point deadline,
                     const cpu_usage_t& billed_cpu_us, bool explicit_billed_cpu_time ) {
      auto r = chain.control->test_push_transaction( trx, deadline, fc::microseconds::maximum(), billed_cpu_us, explicit_billed_cpu_time );
      if( r->except_ptr ) std::rethrow_exception( r->except_ptr );
      return r;
   };

   auto& sub_bill = chain.control->get_mutable_subjective_billing();
   sub_bill.set_disabled(false);
   BOOST_TEST(sub_bill.get_subjective_bill(acc, b->timestamp).count() == 0);
   BOOST_TEST(sub_bill.get_subjective_bill(user, b->timestamp).count() == 0);
   BOOST_TEST(sub_bill.get_subjective_bill(other, b->timestamp).count() == 0);

   auto ptrx = create_trx(0, {0,1,1,1});
   BOOST_CHECK_THROW(push_trx( ptrx, fc::time_point::maximum(), {}, false ), sysio_assert_message_exception);

   BOOST_TEST(sub_bill.get_subjective_bill(acc, b->timestamp).count() > 0);
   BOOST_TEST(sub_bill.get_subjective_bill(user, b->timestamp).count() == 0);
   BOOST_TEST(sub_bill.get_subjective_bill(other, b->timestamp).count() == 0);

   chain.produce_block();
   b = chain.produce_block( fc::days(1) ); // produce for one day to reset account cpu/net
   sub_bill.on_block(log, b, b->timestamp);
   BOOST_TEST(sub_bill.get_subjective_bill(acc, b->timestamp).count() == 0);
   BOOST_TEST(sub_bill.get_subjective_bill(user, b->timestamp).count() == 0);
   BOOST_TEST(sub_bill.get_subjective_bill(other, b->timestamp).count() == 0);

   ptrx = create_trx(0, {1,0,1,1});
   BOOST_CHECK_THROW(push_trx( ptrx, fc::time_point::maximum(), {}, false ), sysio_assert_message_exception);

   BOOST_TEST(sub_bill.get_subjective_bill(acc, b->timestamp).count() > 0);
   BOOST_TEST(sub_bill.get_subjective_bill(user, b->timestamp).count() > 0);
   BOOST_TEST(sub_bill.get_subjective_bill(other, b->timestamp).count() == 0); // didn't make it to the final action

   chain.produce_block();
   b = chain.produce_block( fc::days(1) ); // produce for one day to reset account cpu/net
   sub_bill.on_block(log, b, b->timestamp);
   BOOST_TEST(sub_bill.get_subjective_bill(acc, b->timestamp).count() == 0);
   BOOST_TEST(sub_bill.get_subjective_bill(user, b->timestamp).count() == 0);
   BOOST_TEST(sub_bill.get_subjective_bill(other, b->timestamp).count() == 0);

   ptrx = create_trx(0, {1,1,0,1});
   BOOST_CHECK_THROW(push_trx( ptrx, fc::time_point::maximum(), {}, false ), sysio_assert_message_exception);

   BOOST_TEST(sub_bill.get_subjective_bill(acc, b->timestamp).count() > 0);
   BOOST_TEST(sub_bill.get_subjective_bill(user, b->timestamp).count() > 0);
   BOOST_TEST(sub_bill.get_subjective_bill(other, b->timestamp).count() == 0); // didn't make it to the final action

   chain.produce_block();
   b = chain.produce_block( fc::days(1) ); // produce for one day to reset account cpu/net
   sub_bill.on_block(log, b, b->timestamp);
   BOOST_TEST(sub_bill.get_subjective_bill(acc, b->timestamp).count() == 0);
   BOOST_TEST(sub_bill.get_subjective_bill(user, b->timestamp).count() == 0);
   BOOST_TEST(sub_bill.get_subjective_bill(other, b->timestamp).count() == 0);

   ptrx = create_trx(0, {1,1,1,0});
   BOOST_CHECK_THROW(push_trx( ptrx, fc::time_point::maximum(), {}, false ), sysio_assert_message_exception);

   BOOST_TEST(sub_bill.get_subjective_bill(acc, b->timestamp).count() > 0);
   BOOST_TEST(sub_bill.get_subjective_bill(user, b->timestamp).count() > 0);
   BOOST_TEST(sub_bill.get_subjective_bill(other, b->timestamp).count() > 0);

   sub_bill.set_subjective_account_cpu_allowed(fc::microseconds{1000});
   // Push failing transactions until other's subjective bill reaches the threshold.
   // Check at the top of each iteration so we never push when billing has already
   // crossed the limit; doing so would throw tx_cpu_usage_exceeded instead of
   // sysio_assert_message_exception and trip the BOOST_CHECK_THROW below.
   // update_billed_cpu_time always contributes >= 1 us to other per transaction
   // (delta_per_action = (remaining / num_actions) + 1, minimum = 1), so the
   // threshold of 1000 us is reached in at most 1000 iterations; 5000 gives 5x
   // headroom for any rounding variation across runtimes and sanitizer builds.
   const size_t num_itrs = 5000;
   size_t i = 0;
   for (; i < num_itrs; ++i) {
      if (sub_bill.get_subjective_bill(other, b->timestamp) >= sub_bill.get_subjective_account_cpu_allowed())
         break;
      ptrx = create_trx(0, {1,1,1,0});
      BOOST_CHECK_THROW(push_trx( ptrx, fc::time_point::maximum(), {}, false ), sysio_assert_message_exception);
   }
   BOOST_REQUIRE_MESSAGE(i < num_itrs, "other's subjective billing failed to reach the threshold");
   ptrx = create_trx(0, {1,1,1,1});
   BOOST_CHECK_EXCEPTION(push_trx( ptrx, fc::time_point::maximum(), {}, false ), tx_cpu_usage_exceeded,
                         fc_exception_message_contains("Authorized account other exceeded subjective CPU limit"));
}

/// A transaction of `actions`; `nonce` makes its id unique.
signed_transaction make_trx( vector<action> actions, uint16_t nonce ) {
   signed_transaction trx;
   trx.ref_block_num = nonce;
   trx.actions = std::move( actions );
   return trx;
}

/// An action of `code`; billing reads only its authorizations.
action make_action( account_name code, vector<permission_level> auth ) {
   return action( std::move( auth ), code, "transfer"_n, bytes{} );
}

/// A block at `time` with one receipt per transaction, billing `cpu_us[i]` to each one's i-th action.
signed_block_ptr make_block( fc::time_point time,
                             const vector<std::pair<signed_transaction, vector<uint32_t>>>& trxs ) {
   auto block = std::make_shared<signed_block>();
   block->timestamp = block_timestamp_type{time};
   for( const auto& [trx, cpu_us] : trxs ) {
      BOOST_REQUIRE_EQUAL( cpu_us.size(), trx.total_actions() );
      auto& receipt = block->transactions.emplace_back( packed_transaction{trx} );
      for( uint32_t cpu : cpu_us ) receipt.cpu_usage_us.emplace_back( cpu );
   }
   return block;
}

// A year after the block timestamp epoch, on a 5 s subjective billing interval boundary, so block timestamps and
// `now` share an ordinal.
const fc::time_point sysio_paid_now = fc::time_point{fc::milliseconds( config::block_timestamp_epoch )} + fc::days( 365 );

/// The time over which a charge decays to nothing.
fc::microseconds decay_window( const subjective_billing& sub_bill ) {
   return fc::milliseconds( int64_t{sub_bill.get_expired_accumulator_average_window()} *
                            subjective_billing::subjective_time_interval_ms );
}

BOOST_AUTO_TEST_CASE( sysio_paid_authorizer_billing_test ) {
   fc::logger log;
   const account_name sysio_token = "sysio.token"_n;
   const account_name alice = "alice"_n; // calls a sysio.* contract
   const account_name bob = "bob"_n;     // calls sysio itself
   const account_name carol = "carol"_n; // pays explicitly with sysio.payer
   const account_name dave = "dave"_n;   // calls a contract outside sysio.*
   const auto now = sysio_paid_now;

   subjective_billing sub_bill;
   const auto window = decay_window( sub_bill );
   const auto halftime = now + fc::microseconds( window.count() / 2 );
   const auto endtime = now + window;

   const auto block = make_block( now, {
      { make_trx( {make_action( sysio_token, {{alice, config::active_name}} )}, 1 ), {1000} },
      { make_trx( {make_action( config::system_account_name, {{bob, config::active_name}} )}, 2 ), {1000} },
      { make_trx( {make_action( sysio_token, {{carol, config::sysio_payer_name}, {carol, config::active_name}} )}, 3 ),
        {1000} },
      { make_trx( {make_action( "token"_n, {{dave, config::active_name}} )}, 4 ), {1000} },
   } );
   sub_bill.on_block( log, block, now );

   BOOST_TEST( sub_bill.get_subjective_bill( alice, now ).count() == 1000 );
   BOOST_TEST( sub_bill.get_subjective_bill( bob, now ).count() == 1000 );
   BOOST_TEST( sub_bill.get_subjective_bill( carol, now ).count() == 0 ); // the payer, billed objectively
   BOOST_TEST( sub_bill.get_subjective_bill( dave, now ).count() == 0 );  // the contract has its own CPU limit
   BOOST_TEST( sub_bill.get_subjective_bill( sysio_token, now ).count() == 0 );

   // The charge outlasts the block and decays over the window.
   sub_bill.on_block( log, make_block( now, {} ), now );
   BOOST_TEST( sub_bill.get_subjective_bill( alice, now ).count() == 1000 );
   BOOST_TEST( sub_bill.get_subjective_bill( alice, halftime ).count() == 500 );
   BOOST_TEST( sub_bill.get_subjective_bill( alice, endtime ).count() == 0 );

   // Each block adds to the decayed total.
   sub_bill.on_block( log, make_block( halftime, {
      { make_trx( {make_action( sysio_token, {{alice, config::active_name}} )}, 5 ), {1000} },
   } ), halftime );
   BOOST_TEST( sub_bill.get_subjective_bill( alice, halftime ).count() == 500 + 1000 );
}

BOOST_AUTO_TEST_CASE( sysio_paid_authorizer_billing_actions_test ) {
   fc::logger log;
   const account_name sysio_token = "sysio.token"_n;
   const account_name alice = "alice"_n;
   const account_name erin = "erin"_n;
   const auto now = sysio_paid_now;

   subjective_billing sub_bill;
   signed_transaction trx = make_trx( {
      make_action( sysio_token, {} ),                                                                // 5 us to alice
      make_action( sysio_token, {{alice, config::active_name}} ),                                   // 20 us to alice
      make_action( sysio_token, {{erin, config::sysio_payer_name}, {erin, config::active_name}} ),   // erin pays
      make_action( "token"_n, {{alice, config::active_name}} ),                                      // token pays
      make_action( sysio_token, {{erin, config::active_name}} ),                                     // 160 us to erin
   }, 1 );
   // An action without authorization, like a context-free action, bills the transaction's first authorizer.
   trx.context_free_actions.emplace_back( make_action( sysio_token, {} ) );                          // 10 us to alice

   sub_bill.on_block( log, make_block( now, {{ trx, {10, 5, 20, 40, 80, 160} }} ), now );
   BOOST_TEST( sub_bill.get_subjective_bill( alice, now ).count() == 10 + 5 + 20 );
   BOOST_TEST( sub_bill.get_subjective_bill( erin, now ).count() == 160 );
}

BOOST_AUTO_TEST_CASE( sysio_paid_authorizer_billing_replaces_pending_test ) {
   fc::logger log;
   const account_name alice = "alice"_n;
   const auto now = sysio_paid_now;
   const signed_transaction trx = make_trx( {make_action( "sysio.token"_n, {{alice, config::active_name}} )}, 1 );

   // Executed here first: pending until the block, then charged once from the block's receipt.
   subjective_billing sub_bill;
   sub_bill.subjective_bill( trx.id(), trx.expiration, {}, {{alice, fc::microseconds( 800 )}} );
   BOOST_TEST( sub_bill.get_subjective_bill( alice, now ).count() == 800 );
   sub_bill.on_block( log, make_block( now, {{ trx, {1200} }} ), now );
   BOOST_TEST( sub_bill.get_subjective_bill( alice, now ).count() == 1200 );

   // With the charge disabled, inclusion still drops the pending bill.
   subjective_billing disabled_bill;
   disabled_bill.disable_sysio_authorizer_billing( true );
   disabled_bill.subjective_bill( trx.id(), trx.expiration, {}, {{alice, fc::microseconds( 800 )}} );
   disabled_bill.on_block( log, make_block( now, {{ trx, {1200} }} ), now );
   BOOST_TEST( disabled_bill.get_subjective_bill( alice, now ).count() == 0 );
   BOOST_TEST( disabled_bill.get_account_cache_size() == 0u );
}

BOOST_AUTO_TEST_CASE( sysio_paid_authorizer_billing_block_age_test ) {
   fc::logger log;
   const account_name alice = "alice"_n;
   const auto now = sysio_paid_now;
   const auto block = make_block( now, {
      { make_trx( {make_action( "sysio.token"_n, {{alice, config::active_name}} )}, 1 ), {1000} },
   } );

   subjective_billing live_bill;
   const auto window = decay_window( live_bill );
   const auto half = fc::microseconds( window.count() / 2 );
   const auto three_quarters = fc::microseconds( window.count() * 3 / 4 );
   live_bill.on_block( log, block, now );

   // A node applying the block half a window late, e.g. while syncing, holds the same charge from then on as the
   // node that applied it live: the charge decays from the block's time.
   subjective_billing late_bill;
   late_bill.on_block( log, block, now + half );
   BOOST_TEST( late_bill.get_subjective_bill( alice, now + half ).count() == 500 );
   for( const auto time : {now + half, now + three_quarters, now + window} ) {
      BOOST_TEST( late_bill.get_subjective_bill( alice, time ).count() ==
                  live_bill.get_subjective_bill( alice, time ).count() );
   }
   BOOST_TEST( late_bill.get_subjective_bill( alice, now + window ).count() == 0 );

   // A charge older than the account's last update is decayed to that update first.
   subjective_billing updated_bill;
   updated_bill.subjective_bill_failure( {}, {{alice, fc::microseconds( 1000 )}}, now + half );
   updated_bill.on_block( log, block, now + half );
   BOOST_TEST( updated_bill.get_subjective_bill( alice, now + half ).count() == 1000 + 500 );

   // A block older than the decay window charges nothing.
   subjective_billing old_bill;
   old_bill.on_block( log, block, now + window );
   BOOST_TEST( old_bill.get_subjective_bill( alice, now + window ).count() == 0 );
   BOOST_TEST( old_bill.get_account_cache_size() == 0u );
}

BOOST_AUTO_TEST_CASE( sysio_paid_authorizer_billing_disabled_test ) {
   fc::logger log;
   const account_name alice = "alice"_n;
   const auto now = sysio_paid_now;
   const auto block = make_block( now, {
      { make_trx( {make_action( "sysio.token"_n, {{alice, config::active_name}} )}, 1 ), {1000} },
   } );

   subjective_billing account_disabled;
   account_disabled.disable_account( alice );
   account_disabled.on_block( log, block, now );
   BOOST_TEST( account_disabled.get_account_cache_size() == 0u );

   subjective_billing all_disabled;
   all_disabled.set_disabled( true );
   all_disabled.on_block( log, block, now );
   all_disabled.set_disabled( false );
   BOOST_TEST( all_disabled.get_subjective_bill( alice, now ).count() == 0 );

   subjective_billing charge_disabled;
   charge_disabled.disable_sysio_authorizer_billing( true );
   BOOST_TEST( charge_disabled.is_sysio_authorizer_billing_disabled() );
   charge_disabled.on_block( log, block, now );
   BOOST_TEST( charge_disabled.get_subjective_bill( alice, now ).count() == 0 );

   charge_disabled.reset();
   BOOST_TEST( !charge_disabled.is_sysio_authorizer_billing_disabled() );
}

BOOST_AUTO_TEST_CASE( prune_decayed_accounts_test ) {
   fc::logger log;
   const account_name alice = "alice"_n;
   const account_name bob = "bob"_n;
   const auto now = sysio_paid_now;
   transaction_id_type pending_id = sha256::hash( "pending" );

   subjective_billing sub_bill;
   const auto window = decay_window( sub_bill );
   const auto endtime = now + window;

   sub_bill.on_block( log, make_block( now, {
      { make_trx( {make_action( "sysio.token"_n, {{alice, config::active_name}} )}, 1 ), {1000} },
   } ), now );
   sub_bill.subjective_bill( pending_id, fc::time_point_sec{endtime + fc::days( 1 )}, {},
                             {{bob, fc::microseconds( 50 )}} );
   BOOST_TEST( sub_bill.get_account_cache_size() == 2u );

   // alice's charge has decayed away; bob still has a pending bill.
   sub_bill.on_block( log, make_block( endtime, {} ), endtime );
   BOOST_TEST( sub_bill.get_account_cache_size() == 1u );
   BOOST_TEST( sub_bill.get_subjective_bill( bob, endtime ).count() == 50 );
}

BOOST_AUTO_TEST_CASE( sysio_paid_authorizer_billing_integration_test ) {
   validating_tester chain;

   fc::logger log;
   const account_name sysio_contract = "sysio.assert"_n; // a sysio.* contract, with unlimited CPU
   const account_name contract = "asserter"_n;           // a contract outside sysio.*
   const account_name user = "user"_n;                   // no CPU of its own
   const account_name payer = "payer"_n;                 // has CPU and pays with sysio.payer
   chain.create_accounts( {sysio_contract, contract, payer} );
   chain.create_account( user, config::system_account_name, false, false, false, true );
   for( const auto& [account, ram, weight] : {std::tuple{sysio_contract, int64_t{-1}, int64_t{-1}},
                                              std::tuple{user, int64_t{base_tester::newaccount_ram}, int64_t{0}}} ) {
      chain.push_action( config::system_account_name, "setalimits"_n, config::system_account_name,
                         fc::mutable_variant_object()("account", account)("ram_bytes", ram)
                                                     ("net_weight", weight)("cpu_weight", weight) );
   }
   chain.set_contract( sysio_contract, test_contracts::asserter_wasm(), test_contracts::asserter_abi() );
   chain.set_contract( contract, test_contracts::asserter_wasm(), test_contracts::asserter_abi() );
   chain.produce_block();

   auto& sub_bill = chain.control->get_mutable_subjective_billing();
   auto& validator_bill = chain.validating_node->get_mutable_subjective_billing();
   sub_bill.set_disabled( false );
   validator_bill.set_disabled( false );

   // Pushes a successful assertion on `code`, then produces and applies a block holding it on both nodes:
   // `chain` produced it, the validating node received it. Returns the CPU its receipt billed.
   uint32_t nonce = 0;
   auto push_and_apply = [&]( account_name code, vector<permission_level> auth ) {
      signed_transaction trx;
      trx.actions.emplace_back( std::move( auth ), code, assertdef::get_name(), fc::raw::pack( assertdef{1, "1"} ) );
      chain.set_transaction_headers( trx, ++nonce );
      for( account_name signer : trx.first_authorizers() )
         trx.sign( tester::get_private_key( signer, "active" ), chain.control->get_chain_id() );
      auto ptrx = std::make_shared<packed_transaction>( trx );
      auto meta = transaction_metadata::start_recover_keys( ptrx, chain.control->get_thread_pool(),
                                                            chain.control->get_chain_id(), fc::microseconds::maximum(),
                                                            transaction_metadata::trx_type::input ).get();
      auto trace = chain.control->test_push_transaction( meta, fc::time_point::maximum(), fc::microseconds::maximum(),
                                                         {}, false );
      if( trace->except_ptr ) std::rethrow_exception( trace->except_ptr );
      const auto block = chain.produce_block();
      sub_bill.on_block( log, block, block->timestamp );
      validator_bill.on_block( log, block, block->timestamp );
      const auto receipt = std::ranges::find_if( block->transactions, [&]( const transaction_receipt& r ) {
         return r.trx.id() == trx.id();
      } );
      BOOST_REQUIRE( receipt != block->transactions.end() );
      return receipt->cpu_usage_us.front().value;
   };
   const auto bill_of = [&]( const subjective_billing& bill, account_name account ) {
      return bill.get_subjective_bill( account, chain.control->head().block_time() ).count();
   };

   // A contract outside sysio.* pays from its own CPU, and an explicit sysio.payer pays from the payer's.
   push_and_apply( contract, {{user, config::active_name}} );
   push_and_apply( sysio_contract, {{payer, config::sysio_payer_name}, {payer, config::active_name}} );
   BOOST_TEST( bill_of( sub_bill, user ) == 0 );
   BOOST_TEST( bill_of( sub_bill, payer ) == 0 );
   BOOST_TEST( bill_of( validator_bill, payer ) == 0 );

   // A sysio.*-paid action stays charged to its authorizer on the producing and on the receiving node.
   const uint32_t first_cpu = push_and_apply( sysio_contract, {{user, config::active_name}} );
   BOOST_TEST( first_cpu > 0u );
   BOOST_TEST( bill_of( sub_bill, user ) == first_cpu );
   BOOST_TEST( bill_of( validator_bill, user ) == first_cpu );
   const auto empty_block = chain.produce_block();
   sub_bill.on_block( log, empty_block, empty_block->timestamp );
   BOOST_TEST( bill_of( sub_bill, user ) == first_cpu );

   // Once included charges exceed the budget, the authorizer is rejected before execution.
   sub_bill.set_subjective_account_cpu_allowed( fc::microseconds{bill_of( sub_bill, user ) * 4} );
   const size_t max_pushes = 1000;
   size_t pushes = 0;
   for( ; pushes < max_pushes && bill_of( sub_bill, user ) < sub_bill.get_subjective_account_cpu_allowed().count();
        ++pushes ) {
      push_and_apply( sysio_contract, {{user, config::active_name}} );
   }
   BOOST_REQUIRE( pushes < max_pushes );
   BOOST_CHECK_EXCEPTION( push_and_apply( sysio_contract, {{user, config::active_name}} ), tx_cpu_usage_exceeded,
                          fc_exception_message_contains( "Authorized account user exceeded subjective CPU limit" ) );

   // The charge decays back over the window, and the authorizer is accepted again.
   chain.produce_block( fc::days( 1 ) );
   BOOST_TEST( bill_of( sub_bill, user ) == 0 );
   push_and_apply( sysio_contract, {{user, config::active_name}} );
}

BOOST_AUTO_TEST_SUITE_END()

}
