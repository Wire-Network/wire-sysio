#include <sysio/testing/tester.hpp>
#include <test_contracts.hpp>
#include <test_utils.hpp>
#include <boost/test/unit_test.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace test_utils;
using mvo = fc::mutable_variant_object;

BOOST_AUTO_TEST_SUITE(sysvmoc_interrupt_tests)

BOOST_AUTO_TEST_CASE( wasm_interrupt_test ) { try {
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   fc::temp_directory tempdir;
   constexpr bool use_genesis = true;
   savanna_validating_tester t(
      tempdir,
      [&](controller::config& cfg) {
         cfg.sys_vm_oc_whitelist_suffixes.insert("testapi"_n);
         if (cfg.wasm_runtime != wasm_interface::vm_type::sys_vm_oc)
            cfg.sysvmoc_tierup = chain::wasm_interface::vm_oc_enable::oc_auto;
      },
      use_genesis
   );
   if( t.get_config().wasm_runtime == wasm_interface::vm_type::sys_vm_oc ) {
      // eos_vm_oc wasm_runtime does not tier-up and completes compile before continuing execution.
      // A completely different test with different constraints would be needed to test with eos_vm_oc.
      // Since non-tier-up is not a normal valid nodeos runtime, just skip this test for eos_vm_oc.
      return;
   }
   t.execute_setup_policy( setup_policy::full );
   t.produce_block();

   t.create_account( "testapi"_n );
   t.set_code( "testapi"_n, test_contracts::test_api_wasm() );
   t.produce_block();

   auto pre_count = t.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();

   // Use an infinite executing action. When oc compile completes it will kill the action and restart it under
   // sysvmoc. That action will then fail when it hits the deadline.
   // The deadline has to be long enough for oc compile to complete and kill the non-oc executing transaction.
   // LLVM 18 compilation is slower than LLVM 11, requiring a larger deadline.
   BOOST_CHECK_THROW( push_trx( t, test_api_action<WASM_TEST_ACTION("test_checktime", "checktime_failure")>{},
                                0, 150, 30000, true, fc::raw::pack(10000000000000000000ULL) ),
                      deadline_exception );

   auto post_count = t.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();

   // if post_count == pre_count, then likely that the deadline above was not long enough for oc compile to complete
   BOOST_TEST(post_count == pre_count + 1);

   BOOST_REQUIRE_EQUAL( t.validate(), true );
#endif
} FC_LOG_AND_RETHROW() }

