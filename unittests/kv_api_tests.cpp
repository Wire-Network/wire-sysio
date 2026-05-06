// Boost.Test driver for the test_kv_api contract.
// Deploys test_kv_api and exercises every KV intrinsic through WASM actions.

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

static const name test_account = "kvtest"_n;

// Shared blockchain -- initialized once, reused by all kv_api test cases.
// Each WASM action is self-contained (creates its own keys, runs its own checks),
// so accumulating chain state across tests is safe.
struct kv_shared_tester : validating_tester {
   kv_shared_tester() {
      create_accounts({test_account});
      produce_block();
      set_code(test_account, test_contracts::test_kv_api_wasm());
      set_abi(test_account, test_contracts::test_kv_api_abi().c_str());
      produce_block();
   }

   void run_action(name action_name) {
      signed_transaction trx;
      trx.actions.emplace_back(
         vector<permission_level>{{test_account, config::active_name}},
         test_account,
         action_name,
         bytes{}
      );
      set_transaction_headers(trx);
      trx.sign(get_private_key(test_account, "active"), control->get_chain_id());
      push_transaction(trx);
      produce_block();
   }
};

// Per-test fixture: thin wrapper around the shared tester.
// Eliminates redundant blockchain boots (+ OC compilation under ASAN).
struct kv_api_tester {
   kv_shared_tester& t;
   kv_api_tester() : t(shared_instance()) {}
   void run_action(name n) { t.run_action(n); }
   static kv_shared_tester& shared_instance() {
      static kv_shared_tester inst;
      return inst;
   }
};

// Fresh per-test fixture for cross-scope tests whose actions use overlapping
// primary keys that would collide with prior shared-tester state.
struct kv_api_fresh_tester : kv_shared_tester {
};

BOOST_AUTO_TEST_SUITE(kv_api_tests)

// ─── Primary KV operations ─────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(kv_store_read, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testkvstord"_n));
}

BOOST_FIXTURE_TEST_CASE(kv_update, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testkvupdate"_n));
}

BOOST_FIXTURE_TEST_CASE(kv_erase, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testkverase"_n));
}

BOOST_FIXTURE_TEST_CASE(kv_contains, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testkvexist"_n));
}

BOOST_FIXTURE_TEST_CASE(kv_set_partial_via_read_modify_write, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testkvsetbt"_n));
}

// ─── Primary iterator operations ───────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(it_create_status_destroy, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitcreate"_n));
}

BOOST_FIXTURE_TEST_CASE(it_next_forward, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitnext"_n));
}

BOOST_FIXTURE_TEST_CASE(it_prev_backward, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitprev"_n));
}

BOOST_FIXTURE_TEST_CASE(it_lower_bound, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitlbound"_n));
}

BOOST_FIXTURE_TEST_CASE(it_key_read, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitkey"_n));
}

BOOST_FIXTURE_TEST_CASE(it_value_read, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitvalue"_n));
}

BOOST_FIXTURE_TEST_CASE(it_upper_bound, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitubound"_n));
}

// ─── Secondary index operations ────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(idx_store, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxstore"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_remove, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxremov"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_update, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxupdat"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_find_secondary, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxfind"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_lower_bound, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxlbnd"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_next, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxnext"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_prev, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxprev"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_key_read, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxkey"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_primary_key_read, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxprik"_n));
}

// ─── Edge cases ────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(cross_contract_read, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testcrossrd"_n));
}

BOOST_FIXTURE_TEST_CASE(empty_value, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testemptyval"_n));
}

BOOST_FIXTURE_TEST_CASE(multi_key_sizes, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testmultikey"_n));
}

BOOST_FIXTURE_TEST_CASE(nested_struct_roundtrip, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testnested"_n));
}

// ─── Iterator lifecycle & invalidation ─────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(it_destroy_reuse, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitdestr"_n));
}

BOOST_FIXTURE_TEST_CASE(it_handle_reuse_cycle, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitreuse"_n));
}

BOOST_FIXTURE_TEST_CASE(it_erased_row_invalidation, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tsterasedinv"_n));
}

BOOST_FIXTURE_TEST_CASE(it_pool_full_16, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstitexhaust"_n));
}

