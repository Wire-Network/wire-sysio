#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] test_api_multi_index : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action("s1g")]]
   void idx64_general();

   [[sysio::action("s1store")]]
   void idx64_store_only();

   [[sysio::action("s1check")]]
   void idx64_check_without_storing();

   [[sysio::action("s1findfail1")]]
   void idx64_require_find_fail();

   [[sysio::action("s1findfail2")]]
   void idx64_require_find_fail_with_msg();

   [[sysio::action("s1findfail3")]]
   void idx64_require_find_sk_fail();

   [[sysio::action("s1findfail4")]]
   void idx64_require_find_sk_fail_with_msg();

   [[sysio::action("s1pkend")]]
   void idx64_pk_iterator_exceed_end();

   [[sysio::action("s1skend")]]
   void idx64_sk_iterator_exceed_end();

   [[sysio::action("s1pkbegin")]]
   void idx64_pk_iterator_exceed_begin();

   [[sysio::action("s1skbegin")]]
   void idx64_sk_iterator_exceed_begin();

   [[sysio::action("s1pkref")]]
   void idx64_pass_pk_ref_to_other_table();

   [[sysio::action("s1skref")]]
   void idx64_pass_sk_ref_to_other_table();

   [[sysio::action("s1pkitrto")]]
   void idx64_pass_pk_end_itr_to_iterator_to();

   [[sysio::action("s1pkmodify")]]
   void idx64_pass_pk_end_itr_to_modify();

   [[sysio::action("s1pkerase")]]
   void idx64_pass_pk_end_itr_to_erase();

   [[sysio::action("s1skitrto")]]
   void idx64_pass_sk_end_itr_to_iterator_to();

   [[sysio::action("s1skmodify")]]
   void idx64_pass_sk_end_itr_to_modify();

   [[sysio::action("s1skerase")]]
   void idx64_pass_sk_end_itr_to_erase();

   [[sysio::action("s1modpk")]]
   void idx64_modify_primary_key();

   [[sysio::action("s1exhaustpk")]]
   void idx64_run_out_of_avl_pk();

   [[sysio::action("s1skcache")]]
   void idx64_sk_cache_pk_lookup();

   [[sysio::action("s1pkcache")]]
   void idx64_pk_cache_sk_lookup();

   [[sysio::action("s2g")]]
   void idx128_general();

   [[sysio::action("s2store")]]
   void idx128_store_only();

   [[sysio::action("s2check")]]
   void idx128_check_without_storing();

   [[sysio::action("s2autoinc")]]
   void idx128_autoincrement_test();

   [[sysio::action("s2autoinc1")]]
   void idx128_autoincrement_test_part1();

   [[sysio::action("s2autoinc2")]]
   void idx128_autoincrement_test_part2();

   [[sysio::action("s3g")]]
   void idx256_general();

   [[sysio::action("sdg")]]
   void idx_double_general();

   [[sysio::action("sldg")]]
   void idx_long_double_general();

};