// Regression for the sys-vm-oc interrupt dropping a transaction's dedup record. When oc tier-up
// completes mid-execution of a transaction applied as part of a block (explicit cpu billing), the
// transaction is restarted via transaction_context::reset(), whose undo() reverts the dedup record
// made in init_for_input_trx; the retry re-runs only exec(), so without the fix the record is never
// re-created and the transaction silently drops out of the dedup set on the interrupting node.
//
// The action is finite-but-long: long enough under the interpreter that oc compile completes and
// interrupts+restarts it, but it completes under sys-vm-oc and the transaction is committed. We then
// REQUIRE the interrupt fired (so the dedup record was actually dropped and re-recorded) and assert
// the transaction is still known.
//
// The interrupt only fires if oc compile completes while the action is still executing under the
// interpreter, and no earlier than 500ms after the compile was queued (async_compile_complete's
// floor). Whether a single fixed-size action gets interrupted therefore depends on machine speed: a
// fast release build can finish the action before the floor. To make the interrupt a hard
// prerequisite without making the test timing-flaky, retry with the action size doubling each
// attempt, re-deploying the contract with a distinct trailing custom section each time -- a fresh
// code hash, so every attempt opens a fresh compile-interrupt window.
BOOST_AUTO_TEST_CASE( oc_interrupt_preserves_dedup_record ) { try {
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   fc::temp_directory tempdir;
   constexpr bool use_genesis = true;
   savanna_validating_tester t(
      tempdir,
      [&](controller::config& cfg) {
         cfg.sys_vm_oc_whitelist_suffixes.insert("testapi"_n);
         if (cfg.wasm_runtime != wasm_interface::vm_type::sys_vm_oc)
            cfg.sysvmoc_tierup = chain::wasm_interface::vm_oc_enable::oc_auto;
      },
      use_genesis
   );
   if( t.get_config().wasm_runtime == wasm_interface::vm_type::sys_vm_oc ) {
      return; // eos_vm_oc does not tier-up; no interrupt path (see wasm_interrupt_test)
   }
   t.execute_setup_policy( setup_policy::full );
   t.produce_block();

   t.create_account( "testapi"_n );

   const std::vector<uint8_t> base_wasm = test_contracts::test_api_wasm();
   constexpr int max_attempts = 3;
   transaction_id_type interrupted_trx_id;
   bool fired = false;
   unsigned long long bound = 20000; // checktime_failure runs bound^2 iterations; doubles per attempt

   for( int attempt = 0; attempt < max_attempts && !fired; ++attempt, bound *= 2 ) {
      // Re-deploy with a unique trailing custom section (id 0, size 2, name len 1, 1-char name):
      // identical behavior, distinct code hash, so this attempt compiles -- and can interrupt --
      // afresh. (A code hash that is already compiled executes directly under oc and can never
      // exercise the interrupt path again.)
      //
      // The custom section deliberately has a zero-length payload after the name. That is legal
      // wasm, and the oc compile child deserializes it through wasm-jit's
      // serializeBytes(InputStream&,...), which must tolerate an empty (null-data) byte range --
      // under the ubsan CI build (-fno-sanitize-recover) a regression there kills the compile
      // child, the compile reports unknownfailure, and this test fails its fired-interrupt
      // requirement. Keep the payload empty so that coverage is exercised every run.
      std::vector<uint8_t> wasm = base_wasm;
      const uint8_t custom_section[] = { 0x00, 0x02, 0x01, static_cast<uint8_t>('A' + attempt) };
      wasm.insert( wasm.end(), std::begin(custom_section), std::end(custom_section) );
      t.set_code( "testapi"_n, wasm );
      t.produce_block();

      const auto pre_count = t.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();

      // Build a finite checktime action and push it with explicit cpu billing so it takes the
      // applying-block path where the oc interrupt is allowed.
      signed_transaction trx;
      action act;
      act.account = "testapi"_n;
      act.name = action_name(WASM_TEST_ACTION("test_checktime", "checktime_failure"));
      act.authorization = vector<permission_level>{{"testapi"_n, config::active_name}};
      act.data = fc::raw::pack( bound );
      trx.actions.push_back( act );
      t.set_transaction_headers( trx );
      trx.sign( t.get_private_key( "testapi"_n, "active" ), t.get_chain_id() );
      const auto trx_id = trx.id();
      const auto total_actions = trx.total_actions();

      auto ptrx = std::make_shared<packed_transaction>( std::move(trx) );
      auto fut = transaction_metadata::start_recover_keys( std::move(ptrx), t.control->get_thread_pool(),
                                                           t.get_chain_id(), fc::microseconds::maximum(),
                                                           transaction_metadata::trx_type::input );
      auto trx_meta = fut.get();
      cpu_usage_t billed_cpu_us;
      billed_cpu_us.insert( billed_cpu_us.end(), total_actions, 1000u ); // explicit, within [min, max]
      auto res = t.control->test_push_transaction( trx_meta, fc::time_point::now() + fc::seconds(60),
                                                   fc::seconds(60), billed_cpu_us, /*explicit_bill*/ true );
      if( res->except_ptr ) std::rethrow_exception( res->except_ptr );
      if( res->except ) throw *res->except;

      const auto post_count = t.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();
      t.produce_block(); // commit the pending block containing the transaction

      if( post_count == pre_count + 1 ) {
         fired = true;
         interrupted_trx_id = trx_id;
      } else {
         ilog("oc compile interrupt did not fire at bound {}; retrying with a longer action", bound);
      }
   }

   // The interrupt firing is a prerequisite: only then was the dedup record dropped by reset() and
   // re-recorded by the fix. Without it the assertion below would be trivially satisfied by
   // init_for_input_trx alone.
   BOOST_REQUIRE_MESSAGE( fired, "oc compile interrupt did not fire within " << max_attempts <<
                                 " attempts; regression path not exercised" );

   // Must still be deduplicated. Without the fix, the interrupt's reset() dropped the record.
   BOOST_CHECK( t.control->is_known_unexpired_transaction( interrupted_trx_id ) );
#endif
} FC_LOG_AND_RETHROW() }