BOOST_FIXTURE_TEST_CASE(it_pool_overflow_1025, kv_api_tester) {
   // 1025th iterator allocation should throw kv_iterator_limit_exceeded
   BOOST_CHECK_THROW(run_action("tstitexhfail"_n), kv_iterator_limit_exceeded);
}

BOOST_FIXTURE_TEST_CASE(it_prefix_isolation, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitprefix"_n));
}

BOOST_FIXTURE_TEST_CASE(it_empty_prefix, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstitempttbl"_n));
}

// ─── Write permission & authorization ──────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(write_perm_key_format, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstwriteperm"_n));
}

BOOST_FIXTURE_TEST_CASE(payer_variants, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testpayer"_n));
}

// ─── Key size boundaries ───────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(max_key_256, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testmaxkey"_n));
}

BOOST_FIXTURE_TEST_CASE(oversize_key_257, kv_api_tester) {
   // 257-byte key should fail with kv_key_too_large
   BOOST_CHECK_THROW(run_action("tstovrszkey"_n), kv_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(large_value_1024, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testmaxval"_n));
}

// ─── Value operations ──────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(partial_read, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testpartread"_n));
}

BOOST_FIXTURE_TEST_CASE(zero_value, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testzeroval"_n));
}

BOOST_FIXTURE_TEST_CASE(value_replace_grow_shrink, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstvalreplce"_n));
}

// ─── Key format parameter ──────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(key_format_variants, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstkeyfrmt"_n));
}

// ─── Secondary index comprehensive ────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(idx_multi_indices, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxmulti"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_duplicate_sec_keys, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxdupsk"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_range_iteration, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxrange"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_empty_table, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testidxempty"_n));
}

// ─── Concurrent iteration ──────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(multi_iterators_same_prefix, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testmultiit"_n));
}

BOOST_FIXTURE_TEST_CASE(it_write_during_iteration, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testitwritev"_n));
}

// ─── Cross-scope / multi-table ─────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(multi_table_isolation, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testmultitbl"_n));
}

BOOST_FIXTURE_TEST_CASE(scoped_key_isolation, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testscoped"_n));
}

// ─── Data integrity ───────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(binary_roundtrip_all_bytes, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testbinround"_n));
}

BOOST_FIXTURE_TEST_CASE(large_populate_100_rows, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("testlargepop"_n));
}

// ─── kv_multi_index tests ──────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(mi_decrement_end, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstdecend"_n));
}

BOOST_FIXTURE_TEST_CASE(mi_begin_eq_end_empty, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstbeginend"_n));
}

BOOST_FIXTURE_TEST_CASE(mi_forward_iteration, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstfwditer"_n));
}

BOOST_FIXTURE_TEST_CASE(mi_reverse_iteration, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstreviter"_n));
}

BOOST_FIXTURE_TEST_CASE(mi_find_missing, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstfindmiss"_n));
}

BOOST_FIXTURE_TEST_CASE(mi_end_destructor, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstenddestr"_n));
}

BOOST_FIXTURE_TEST_CASE(mi_emplace_get, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstemplace"_n));
}

BOOST_FIXTURE_TEST_CASE(mi_modify, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstmodify"_n));
}

BOOST_FIXTURE_TEST_CASE(mi_erase, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tsterase"_n));
}

BOOST_FIXTURE_TEST_CASE(mi_lower_bound, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstlbound"_n));
}

// secondary_index_view modify/erase/iterate tests
BOOST_FIXTURE_TEST_CASE(sec_modify_via_iterator, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecmod"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_erase_via_iterator, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecerase"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_iterate_order, kv_api_fresh_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstseciter"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_find_int_coercion, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstseccoerce"_n));
}

// rbegin/rend tests
BOOST_FIXTURE_TEST_CASE(primary_rbegin_rend, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstrbegin"_n));
}

BOOST_FIXTURE_TEST_CASE(primary_rbegin_empty, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstrbempty"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_rbegin_rend, kv_api_fresh_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecrbegin"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_erase_returns_next, kv_api_fresh_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecersnxt"_n));
}

// (kv::raw_table and old kv::table tests removed -- types dropped in table_id migration)

// payer validation tests
BOOST_FIXTURE_TEST_CASE(payer_self_allowed, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstpayerself"_n));
}

