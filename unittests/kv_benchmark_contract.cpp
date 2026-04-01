#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(kv_benchmark_contract)

#ifndef RUN_PERF_BENCHMARKS
BOOST_AUTO_TEST_CASE(perf_benchmarks_disabled) {
   BOOST_TEST_MESSAGE("KV performance benchmarks disabled. Define RUN_PERF_BENCHMARKS to enable.");
}
#endif

//
// Performance Tests. Enable via RUN_PERF_BENCHMARKS. Take too long for normal test runs.
//

//#define RUN_PERF_BENCHMARKS
#ifdef RUN_PERF_BENCHMARKS

#include <sysio/testing/tester.hpp>
#include <test_contracts.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

static std::string fmt_us(int64_t us) {
   std::ostringstream ss;
   if (us >= 1000) ss << std::fixed << std::setprecision(2) << us / 1000.0 << " ms";
   else ss << us << " us";
   return ss.str();
}

BOOST_AUTO_TEST_CASE(contract_level_benchmark) {
   validating_tester kv_chain;
   validating_tester shim_chain;

   kv_chain.create_accounts({"benchkv"_n});
   kv_chain.produce_block();
   kv_chain.set_code("benchkv"_n, test_contracts::bench_kv_db_wasm());
   kv_chain.set_abi("benchkv"_n, test_contracts::bench_kv_db_abi());
   kv_chain.produce_block();

   shim_chain.create_accounts({"benchshim"_n});
   shim_chain.produce_block();
   shim_chain.set_code("benchshim"_n, test_contracts::bench_kv_shim_wasm());
   shim_chain.set_abi("benchshim"_n, test_contracts::bench_kv_shim_abi());
   shim_chain.produce_block();

   const std::vector<uint32_t> row_counts = {100, 500};

   std::cout << "\n===== Contract-Level Benchmark (WASM end-to-end) =====\n";
   std::cout << std::left << std::setw(20) << "Operation"
             << std::setw(8)  << "Rows"
             << std::setw(14) << "KV Raw"
             << std::setw(14) << "KV Shim"
             << std::setw(10) << "Shim/Raw" << "\n";
   std::cout << std::string(66, '-') << "\n";

   for (auto count : row_counts) {
      auto raw_pop = kv_chain.push_action("benchkv"_n, "populate"_n, "benchkv"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();
      auto shim_pop = shim_chain.push_action("benchshim"_n, "populate"_n, "benchshim"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();
      std::cout << std::setw(20) << "Populate"
                << std::setw(8) << count
                << std::setw(14) << fmt_us(raw_pop)
                << std::setw(14) << fmt_us(shim_pop)
                << std::fixed << std::setprecision(2)
                << std::setw(10) << double(shim_pop) / double(raw_pop) << "x\n";

      kv_chain.produce_block();
      shim_chain.produce_block();

      auto raw_find = kv_chain.push_action("benchkv"_n, "findall"_n, "benchkv"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();
      auto shim_find = shim_chain.push_action("benchshim"_n, "findall"_n, "benchshim"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();
      std::cout << std::setw(20) << "Find All"
                << std::setw(8) << count
                << std::setw(14) << fmt_us(raw_find)
                << std::setw(14) << fmt_us(shim_find)
                << std::fixed << std::setprecision(2)
                << std::setw(10) << double(shim_find) / double(raw_find) << "x\n";

      auto raw_iter = kv_chain.push_action("benchkv"_n, "iterall"_n, "benchkv"_n,
         fc::mutable_variant_object())->elapsed.count();
      auto shim_iter = shim_chain.push_action("benchshim"_n, "iterall"_n, "benchshim"_n,
         fc::mutable_variant_object())->elapsed.count();
      std::cout << std::setw(20) << "Iterate All"
                << std::setw(8) << count
                << std::setw(14) << fmt_us(raw_iter)
                << std::setw(14) << fmt_us(shim_iter)
                << std::fixed << std::setprecision(2)
                << std::setw(10) << double(shim_iter) / double(raw_iter) << "x\n";

      auto raw_upd = kv_chain.push_action("benchkv"_n, "updateall"_n, "benchkv"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();
      auto shim_upd = shim_chain.push_action("benchshim"_n, "updateall"_n, "benchshim"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();
      std::cout << std::setw(20) << "Update All"
                << std::setw(8) << count
                << std::setw(14) << fmt_us(raw_upd)
                << std::setw(14) << fmt_us(shim_upd)
                << std::fixed << std::setprecision(2)
                << std::setw(10) << double(shim_upd) / double(raw_upd) << "x\n";

      kv_chain.produce_block();
      shim_chain.produce_block();

      auto raw_del = kv_chain.push_action("benchkv"_n, "eraseall"_n, "benchkv"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();
      auto shim_del = shim_chain.push_action("benchshim"_n, "eraseall"_n, "benchshim"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();
      std::cout << std::setw(20) << "Erase All"
                << std::setw(8) << count
                << std::setw(14) << fmt_us(raw_del)
                << std::setw(14) << fmt_us(shim_del)
                << std::fixed << std::setprecision(2)
                << std::setw(10) << double(shim_del) / double(raw_del) << "x\n";

      std::cout << std::string(66, '-') << "\n";

      kv_chain.produce_block();
      shim_chain.produce_block();
   }

   std::cout << "=================================================\n\n";
}

BOOST_AUTO_TEST_CASE(token_transfer_benchmark) {
   validating_tester shim_chain;
   validating_tester raw_chain;
   validating_tester fast_chain;

   shim_chain.create_accounts({"shimtoken"_n});
   shim_chain.produce_block();
   shim_chain.set_code("shimtoken"_n, test_contracts::bench_kv_shim_token_wasm());
   shim_chain.set_abi("shimtoken"_n, test_contracts::bench_kv_shim_token_abi());
   shim_chain.produce_block();

   raw_chain.create_accounts({"rawtoken"_n});
   raw_chain.produce_block();
   raw_chain.set_code("rawtoken"_n, test_contracts::bench_kv_token_wasm());
   raw_chain.set_abi("rawtoken"_n, test_contracts::bench_kv_token_abi());
   raw_chain.produce_block();

   fast_chain.create_accounts({"fasttoken"_n});
   fast_chain.produce_block();
   fast_chain.set_code("fasttoken"_n, test_contracts::bench_kv_fast_token_wasm());
   fast_chain.set_abi("fasttoken"_n, test_contracts::bench_kv_fast_token_abi());
   fast_chain.produce_block();

   // Setup: 100 accounts each with 1M balance
   shim_chain.push_action("shimtoken"_n, "setup"_n, "shimtoken"_n,
      fc::mutable_variant_object()("num_accounts", 100));
   shim_chain.produce_block();

   raw_chain.push_action("rawtoken"_n, "setup"_n, "rawtoken"_n,
      fc::mutable_variant_object()("num_accounts", 100));
   raw_chain.produce_block();

   fast_chain.push_action("fasttoken"_n, "setup"_n, "fasttoken"_n,
      fc::mutable_variant_object()("num_accounts", 100));
   fast_chain.produce_block();

   // Warm-up: trigger OC compilation so timed runs use cached native code
   auto oc_pre_shim = shim_chain.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();
   auto oc_pre_raw  = raw_chain.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();
   auto oc_pre_fast = fast_chain.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();

   shim_chain.push_action("shimtoken"_n, "dotransfers"_n, "shimtoken"_n,
      fc::mutable_variant_object()("count", 1));
   raw_chain.push_action("rawtoken"_n, "dotransfers"_n, "rawtoken"_n,
      fc::mutable_variant_object()("count", 1));
   fast_chain.push_action("fasttoken"_n, "dotransfers"_n, "fasttoken"_n,
      fc::mutable_variant_object()("count", 1));
   shim_chain.produce_block();
   raw_chain.produce_block();
   fast_chain.produce_block();

   // Second warm-up pass to ensure OC code is cached (first may have been interrupted)
   shim_chain.push_action("shimtoken"_n, "dotransfers"_n, "shimtoken"_n,
      fc::mutable_variant_object()("count", 1));
   raw_chain.push_action("rawtoken"_n, "dotransfers"_n, "rawtoken"_n,
      fc::mutable_variant_object()("count", 1));
   fast_chain.push_action("fasttoken"_n, "dotransfers"_n, "fasttoken"_n,
      fc::mutable_variant_object()("count", 1));
   shim_chain.produce_block();
   raw_chain.produce_block();
   fast_chain.produce_block();

   auto oc_post_shim = shim_chain.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();
   auto oc_post_raw  = raw_chain.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();
   auto oc_post_fast = fast_chain.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();

   std::cout << "\n===== Token Transfer Benchmark (sysio.token pattern) =====\n";
   std::cout << "OC tier-up interrupts:"
             << " shim=" << (oc_post_shim - oc_pre_shim)
             << " raw=" << (oc_post_raw - oc_pre_raw)
             << " fast=" << (oc_post_fast - oc_pre_fast) << "\n";
   std::cout << "Each transfer = 2 reads + 2 writes (sub_balance + add_balance)\n";
   std::cout << std::left << std::setw(10) << "Xfers"
             << std::setw(12) << "KV Shim"
             << std::setw(12) << "KV Raw"
             << std::setw(12) << "kv::table"
             << std::setw(10) << "Shim/Raw"
             << std::setw(10) << "Fast/Raw" << "\n";
   std::cout << std::string(66, '-') << "\n";

   for (uint32_t count : {10u, 50u, 100u, 500u}) {
      auto shim = shim_chain.push_action("shimtoken"_n, "dotransfers"_n, "shimtoken"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();

      auto raw = raw_chain.push_action("rawtoken"_n, "dotransfers"_n, "rawtoken"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();

      auto fast = fast_chain.push_action("fasttoken"_n, "dotransfers"_n, "fasttoken"_n,
         fc::mutable_variant_object()("count", count))->elapsed.count();

      std::cout << std::setw(10) << count
                << std::setw(12) << fmt_us(shim)
                << std::setw(12) << fmt_us(raw)
                << std::setw(12) << fmt_us(fast)
                << std::fixed << std::setprecision(2)
                << std::setw(10) << (double(shim) / double(raw))
                << std::setw(10) << (double(fast) / double(raw)) << "\n";

      shim_chain.produce_block();
      raw_chain.produce_block();
      fast_chain.produce_block();
   }

   std::cout << "=================================================\n\n";
}

#endif // RUN_PERF_BENCHMARKS

BOOST_AUTO_TEST_SUITE_END()
