#pragma once
#include <sysio/testing/tester.hpp>

struct dummy_action {
   static sysio::chain::name get_name() {
      using namespace sysio::chain::literals;
      return "dummyaction"_n;
   }
   static sysio::chain::name get_account() {
      using namespace sysio::chain::literals;
      return "testapi"_n;
   }

   char     a; // 1
   uint64_t b; // 8
   int32_t  c; // 4
};

struct cf_action {
   static sysio::chain::name get_name() {
      using namespace sysio::chain::literals;
      return "cfaction"_n;
   }
   static sysio::chain::name get_account() {
      using namespace sysio::chain::literals;
      return "testapi"_n;
   }

   uint32_t payload = 100;
   uint32_t cfd_idx = 0; // context free data index
};

FC_REFLECT(dummy_action, (a)(b)(c))
FC_REFLECT(cf_action, (payload)(cfd_idx))

#define DUMMY_ACTION_DEFAULT_A 0x45
#define DUMMY_ACTION_DEFAULT_B 0xab11cd1244556677
#define DUMMY_ACTION_DEFAULT_C 0x7451ae12

std::vector<sysio::chain::signed_block_ptr> deploy_test_api(sysio::testing::tester& chain);
sysio::chain::transaction_trace_ptr push_test_cfd_transaction(sysio::testing::tester& chain);