// A FAILED oc compile must not interrupt the executing transaction: there is no oc code to
// switch to, so the interrupt would only force a pointless restart on the baseline runtime --
// and worse. For a whitelisted account get_descriptor_for_code() un-blacklists the code and
// queues a fresh compile on the restart, so a persistently failing compile interrupts the
// restarted transaction again, exceeding the single interrupt-retry transaction_context::exec()
// allows; the transaction then fails with interrupt_oc_exception, which on a node applying a
// block means rejecting a valid block. (This is exactly how the ubsan CI failed before
// wasm-jit's serializeBytes tolerated empty user-section payloads: the sanitizer killed the
// compile child, the monitor reported unknownfailure, and the interrupt still fired.)
//
// Force a deterministic compile failure via the subjective generated-code size limit (enforced
// only for non-whitelisted accounts; payloadless generates far more than 1 KiB of native code,
// see sysvmoc_limits_tests/generated_code_size_limit) and run an action that spins until the
// deadline. The compile failure lands mid-execution; before the fix it fired the interrupt
// (observable as the interrupt count incrementing), with the fix the count must stay unchanged.
BOOST_AUTO_TEST_CASE( oc_compile_failure_does_not_interrupt ) { try {
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   fc::temp_directory tempdir;
   constexpr bool use_genesis = true;
   savanna_validating_tester t(
      tempdir,
      [&](controller::config& cfg) {
         if (cfg.wasm_runtime != wasm_interface::vm_type::sys_vm_oc)
            cfg.sysvmoc_tierup = chain::wasm_interface::vm_oc_enable::oc_all; // tier up non-whitelisted accounts too
         // libtester resets all subjective compile limits; re-enable only the generated code
         // size limit, small enough that compiling payloadless always fails.
         cfg.sysvmoc_config.non_whitelisted_limits.cpu_limit.reset();
         cfg.sysvmoc_config.non_whitelisted_limits.vm_limit.reset();
         cfg.sysvmoc_config.non_whitelisted_limits.stack_size_limit.reset();
         cfg.sysvmoc_config.non_whitelisted_limits.generated_code_size_limit = 1024;
      },
      use_genesis
   );
   if( t.get_config().wasm_runtime == wasm_interface::vm_type::sys_vm_oc ) {
      // With sys-vm-oc as the base runtime the compile is synchronous and its failure surfaces
      // as wasm_execution_error (covered by sysvmoc_limits_tests); there is no interrupt path.
      return;
   }
   t.execute_setup_policy( setup_policy::full );
   t.produce_block();

   t.create_account( "payloadless"_n );
   t.set_code( "payloadless"_n, test_contracts::payloadless_wasm() );
   t.set_abi( "payloadless"_n, test_contracts::payloadless_abi() );
   t.produce_block();

   const auto pre_count = t.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();

   // doitforever spins until the block deadline below stops it; the failed compile result
   // arrives well within that window. Explicit cpu billing takes the applying-block path where
   // the oc interrupt is allowed.
   signed_transaction trx;
   action act;
   act.account = "payloadless"_n;
   act.name = "doitforever"_n;
   act.authorization = vector<permission_level>{{"payloadless"_n, config::active_name}};
   trx.actions.push_back( act );
   t.set_transaction_headers( trx );
   trx.sign( t.get_private_key( "payloadless"_n, "active" ), t.get_chain_id() );
   const auto total_actions = trx.total_actions();

   auto ptrx = std::make_shared<packed_transaction>( std::move(trx) );
   auto fut = transaction_metadata::start_recover_keys( std::move(ptrx), t.control->get_thread_pool(),
                                                        t.get_chain_id(), fc::microseconds::maximum(),
                                                        transaction_metadata::trx_type::input );
   auto trx_meta = fut.get();
   cpu_usage_t billed_cpu_us;
   billed_cpu_us.insert( billed_cpu_us.end(), total_actions, 1000u );

   BOOST_CHECK_THROW(
      ([&]() {
         auto res = t.control->test_push_transaction( trx_meta, fc::time_point::now() + fc::seconds(8),
                                                      fc::seconds(60), billed_cpu_us, /*explicit_bill*/ true );
         if( res->except_ptr ) std::rethrow_exception( res->except_ptr );
         if( res->except ) throw *res->except;
      })(),
      deadline_exception );

   const auto post_count = t.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();
   BOOST_TEST( post_count == pre_count );

   t.produce_block();
   BOOST_REQUIRE_EQUAL( t.validate(), true );
#endif
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
