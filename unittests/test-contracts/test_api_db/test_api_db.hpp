#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] test_api_db : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action("pg")]]
   void primary_i64_general();

   [[sysio::action("pl")]]
   void primary_i64_lowerbound();

   [[sysio::action("pu")]]
   void primary_i64_upperbound();

   [[sysio::action("s1g")]]
   void idx64_general();

   [[sysio::action("s1l")]]
   void idx64_lowerbound();

   [[sysio::action("s1u")]]
   void idx64_upperbound();

   [[sysio::action("tia")]]
   void test_invalid_access( sysio::name code, uint64_t val, uint32_t index, bool store );

   [[sysio::action("sdnancreate")]]
   void idx_double_nan_create_fail();

   [[sysio::action("sdnanmodify")]]
   void idx_double_nan_modify_fail();

   [[sysio::action("sdnanlookup")]]
   void idx_double_nan_lookup_fail( uint32_t lookup_type );

   [[sysio::action("sk32align")]]
   void misaligned_secondary_key256_tests();

   // --- Comprehensive secondary index tests ---

   [[sysio::action("s2g")]]
   void idx128_general();

   [[sysio::action("s2l")]]
   void idx128_lowerbound();

   [[sysio::action("s2u")]]
   void idx128_upperbound();

   [[sysio::action("s3g")]]
   void idx256_general();

   [[sysio::action("s3l")]]
   void idx256_lowerbound();

   [[sysio::action("s3u")]]
   void idx256_upperbound();

   [[sysio::action("s4g")]]
   void idx_double_general();

   [[sysio::action("s4l")]]
   void idx_double_lowerbound();

   [[sysio::action("s4u")]]
   void idx_double_upperbound();

   [[sysio::action("s5g")]]
   void idx_long_double_general();

   [[sysio::action("s5l")]]
   void idx_long_double_lowerbound();

   [[sysio::action("s5u")]]
   void idx_long_double_upperbound();

   // --- Action I/O and transaction metadata tests ---

   [[sysio::action("actsize")]]
   void test_action_data_size( uint64_t val );

   [[sysio::action("actread")]]
   void test_read_action_data( uint64_t val );

   [[sysio::action("actrecv")]]
   void test_current_receiver();

   [[sysio::action("trxsize")]]
   void test_transaction_size();

   [[sysio::action("trxexp")]]
   void test_expiration();

   [[sysio::action("trxtapos")]]
   void test_tapos();

   [[sysio::action("trxread")]]
   void test_read_transaction();

};
