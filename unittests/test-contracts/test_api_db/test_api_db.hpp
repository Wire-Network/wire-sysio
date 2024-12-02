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

};
