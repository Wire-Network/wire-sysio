#include <sysio/testing/tester.hpp>
#include <boost/test/unit_test.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
namespace {
constexpr auto test_account = "codegentest"_n;
constexpr auto test_action = "check"_n;
/// Keep the comparison operand dynamic inside each function so InstCombine
/// must reason about the negative i32 constant rather than fold a constant call.
constexpr auto signed_constants_wast = R"(
(module
  (memory 1)
  (func $below_threshold (param i32) (result i32)
    (i32.lt_u (get_local 0) (i32.const -16)))
  (func $below_sign_bit (param i32) (result i32)
    (i32.lt_u (get_local 0) (i32.const -2147483648)))
  (func $apply (export "apply") (param i64 i64 i64)
    (if (i32.eqz (call $below_threshold (i32.const 100))) (then (unreachable)))
    (if (call $below_threshold (i32.const -1)) (then (unreachable)))
    (if (i32.eqz (call $below_sign_bit (i32.const 2147483647))) (then (unreachable)))
    (if (call $below_sign_bit (i32.const -2147483648)) (then (unreachable)))
  )
))";
}

BOOST_AUTO_TEST_SUITE(sysvmoc_codegen_tests)

BOOST_AUTO_TEST_CASE(negative_i32_constants_preserve_unsigned_comparisons) { try {
   fc::temp_directory directory;
   tester chain(directory, [](controller::config& cfg) {
      cfg.wasm_runtime = wasm_interface::vm_type::sys_vm_oc;
   }, true);
   chain.create_accounts({test_account});
   chain.set_code(test_account, signed_constants_wast);
   action check;
   check.account = test_account;
   check.name = test_action;
   BOOST_CHECK_EQUAL(chain.push_action(std::move(check), test_account.to_uint64_t()), tester::success());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
#endif
