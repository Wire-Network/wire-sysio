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
// assert the transaction is still known. This assertion can fail ONLY when the bug manifests (the
// interrupt fired AND the record was dropped); if oc compile happens not to interrupt this run, the
// record is trivially still present, so the test never fails spuriously.
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
   t.set_code( "testapi"_n, test_contracts::test_api_wasm() );
   t.produce_block();

   const auto pre_count = t.control->get_wasm_interface().get_sys_vm_oc_compile_interrupt_count();

   // Build a finite checktime action (bound^2 inner iterations) and push it with explicit cpu
   // billing so it takes the applying-block path where the oc interrupt is allowed.
   signed_transaction trx;
   action act;
   act.account = "testapi"_n;
   act.name = action_name(WASM_TEST_ACTION("test_checktime", "checktime_failure"));
   act.authorization = vector<permission_level>{{"testapi"_n, config::active_name}};
   act.data = fc::raw::pack( (unsigned long long)15000 );
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

   // Must still be deduplicated. Without the fix, the interrupt's reset() dropped the record.
   BOOST_CHECK( t.control->is_known_unexpired_transaction( trx_id ) );
   if( post_count == pre_count )
      ilog("oc compile interrupt did not fire this run; dedup-survival path not exercised");
#endif
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
