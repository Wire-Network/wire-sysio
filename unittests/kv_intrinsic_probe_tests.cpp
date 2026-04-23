// Boost.Test driver for the kv_intrinsic_probe contract.
// Exercises adversarial inputs against the raw kv_* host intrinsics --
// inputs CDT wrappers would never emit but a malicious contract can.

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/exceptions.hpp>
#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

static const name probe_account  = "kvprobe"_n;
static const name probe_account2 = "kvprobe2"_n;

// Shared tester: deploy kv_intrinsic_probe once, reuse across cases.
// Each action uses distinct table_ids/keys so shared state is safe.
// Second account (kvprobe2) hosts the same contract for cross-contract probes.
struct kv_probe_shared_tester : validating_tester {
   kv_probe_shared_tester() {
      create_accounts({probe_account, probe_account2});
      produce_block();
      set_code(probe_account, test_contracts::kv_intrinsic_probe_wasm());
      set_abi(probe_account, test_contracts::kv_intrinsic_probe_abi().c_str());
      set_code(probe_account2, test_contracts::kv_intrinsic_probe_wasm());
      set_abi(probe_account2, test_contracts::kv_intrinsic_probe_abi().c_str());
      produce_block();
   }

   void run_action(name action_name) { run_action_on(probe_account, action_name); }

   void run_action_on(name acct, name action_name) {
      signed_transaction trx;
      trx.actions.emplace_back(
         vector<permission_level>{{acct, config::active_name}},
         acct,
         action_name,
         bytes{}
      );
      set_transaction_headers(trx);
      trx.sign(get_private_key(acct, "active"), control->get_chain_id());
      push_transaction(trx);
      produce_block();
   }
};

struct kv_probe_tester {
   kv_probe_shared_tester& t;
   kv_probe_tester() : t(shared_instance()) {}
   void run_action(name n) { t.run_action(n); }
   void run_action_on(name acct, name n) { t.run_action_on(acct, n); }
   static kv_probe_shared_tester& shared_instance() {
      static kv_probe_shared_tester inst;
      return inst;
   }
};

BOOST_AUTO_TEST_SUITE(kv_intrinsic_probe_tests)

// =============================================================================
// A. Accepted-behavior probes.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(zero_length_value, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("zval"_n));
}

BOOST_FIXTURE_TEST_CASE(zero_length_sec_key, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("zseckey"_n));
}

BOOST_FIXTURE_TEST_CASE(contains_consistency, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("contcns"_n));
}

BOOST_FIXTURE_TEST_CASE(iterator_invalidated_by_erase, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("itinvl"_n));
}

BOOST_FIXTURE_TEST_CASE(iterator_end_state_reads, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("endstat"_n));
}

BOOST_FIXTURE_TEST_CASE(it_value_offset_boundaries, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("offsa"_n));
}

// =============================================================================
// B. Rejection probes.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(kv_set_empty_key_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("zkeyset"_n), kv_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(kv_erase_empty_key_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("zkeyerz"_n), kv_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(kv_set_oversize_key_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("bigkey"_n), kv_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(kv_set_oversize_value_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("bigval"_n), kv_value_too_large);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_store_oversize_sec_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("bigsec"_n), kv_secondary_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(kv_it_create_oversize_prefix_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("bigpfx"_n), kv_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(table_id_above_uint16_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("tidmax"_n), kv_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(reserved_bits_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("badh"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(primary_intrinsic_on_secondary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crshp"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(secondary_intrinsic_on_primary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crshs"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_store_negative_primary_id_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("secneg"_n), kv_key_not_found);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_store_missing_primary_id_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("secmis"_n), kv_key_not_found);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_remove_negative_primary_id_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("idxrmneg"_n), kv_key_not_found);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_update_negative_primary_id_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("idxupdneg"_n), kv_key_not_found);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_update_oversize_new_sec_key_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("idxupdbig"_n), kv_secondary_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(use_after_destroy_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("uaf"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(double_destroy_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("dbldest"_n), kv_invalid_iterator);
}

