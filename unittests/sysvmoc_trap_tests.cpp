#include <sysio/chain/exceptions.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   #include <malloc.h>
#endif

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
namespace {

// One contract per way an action leaves sys-vm-oc through siglongjmp with an exception.

const char unreachable_wast[] = R"=====(
(module
 (func (export "apply") (param i64 i64 i64)
  (unreachable)
 )
)
)=====";

const char div0_wast[] = R"=====(
(module
 (func (export "apply") (param i64 i64 i64)
  (drop (i64.div_u (get_local 0) (i64.const 0)))
 )
)
)=====";

const char indirect_call_oob_wast[] = R"=====(
(module
 (type $void (func))
 (table 1 anyfunc)
 (func (export "apply") (param i64 i64 i64)
  (call_indirect $void (i32.const 1))
 )
)
)=====";

const char indirect_call_mismatch_wast[] = R"=====(
(module
 (type $void (func))
 (type $one_param (func (param i64)))
 (table 1 anyfunc)
 (elem (i32.const 0) $callee)
 (func $callee (type $void))
 (func (export "apply") (param i64 i64 i64)
  (call_indirect $one_param (get_local 0) (i32.const 0))
 )
)
)=====";

const char call_depth_wast[] = R"=====(
(module
 (func $recurse (call $recurse))
 (func (export "apply") (param i64 i64 i64)
  (call $recurse)
 )
)
)=====";

// A host function checks the depth before the next call's prologue does, so the trap is raised from inside its wrapper.
const char call_depth_in_host_function_wast[] = R"=====(
(module
 (import "env" "current_time" (func $current_time (result i64)))
 (func $recurse
  (drop (call $current_time))
  (call $recurse)
 )
 (func (export "apply") (param i64 i64 i64)
  (call $recurse)
 )
)
)=====";

// sys-vm-oc copies a memcpy of a small constant size inline, after sysvmoc_internal.check_memcpy_params.
const char overlapping_memcpy_wast[] = R"=====(
(module
 (import "env" "memcpy" (func $memcpy (param i32 i32 i32) (result i32)))
 (memory 1)
 (func (export "apply") (param i64 i64 i64)
  (drop (call $memcpy (i32.const 0) (i32.const 1) (i32.const 8)))
 )
)
)=====";

const char host_function_throws_wast[] = R"=====(
(module
 (import "env" "sysio_assert" (func $sysio_assert (param i32 i32)))
 (memory 1)
 (func (export "apply") (param i64 i64 i64)
  (call $sysio_assert (i32.const 0) (i32.const 0))
 )
)
)=====";

const char exit_code_wast[] = R"=====(
(module
 (import "env" "sysio_exit" (func $sysio_exit (param i32)))
 (func (export "apply") (param i64 i64 i64)
  (call $sysio_exit (i32.const 1))
 )
)
)=====";

const char store_out_of_bounds_wast[] = R"=====(
(module
 (memory 1)
 (func (export "apply") (param i64 i64 i64)
  (i64.store (i32.const 65536) (i64.const 0))
 )
)
)=====";

// The data span faults while the arguments are converted, after the unaligned digest's argument proxy is built.
const char host_argument_out_of_bounds_wast[] = R"=====(
(module
 (import "env" "sha256" (func $sha256 (param i32 i32 i32)))
 (memory 1)
 (func (export "apply") (param i64 i64 i64)
  (call $sha256 (i32.const 65536) (i32.const 8) (i32.const 1))
 )
)
)=====";

/// Failures run before measuring, so one-time allocations (OC compile, caches, allocator bins) are not counted.
constexpr size_t warmup_failures = 100;
/// Failures the heap is measured across.
constexpr size_t measured_failures = 1000;
/// Heap growth per measured failure that fails the test. A leaked fc::exception with its log message is several
/// hundred bytes; without a leak the total stays within a few kilobytes either way.
constexpr int64_t max_growth_per_failure = 32;
/// Size of the allocation that checks the heap can be measured.
constexpr size_t probe_bytes = 4096;

/// Bytes glibc malloc has handed out and not got back, over every arena and mmapped chunk.
int64_t heap_bytes_in_use() {
   const struct mallinfo2 info = ::mallinfo2();
   return static_cast<int64_t>(info.uordblks + info.hblkhd);
}

/// False when malloc is not glibc's (the sanitizers replace it), where heap_bytes_in_use() sees no allocations.
bool heap_is_measurable() {
   const int64_t before = heap_bytes_in_use();
   void* volatile probe = ::malloc(probe_bytes);
   const int64_t after = heap_bytes_in_use();
   ::free(probe);
   return after - before >= static_cast<int64_t>(probe_bytes);
}

