#include <fc/scoped_exit.hpp>
#include <fc/thread_stack.hpp>
#include <fc/thread_utils.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdio>
#include <future>
#include <sstream>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

namespace {

/// Locals of the recursive function in `deep_recursion_wast`.
constexpr int live_locals = 2000;
/// Stack, beyond the platform default, requested to show a pool applies its stack size.
constexpr std::size_t extra_stack_bytes = 1024 * 1024;
/// Linear memory address holding the recursion limit.
constexpr int limit_address = 65528;
/// Size of an i32 in linear memory.
constexpr int i32_bytes = 4;

/// A contract whose action recurses until depth `limit`, reading `limit` from memory.
std::string deep_recursion_wast(uint32_t limit) {
   std::ostringstream w;
   w << "(module\n (memory 1)\n (data (i32.const " << limit_address << ") \"";
   for (int i = 0; i < i32_bytes; ++i) {
      char byte[8];
      std::snprintf(byte, sizeof(byte), "\\%02x", (limit >> (8 * i)) & 0xffu);
      w << byte;
   }
   w << "\")\n (func $recurse (param i32)\n  (local";
   for (int i = 0; i <= live_locals; ++i)
      w << " i32";
   w << ")\n";
   for (int i = 1; i <= live_locals; ++i)
      w << "  (set_local " << i << " (i32.load (i32.const " << (i * i32_bytes) % limit_address << ")))\n";
   w << "  (if (i32.lt_u (get_local 0) (i32.load (i32.const " << limit_address << ")))\n"
     << "   (then (call $recurse (i32.add (get_local 0) (i32.const 1)))))\n";
   const int sum = live_locals + 1;
   for (int i = 1; i <= live_locals; ++i)
      w << "  (set_local " << sum << " (i32.add (get_local " << sum << ") (get_local " << i << ")))\n";
   w << "  (i32.store (i32.const 0) (get_local " << sum << "))\n )\n"
     << " (func (export \"apply\") (param i64 i64 i64) (call $recurse (i32.const 0)))\n)\n";
   return w.str();
}

/// Runs `f` on the thread of a one-thread pool started with `stack_size`, and returns its result.
template <typename F>
auto run_on_pool_thread(std::optional<std::size_t> stack_size, F&& f) {
   named_thread_pool<struct contract> pool;
   pool.start(1, {}, {}, stack_size);
   auto stop = fc::make_scoped_exit([&pool]() { pool.stop(); });
   return post_async_task(pool.get_executor(), std::forward<F>(f)).get();
}

/// Runs `f` on a thread of a pool started the way producer_plugin starts its read-only threads, and returns its result.
template <typename F>
auto run_on_contract_thread(F&& f) {
   return run_on_pool_thread(config::contract_thread_stack_size, std::forward<F>(f));
}

} // namespace

BOOST_AUTO_TEST_SUITE(contract_thread_stack_tests)

/// On a thread of config::contract_thread_stack_size, the contract succeeds at the chain's call depth limit and fails
/// with wasm_execution_error one call deeper.
BOOST_AUTO_TEST_CASE(deepest_recursion_fits_contract_thread_stack) try {
   // Runs the contract recursing `past_deepest` calls deeper than the chain's max_call_depth allows.
   auto push = [](uint32_t past_deepest) {
      return run_on_contract_thread([past_deepest]() -> std::optional<fc::exception> {
         tester chain;
         // apply and the first $recurse take two of the depth
         const uint32_t deepest_limit = chain.control->get_global_properties().wasm_configuration.max_call_depth - 2;
         chain.create_account("deep"_n);
         chain.set_code("deep"_n, deep_recursion_wast(deepest_limit + past_deepest).c_str());
         chain.produce_block();
         signed_transaction trx;
         trx.actions.push_back({{{"deep"_n, config::active_name}}, "deep"_n, ""_n, bytes()});
         chain.set_transaction_headers(trx);
         trx.sign(chain.get_private_key("deep"_n, "active"), chain.get_chain_id());
         constexpr bool no_throw = true;
         const transaction_trace_ptr trace =
            chain.push_transaction(trx, fc::time_point::maximum(), base_tester::DEFAULT_BILLED_CPU_TIME_US, no_throw);
         if (trace->except)
            return *trace->except;
         return {};
      });
   };

   const std::optional<fc::exception> at_limit = push(0);
   BOOST_CHECK_MESSAGE(!at_limit, "deepest call chain failed: " << (at_limit ? at_limit->to_string() : std::string{}));

   const std::optional<fc::exception> past_limit = push(1);
   BOOST_REQUIRE(past_limit);
   BOOST_TEST(past_limit->code() == wasm_execution_error::code_value);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(thread_stack_size_tests)

/// A pool started with a stack size gives each thread at least that much stack, including more than the platform
/// default.
BOOST_AUTO_TEST_CASE(named_thread_pool_uses_requested_stack_size) try {
   const std::size_t platform_default = run_on_pool_thread({}, []() { return fc::current_thread_stack_size(); });
   const std::size_t requested = std::max(platform_default, config::contract_thread_stack_size) + extra_stack_bytes;
   const std::size_t stack = run_on_pool_thread(requested, []() { return fc::current_thread_stack_size(); });
   BOOST_TEST(stack >= requested);
   BOOST_TEST(run_on_contract_thread([]() { return fc::current_thread_stack_size(); }) >=
              config::contract_thread_stack_size);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