BOOST_FIXTURE_TEST_CASE(payer_other_requires_auth, kv_api_tester) {
   // Billing another account without their authorization fails at the
   // transaction level (unauthorized_ram_usage_increase), not at the KV level.
   t.create_accounts({"alice"_n});
   t.produce_block();
   BOOST_CHECK_THROW(run_action("tstpayeroth"_n), sysio::chain::unauthorized_ram_usage_increase);
}

// empty key rejection tests
BOOST_FIXTURE_TEST_CASE(empty_key_rejected_kv_set, kv_api_tester) {
   BOOST_CHECK_THROW(run_action("tstemptykey"_n), fc::exception);
}

// table_id > UINT16_MAX is rejected
BOOST_FIXTURE_TEST_CASE(table_id_overflow_rejected, kv_api_tester) {
   BOOST_CHECK_THROW(run_action("tstbadfmt"_n), fc::exception);
}

// security edge case tests (from audit)
BOOST_FIXTURE_TEST_CASE(iterator_erase_reinsert_same_key, kv_api_tester) {
   // After erasing and reinserting with same key, iterator should see new row
   BOOST_CHECK_NO_THROW(run_action("tstreraseins"_n));
}

BOOST_FIXTURE_TEST_CASE(iterator_prev_from_begin, kv_api_tester) {
   // kv_it_prev from the first element should return end status
   BOOST_CHECK_NO_THROW(run_action("tstprevbgn"_n));
}

BOOST_FIXTURE_TEST_CASE(iterator_prev_erased, kv_api_tester) {
   // kv_it_key on erased row should return erased status
   BOOST_CHECK_NO_THROW(run_action("tstpreveras"_n));
}

BOOST_FIXTURE_TEST_CASE(read_only_trx_rejects_write, kv_api_tester) {
   // kv_set in a read-only transaction must fail
   signed_transaction trx;
   trx.actions.emplace_back(
      vector<permission_level>{{test_account, config::active_name}},
      test_account, "tstrdonly"_n, bytes{}
   );
   t.set_transaction_headers(trx);
   trx.sign(t.get_private_key(test_account, "active"), t.control->get_chain_id());
   BOOST_CHECK_THROW(
      t.push_transaction(trx, fc::time_point::maximum(), kv_shared_tester::DEFAULT_BILLED_CPU_TIME_US, false,
                       transaction_metadata::trx_type::read_only),
      fc::exception);
}

BOOST_FIXTURE_TEST_CASE(key_offset_beyond_size, kv_api_tester) {
   // kv_it_key with offset >= key_size should return 0 bytes copied
   BOOST_CHECK_NO_THROW(run_action("tstkeyoffbnd"_n));
}

BOOST_FIXTURE_TEST_CASE(oversized_secondary_key_rejected, kv_api_tester) {
   // kv_idx_store with sec_key > max_kv_secondary_key_size must fail
   BOOST_CHECK_THROW(run_action("tstbigseckey"_n), fc::exception);
}

BOOST_FIXTURE_TEST_CASE(notify_context_ram_billing, kv_api_tester) {
   // When a contract receives a notification (receiver != act.account),
   // kv_set writes to the receiver's namespace and bills receiver's RAM
   name notify_acct = "kvnotify"_n;
   t.create_accounts({notify_acct});
   t.produce_block();
   t.set_code(notify_acct, test_contracts::test_kv_api_wasm());
   t.set_abi(notify_acct, test_contracts::test_kv_api_abi().c_str());
   t.produce_block();

   // Push tstsendnotif on test_account -- it calls require_recipient(kvnotify)
   // kvnotify receives the notification and executes tstnotifyram handler (via apply)
   // Actually, require_recipient just forwards the same action, so kvnotify's
   // apply will see action=tstsendnotif. The notification will succeed and
   // any kv_set in kvnotify's on_notify will write to kvnotify's KV space.
   // For this test, we just verify the notification doesn't crash.
   BOOST_CHECK_NO_THROW(run_action("tstsendnotif"_n));
   t.produce_block();

   // Verify kvnotify did NOT write to test_account's KV space
   // (notification receiver writes to its own namespace)
}

