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

   {  // Removal via block inclusion
      subjective_billing sub_bill;

      sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 10}}}, {{a, fc::microseconds(5)}} );

      BOOST_CHECK_EQUAL( 10 + 5, sub_bill.get_subjective_bill(a, now).count() );

      // Simulate id1 landing in a block.
      sub_bill.remove_subjective_billing( id1, 0 );

      // Full contribution must be cleared; with the overlap bug only 5 would be
      // subtracted, leaving 10 stuck in the cache.
      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
   }
   {  // Same check, but with auth_cpu listed first — verify order-independence.
      subjective_billing sub_bill;

      sub_bill.subjective_bill( id1, now_sec, {{a, {.cpu_usage_us = 7}}}, {{a, fc::microseconds(3)}} );

      BOOST_CHECK_EQUAL( 7 + 3, sub_bill.get_subjective_bill(a, now).count() );

      sub_bill.remove_subjective_billing( id1, 0 );

      BOOST_CHECK_EQUAL( 0, sub_bill.get_subjective_bill(a, now).count() );
   }
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
      if( r->except ) throw *r->except;
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
   const size_t num_itrs = 1000;
   size_t i = 0;
   for (; i < num_itrs; ++i) {
      ptrx = create_trx(0, {1,1,1,0});
      BOOST_CHECK_THROW(push_trx( ptrx, fc::time_point::maximum(), {}, false ), sysio_assert_message_exception);
      if (sub_bill.get_subjective_bill(other, b->timestamp) >= sub_bill.get_subjective_account_cpu_allowed())
         break;
   }
   BOOST_REQUIRE(i < num_itrs); // failed to accumulate subjective billing
   ptrx = create_trx(0, {1,1,1,1});
   BOOST_CHECK_EXCEPTION(push_trx( ptrx, fc::time_point::maximum(), {}, false ), tx_cpu_usage_exceeded,
                         fc_exception_message_contains("Authorized account other exceeded subjective CPU limit"));
}

BOOST_AUTO_TEST_SUITE_END()

}