/**
 * Deploys `wast`, whose only action fails, to a new `account` and fails that action repeatedly.
 *
 * Every failure has to be `expected_code` with `expected_message`, and the heap must not grow with their number.
 */
void check_failures_do_not_leak(tester& chain, account_name account, const char* wast, int64_t expected_code,
                                const std::string& expected_message) {
   BOOST_TEST_CONTEXT(account.to_string()) {
      chain.create_account(account);
      chain.set_code(account, wast);
      chain.produce_block();

      // A failed transaction is not recorded, so the same one can be pushed again.
      signed_transaction trx;
      trx.actions.push_back({{{account, config::active_name}}, account, ""_n, bytes()});
      chain.set_transaction_headers(trx);
      trx.sign(chain.get_private_key(account, "active"), chain.get_chain_id());

      fc_exception_message_is is_expected_message{expected_message};
      size_t unexpected_results = 0;
      auto fail_once = [&] {
         constexpr bool no_throw = true;
         const transaction_trace_ptr trace =
            chain.push_transaction(trx, fc::time_point::maximum(), base_tester::DEFAULT_BILLED_CPU_TIME_US, no_throw);
         if (!trace->except || trace->except->code() != expected_code || !is_expected_message(*trace->except))
            ++unexpected_results;
      };

      for (size_t i = 0; i < warmup_failures; ++i)
         fail_once();
      const int64_t before = heap_bytes_in_use();
      for (size_t i = 0; i < measured_failures; ++i)
         fail_once();
      const int64_t growth = heap_bytes_in_use() - before;

      BOOST_TEST_MESSAGE(account.to_string() << ": heap grew " << growth << " bytes");
      BOOST_TEST(unexpected_results == 0u);
      BOOST_TEST(growth < static_cast<int64_t>(measured_failures) * max_growth_per_failure);
   }
}

} // namespace
#endif

BOOST_AUTO_TEST_SUITE(sysvmoc_trap_tests)

/// An action that fails under sys-vm-oc leaves wasm with siglongjmp, which runs no destructors. Whatever built the
/// exception has to be destroyed before the jump, or every failed action leaks it.
BOOST_AUTO_TEST_CASE(failed_action_does_not_grow_heap) try {
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   const bool measurable = heap_is_measurable(); // before the tester starts threads that allocate
   tester chain;
   if (chain.get_config().wasm_runtime != wasm_interface::vm_type::sys_vm_oc)
      return; // the other runtimes unwind with C++ exceptions
   if (!measurable) {
      BOOST_TEST_MESSAGE("malloc is not glibc's; heap growth cannot be measured");
      return;
   }

   constexpr int64_t wasm_error = wasm_execution_error::code_value;

   // The trap intrinsics.
   check_failures_do_not_leak(chain, "unreachable"_n, unreachable_wast, wasm_error, "Unreachable reached");
   check_failures_do_not_leak(chain, "div"_n, div0_wast, wasm_error, "Division by 0 or integer overflow trapped");
   check_failures_do_not_leak(chain, "calloob"_n, indirect_call_oob_wast, wasm_error,
                              "Indirect call index out of bounds");
   check_failures_do_not_leak(chain, "callmismatch"_n, indirect_call_mismatch_wast, wasm_error,
                              "Indirect call function type mismatch");
   check_failures_do_not_leak(chain, "calldepth"_n, call_depth_wast, wasm_error, "Exceeded call depth maximum");
   check_failures_do_not_leak(chain, "hostdepth"_n, call_depth_in_host_function_wast, wasm_error,
                              "Exceeded call depth maximum");
   check_failures_do_not_leak(chain, "memcpy"_n, overlapping_memcpy_wast, overlapping_memory_error::code_value,
                              "memcpy can only accept non-aliasing pointers");

   // The other exits: a host function that throws, sysio_exit, and the SIGSEGV handler.
   check_failures_do_not_leak(chain, "hostthrows"_n, host_function_throws_wast,
                              sysio_assert_message_exception::code_value, "assertion failure with message: ");
   check_failures_do_not_leak(chain, "exitcode"_n, exit_code_wast, wasm_error, "sysio_exit code must be 0, got 1");
   check_failures_do_not_leak(chain, "storeoob"_n, store_out_of_bounds_wast, wasm_error, "access violation");
   check_failures_do_not_leak(chain, "hostargoob"_n, host_argument_out_of_bounds_wast, wasm_error, "access violation");
#endif
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