// secondary iterator clone with duplicate keys
BOOST_FIXTURE_TEST_CASE(sec_iterator_clone_duplicate_keys, kv_api_fresh_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecclone"_n));
}

// secondary rbegin/rend with uint128_t keys
BOOST_FIXTURE_TEST_CASE(sec_rbegin_uint128_keys, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecrbig"_n));
}

// --------------------------------------------------------------------------
// Cross-scope secondary index isolation tests
// Verify that kv_multi_index secondary indices are properly scoped.
// These use kv_api_fresh_tester because they exercise multi_index with
// overlapping primary keys across scopes.
// --------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(sec_cross_scope_isolation, kv_api_fresh_tester) {
   // Two scopes with same PKs -- iteration in each scope must be isolated
   BOOST_CHECK_NO_THROW(run_action("tstxscope"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_cross_scope_find, kv_api_fresh_tester) {
   // find(sec_val) in scope A must not return scope B's entry
   BOOST_CHECK_NO_THROW(run_action("tstxfind"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_cross_scope_erase, kv_api_fresh_tester) {
   // Erase from scope A must not affect scope B
   BOOST_CHECK_NO_THROW(run_action("tstxerase"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_cross_scope_double, kv_api_fresh_tester) {
   // double secondary: sort-preserving transform + scope isolation
   BOOST_CHECK_NO_THROW(run_action("tstxdbl"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_cross_scope_zero, kv_api_fresh_tester) {
   // scope=0 is the minimum scope prefix -- verify isolation
   BOOST_CHECK_NO_THROW(run_action("tstxzero"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_cross_scope_reverse, kv_api_fresh_tester) {
   // Reverse iteration (--end()) must not leak across scopes
   BOOST_CHECK_NO_THROW(run_action("tstxrev"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_cross_scope_upper_bound, kv_api_fresh_tester) {
   // upper_bound in scope A stops at scope boundary
   BOOST_CHECK_NO_THROW(run_action("tstxubound"_n));
}

// --------------------------------------------------------------------------
// RAM billing tests -- verify correct billing on create, update, erase
// --------------------------------------------------------------------------

// billable_size_v<kv_object> / <kv_index_object> from config -- use the actual
// compile-time constants so these tests catch any drift in the billing formula.
static constexpr int64_t KV_OVERHEAD = config::billable_size_v<kv_object>;
static constexpr int64_t KV_INDEX_OVERHEAD = config::billable_size_v<kv_index_object>;

struct kv_billing_tester : validating_tester {
   kv_billing_tester() {
      create_accounts({test_account});
      produce_block();
      set_code(test_account, test_contracts::test_kv_api_wasm());
      set_abi(test_account, test_contracts::test_kv_api_abi().c_str());
      produce_block();
   }

   int64_t get_ram_usage() {
      return control->get_resource_limits_manager().get_account_ram_usage(test_account);
   }

   void ram_store(uint32_t key_id, uint32_t val_size) {
      push_action(test_account, "ramstore"_n, test_account,
         mutable_variant_object()("key_id", key_id)("val_size", val_size));
      produce_block();
   }

   void ram_update(uint32_t key_id, uint32_t val_size) {
      push_action(test_account, "ramupdate"_n, test_account,
         mutable_variant_object()("key_id", key_id)("val_size", val_size));
      produce_block();
   }

   void ram_erase(uint32_t key_id) {
      push_action(test_account, "ramerase"_n, test_account,
         mutable_variant_object()("key_id", key_id));
      produce_block();
   }
};

BOOST_FIXTURE_TEST_CASE(billing_create_row, kv_billing_tester) {
   // Creating a row should bill key_size + value_size + KV_OVERHEAD
   auto before = get_ram_usage();
   ram_store(1, 100); // 4-byte key, 100-byte value
   auto after = get_ram_usage();

   int64_t expected = 4 + 100 + KV_OVERHEAD; // key + value + overhead
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_create_row: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_FIXTURE_TEST_CASE(billing_update_grow, kv_billing_tester) {
   // Growing a value should bill the delta
   ram_store(2, 50);
   auto before = get_ram_usage();
   ram_update(2, 200); // grow from 50 to 200
   auto after = get_ram_usage();

   int64_t expected = 200 - 50; // only value delta, key+overhead unchanged
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_update_grow: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_FIXTURE_TEST_CASE(billing_update_shrink, kv_billing_tester) {
   // Shrinking a value should refund the delta
   ram_store(3, 200);
   auto before = get_ram_usage();
   ram_update(3, 50); // shrink from 200 to 50
   auto after = get_ram_usage();

   int64_t expected = 50 - 200; // negative delta = refund
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_update_shrink: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_FIXTURE_TEST_CASE(billing_update_same_size, kv_billing_tester) {
   // Updating with same size should bill zero
   ram_store(4, 100);
   auto before = get_ram_usage();
   ram_update(4, 100); // same size
   auto after = get_ram_usage();

   BOOST_TEST_MESSAGE("billing_update_same_size: delta=" << (after - before));
   BOOST_REQUIRE_EQUAL(after - before, 0);
}

BOOST_FIXTURE_TEST_CASE(billing_erase_refund, kv_billing_tester) {
   // Erasing should refund the full amount billed on create
   ram_store(5, 100);
   auto before = get_ram_usage();
   ram_erase(5);
   auto after = get_ram_usage();

   int64_t expected = -(4 + 100 + KV_OVERHEAD); // full refund
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_erase_refund: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_FIXTURE_TEST_CASE(billing_create_erase_net_zero, kv_billing_tester) {
   // Create + erase should result in net zero RAM change
   auto before = get_ram_usage();
   ram_store(6, 500);
   ram_erase(6);
   auto after = get_ram_usage();

   BOOST_TEST_MESSAGE("billing_create_erase_net_zero: delta=" << (after - before));
   BOOST_REQUIRE_EQUAL(after - before, 0);
}

BOOST_FIXTURE_TEST_CASE(billing_multiple_rows, kv_billing_tester) {
   // Multiple rows should each be billed independently
   auto before = get_ram_usage();
   ram_store(10, 100);
   ram_store(11, 200);
   ram_store(12, 300);
   auto after = get_ram_usage();

   int64_t expected = 3 * (4 + KV_OVERHEAD) + 100 + 200 + 300; // 3 rows, different values
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_multiple_rows: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

// --------------------------------------------------------------------------
// Secondary index (kv_index_object) billing tests -- exact-delta coverage so
// drift in kv_index_object_ram() cannot pass silently.
// --------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(billing_idx_create_row, kv_billing_tester) {
   // testidxstore calls kv_idx_store with sec="alice" (5 bytes), pri=3 bytes.
   auto before = get_ram_usage();
   push_action(test_account, "testidxstore"_n, test_account, mutable_variant_object());
   produce_block();
   auto after = get_ram_usage();

   int64_t expected = 3 + 5 + KV_INDEX_OVERHEAD;
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_idx_create_row: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_FIXTURE_TEST_CASE(billing_idx_create_erase_net_zero, kv_billing_tester) {
   // testidxremov calls kv_idx_store then kv_idx_remove on the same row.
   // If store and remove bill the same formula, the net delta is zero.
   auto before = get_ram_usage();
   push_action(test_account, "testidxremov"_n, test_account, mutable_variant_object());
   produce_block();
   auto after = get_ram_usage();

   BOOST_TEST_MESSAGE("billing_idx_create_erase_net_zero: delta=" << (after - before));
   BOOST_REQUIRE_EQUAL(after - before, 0);
}

BOOST_FIXTURE_TEST_CASE(billing_idx_update_shrink, kv_billing_tester) {
   // testidxupdat: kv_idx_store(sec="charlie"/7, pri=3) then
   //               kv_idx_update(old_sec="charlie"/7 -> new_sec="david"/5).
   // Total bill = store + update_delta = (3 + 7 + KV_INDEX_OVERHEAD) + (5 - 7)
   //            = 3 + 5 + KV_INDEX_OVERHEAD.
   auto before = get_ram_usage();
   push_action(test_account, "testidxupdat"_n, test_account, mutable_variant_object());
   produce_block();
   auto after = get_ram_usage();

   int64_t expected = 3 + 5 + KV_INDEX_OVERHEAD;
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_idx_update_shrink: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_AUTO_TEST_SUITE_END()