// =============================================================================
// C. Additional accepted-behavior probes.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(aliased_key_value_spans, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("alias"_n));
}

BOOST_FIXTURE_TEST_CASE(kv_set_payer_zero_bills_receiver, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("payzero"_n));
}

BOOST_FIXTURE_TEST_CASE(iterator_slot_exhaustion, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("itlimit"_n), kv_iterator_limit_exceeded);
}

// =============================================================================
// D. Additional rejection probes (distinct SYS_ASSERT sites).
// =============================================================================

BOOST_FIXTURE_TEST_CASE(kv_erase_missing_key_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("erasmis"_n), kv_key_not_found);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_remove_missing_entry_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("idxrmmis"_n), kv_key_not_found);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_update_missing_entry_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("idxupmis"_n), kv_key_not_found);
}

// =============================================================================
// E. Additional accepted-behavior probes.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(find_secondary_miss_no_slot_leak, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("findmis"_n));
}

BOOST_FIXTURE_TEST_CASE(lower_bound_past_end_nonempty, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("lbpast"_n));
}

BOOST_FIXTURE_TEST_CASE(lower_bound_empty_table_no_slot_leak, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("lbempty"_n));
}

BOOST_FIXTURE_TEST_CASE(primary_key_after_primary_erased, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("prmera"_n));
}

BOOST_FIXTURE_TEST_CASE(it_prev_at_begin_transitions_to_end, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("itprvbgn"_n));
}

// =============================================================================
// F. Cross-pool rejection matrix.
//    Each primary kv_it_* intrinsic must reject secondary handles and each
//    secondary kv_idx_* intrinsic must reject primary handles. kv_it_next and
//    kv_idx_next are already covered by crshp/crshs above; the rest follow.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(kv_it_destroy_on_secondary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crshdst"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_it_prev_on_secondary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crshprv"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_it_lower_bound_on_secondary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crshlb"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_it_key_on_secondary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crshk"_n), kv_invalid_iterator);
}

// kv_it_value and kv_it_status accept both handle kinds by design; kv_it_value
// on a secondary handle reads through the cached primary_id.
BOOST_FIXTURE_TEST_CASE(kv_it_value_via_secondary_handle, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("itvalsec"_n));
}

BOOST_FIXTURE_TEST_CASE(kv_idx_prev_on_primary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crsiprv"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_key_on_primary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crsik"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_primary_key_on_primary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crsipk"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_destroy_on_primary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crsidst"_n), kv_invalid_iterator);
}

// =============================================================================
// G. Cross-contract primary_id rejection. Two-account setup: kvprobe stakes a
//    primary and caches its id in a kv row; kvprobe2 reads the id and attempts
//    kv_idx_store against it (its receiver is kvprobe2 but primary->code is
//    kvprobe -> table_operation_not_permitted via kv_idx_store's
//    primary->code == receiver guard).
// =============================================================================

BOOST_FIXTURE_TEST_CASE(cross_contract_primary_id_rejected, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("csrcstp"_n));
   BOOST_CHECK_THROW(run_action_on(probe_account2, "csrcttk"_n),
                     table_operation_not_permitted);
}

// =============================================================================
// H. Resource and invariant probes.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(secondary_iterator_slot_exhaustion, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("secilim"_n), kv_iterator_limit_exceeded);
}

BOOST_FIXTURE_TEST_CASE(max_size_key_and_value_accepted, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("maxok"_n));
}

BOOST_FIXTURE_TEST_CASE(dangling_secondary_after_primary_erase, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("danglng"_n));
}

BOOST_FIXTURE_TEST_CASE(cross_table_primary_id_permitted, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("prixtab"_n));
}

// Inline action gets its own apply_context with an empty iterator pool; the
// parent's handle 0 is meaningless to the child.
BOOST_FIXTURE_TEST_CASE(inline_action_iterator_isolation, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("inlparnt"_n), kv_invalid_iterator);
}

BOOST_AUTO_TEST_SUITE_END()
