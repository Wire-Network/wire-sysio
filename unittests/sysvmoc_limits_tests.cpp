#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED

#include <sysio/testing/tester.hpp>
#include <test_contracts.hpp>
#include <boost/test/unit_test.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using mvo = fc::mutable_variant_object;

BOOST_AUTO_TEST_SUITE(sysvmoc_limits_tests)

// common routine to verify wasm_execution_error is raised when a resource
// limit specified in sysvmoc_config is reached
void limit_violated_test(const sysvmoc::config& sysvmoc_config) {
   fc::temp_directory tempdir;

   constexpr bool use_genesis = true;
   validating_tester chain(
      tempdir,
      [&](controller::config& cfg) {
         cfg.sysvmoc_config = sysvmoc_config;
      },
      use_genesis
   );

   chain.create_accounts({"sysio.test"_n});
   chain.set_code("sysio.test"_n, test_contracts::payloadless_wasm());
   chain.set_abi("sysio.test"_n, test_contracts::payloadless_abi());

   if (chain.control->is_sys_vm_oc_enabled()) {
      BOOST_CHECK_EXCEPTION(
         chain.push_action( "sysio.test"_n, "doitslow"_n, "sysio.test"_n, mvo()),
         sysio::chain::wasm_execution_error,
         [](const sysio::chain::wasm_execution_error& e) {
            return expect_assert_message(e, "failed to compile wasm");
         }
      );
   } else {
      chain.push_action( "sysio.test"_n, "doitslow"_n, "sysio.test"_n, mvo());
   }
}

// common routine to verify no wasm_execution_error is raised
// because limits specified in sysvmoc_config are not reached
void limit_not_violated_test(const sysvmoc::config& sysvmoc_config) {
   fc::temp_directory tempdir;

   constexpr bool use_genesis = true;
   validating_tester chain(
      tempdir,
      [&](controller::config& cfg) {
         cfg.sysvmoc_config = sysvmoc_config;
      },
      use_genesis
   );

   chain.create_accounts({"sysio.test"_n});
   chain.set_code("sysio.test"_n, test_contracts::payloadless_wasm());
   chain.set_abi("sysio.test"_n, test_contracts::payloadless_abi());

   chain.push_action( "sysio.test"_n, "doit"_n, "sysio.test"_n, mvo());
}

static sysvmoc::config make_sysvmoc_config_without_limits() {
   sysvmoc::config cfg;
   cfg.cpu_limit.reset();
   cfg.vm_limit.reset();
   cfg.stack_size_limit.reset();
   cfg.generated_code_size_limit.reset();
   return cfg;
}

// test all limits are not set for tests
BOOST_AUTO_TEST_CASE( limits_not_set ) { try {
   validating_tester chain;
   auto& cfg = chain.get_config();

   BOOST_REQUIRE(cfg.sysvmoc_config.cpu_limit == std::nullopt);
   BOOST_REQUIRE(cfg.sysvmoc_config.vm_limit == std::nullopt);
   BOOST_REQUIRE(cfg.sysvmoc_config.stack_size_limit == std::nullopt);
   BOOST_REQUIRE(cfg.sysvmoc_config.generated_code_size_limit == std::nullopt);
} FC_LOG_AND_RETHROW() }

// test limits are not enforced unless limits in sysvmoc_config
// are modified
BOOST_AUTO_TEST_CASE( limits_not_enforced ) { try {
   sysvmoc::config sysvmoc_config = make_sysvmoc_config_without_limits();
   limit_not_violated_test(sysvmoc_config);
} FC_LOG_AND_RETHROW() }

// UBSAN & ASAN can add massive virtual memory usage; skip this test when either are enabled
#if !__has_feature(undefined_behavior_sanitizer) && !__has_feature(address_sanitizer)
// test VM limit are checked
BOOST_AUTO_TEST_CASE( vm_limit ) { try {
   sysvmoc::config sysvmoc_config = make_sysvmoc_config_without_limits();

   // set vm_limit to a small value such that it is exceeded
   sysvmoc_config.vm_limit = 32u*1024u*1024u;
   limit_violated_test(sysvmoc_config);

   // set vm_limit to a large value such that it is not exceeded
   sysvmoc_config.vm_limit = 128u*1024u*1024u;
   limit_not_violated_test(sysvmoc_config);
} FC_LOG_AND_RETHROW() }

//make sure vm_limit is populated for a default constructed config (what nodeos will use)
BOOST_AUTO_TEST_CASE( check_config_default_vm_limit ) { try {
   sysvmoc::config eosvmoc_config;

   BOOST_REQUIRE(eosvmoc_config.vm_limit);
} FC_LOG_AND_RETHROW() }
#endif // !__has_feature(undefined_behavior_sanitizer) && !__has_feature(address_sanitizer)

// test stack size limit is checked
BOOST_AUTO_TEST_CASE( stack_limit ) { try {
   sysvmoc::config sysvmoc_config = make_sysvmoc_config_without_limits();

   // The stack size of the compiled WASM in the test is 39.
   // Set stack_size_limit one less than the actual needed stack size
   sysvmoc_config.stack_size_limit = 39;
   limit_violated_test(sysvmoc_config);

   // set stack_size_limit to the actual needed stack size
   sysvmoc_config.stack_size_limit = 40;
   limit_not_violated_test(sysvmoc_config);
} FC_LOG_AND_RETHROW() }

// test generated code size limit is checked
BOOST_AUTO_TEST_CASE( generated_code_size_limit ) { try {
   sysvmoc::config sysvmoc_config = make_sysvmoc_config_without_limits();

   // The generated code size of the compiled WASM in the test is 4016(used to be 3952).
   // Set generated_code_size_limit to the actual generated code size
   sysvmoc_config.generated_code_size_limit = 3952;
   limit_violated_test(sysvmoc_config);

   // Set generated_code_size_limit to one above the actual generated code size
   sysvmoc_config.generated_code_size_limit = 4017;
   limit_not_violated_test(sysvmoc_config);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

#endif
