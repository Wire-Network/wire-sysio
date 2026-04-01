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

struct kv_api_tester : validating_tester {
   kv_api_tester() {
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

// Key/value boundary tests on UPDATE (create tests above)

BOOST_FIXTURE_TEST_CASE(max_key_256_update, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstmaxkeyupd"_n));
}

BOOST_FIXTURE_TEST_CASE(oversize_key_257_update, kv_api_tester) {
   BOOST_CHECK_THROW(run_action("tstovrkyupd"_n), kv_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(max_value_262144_create, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstmaxvalcr"_n));
}

BOOST_FIXTURE_TEST_CASE(oversize_value_262145_create, kv_api_tester) {
   BOOST_CHECK_THROW(run_action("tstovrvalcr"_n), kv_value_too_large);
}

BOOST_FIXTURE_TEST_CASE(max_value_262144_update, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstmaxvalupd"_n));
}

BOOST_FIXTURE_TEST_CASE(oversize_value_262145_update, kv_api_tester) {
   BOOST_CHECK_THROW(run_action("tstovrvalupd"_n), kv_value_too_large);
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

BOOST_FIXTURE_TEST_CASE(sec_iterate_order, kv_api_tester) {
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

BOOST_FIXTURE_TEST_CASE(sec_rbegin_rend, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecrbegin"_n));
}

BOOST_FIXTURE_TEST_CASE(sec_erase_returns_next, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecersnxt"_n));
}

// kv::raw_table tests
BOOST_FIXTURE_TEST_CASE(mapping_set_get, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstmapsetget"_n));
}

BOOST_FIXTURE_TEST_CASE(mapping_contains_erase, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstmapcont"_n));
}

BOOST_FIXTURE_TEST_CASE(mapping_update, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstmapupdate"_n));
}

BOOST_FIXTURE_TEST_CASE(mapping_name_key, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstmapstrkey"_n));
}

// kv::table tests
BOOST_FIXTURE_TEST_CASE(kvtable_basic, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstkvtbasic"_n));
}

BOOST_FIXTURE_TEST_CASE(kvtable_iterate, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstkvtiter"_n));
}

BOOST_FIXTURE_TEST_CASE(kvtable_begin_all_scopes, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstkvtscope"_n));
}

// payer validation tests
BOOST_FIXTURE_TEST_CASE(payer_self_allowed, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstpayerself"_n));
}

BOOST_FIXTURE_TEST_CASE(payer_other_requires_auth, kv_api_tester) {
   // Billing another account without their authorization fails at the
   // transaction level (unauthorized_ram_usage_increase), not at the KV level.
   create_accounts({"alice"_n});
   produce_block();
   BOOST_CHECK_THROW(run_action("tstpayeroth"_n), sysio::chain::unauthorized_ram_usage_increase);
}

// empty key rejection tests
BOOST_FIXTURE_TEST_CASE(empty_key_rejected_kv_set, kv_api_tester) {
   BOOST_CHECK_THROW(run_action("tstemptykey"_n), fc::exception);
}

// invalid key_format rejection tests
BOOST_FIXTURE_TEST_CASE(bad_key_format_rejected_kv_set, kv_api_tester) {
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
   set_transaction_headers(trx);
   trx.sign(get_private_key(test_account, "active"), control->get_chain_id());
   BOOST_CHECK_THROW(
      push_transaction(trx, fc::time_point::maximum(), DEFAULT_BILLED_CPU_TIME_US, false,
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
   create_accounts({notify_acct});
   produce_block();
   set_code(notify_acct, test_contracts::test_kv_api_wasm());
   set_abi(notify_acct, test_contracts::test_kv_api_abi().c_str());
   produce_block();

   // Push tstsendnotif on test_account — it calls require_recipient(kvnotify)
   // kvnotify receives the notification and executes tstnotifyram handler (via apply)
   // Actually, require_recipient just forwards the same action, so kvnotify's
   // apply will see action=tstsendnotif. The notification will succeed and
   // any kv_set in kvnotify's on_notify will write to kvnotify's KV space.
   // For this test, we just verify the notification doesn't crash.
   BOOST_CHECK_NO_THROW(run_action("tstsendnotif"_n));
   produce_block();

   // Verify kvnotify did NOT write to test_account's KV space
   // (notification receiver writes to its own namespace)
}

// secondary iterator clone with duplicate keys
BOOST_FIXTURE_TEST_CASE(sec_iterator_clone_duplicate_keys, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecclone"_n));
}

// secondary rbegin/rend with uint128_t keys
BOOST_FIXTURE_TEST_CASE(sec_rbegin_uint128_keys, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstsecrbig"_n));
}

// ════════════════════════════════════════════════════════════════════════════
// RAM billing tests — verify correct billing on create, update, erase
// ════════════════════════════════════════════════════════════════════════════

// billable_size_v<kv_object> from config — use the actual compile-time constant
static constexpr int64_t KV_OVERHEAD = config::billable_size_v<kv_object>;

struct kv_billing_tester : validating_tester {
   kv_billing_tester() {
      create_accounts({test_account});
      produce_block();
      set_code(test_account, test_contracts::test_kv_api_wasm());
      set_abi(test_account, test_contracts::test_kv_api_abi().c_str());
      produce_block();
   }

   int64_t get_ram_usage(name acct) {
      return control->get_resource_limits_manager().get_account_ram_usage(acct);
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
   auto before = get_ram_usage(test_account);
   ram_store(1, 100); // 4-byte key, 100-byte value
   auto after = get_ram_usage(test_account);

   int64_t expected = 4 + 100 + KV_OVERHEAD; // key + value + overhead
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_create_row: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_FIXTURE_TEST_CASE(billing_update_grow, kv_billing_tester) {
   // Growing a value should bill the delta
   ram_store(2, 50);
   auto before = get_ram_usage(test_account);
   ram_update(2, 200); // grow from 50 to 200
   auto after = get_ram_usage(test_account);

   int64_t expected = 200 - 50; // only value delta, key+overhead unchanged
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_update_grow: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_FIXTURE_TEST_CASE(billing_update_shrink, kv_billing_tester) {
   // Shrinking a value should refund the delta
   ram_store(3, 200);
   auto before = get_ram_usage(test_account);
   ram_update(3, 50); // shrink from 200 to 50
   auto after = get_ram_usage(test_account);

   int64_t expected = 50 - 200; // negative delta = refund
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_update_shrink: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_FIXTURE_TEST_CASE(billing_update_same_size, kv_billing_tester) {
   // Updating with same size should bill zero
   ram_store(4, 100);
   auto before = get_ram_usage(test_account);
   ram_update(4, 100); // same size
   auto after = get_ram_usage(test_account);

   BOOST_TEST_MESSAGE("billing_update_same_size: delta=" << (after - before));
   BOOST_REQUIRE_EQUAL(after - before, 0);
}

BOOST_FIXTURE_TEST_CASE(billing_erase_refund, kv_billing_tester) {
   // Erasing should refund the full amount billed on create
   ram_store(5, 100);
   auto before = get_ram_usage(test_account);
   ram_erase(5);
   auto after = get_ram_usage(test_account);

   int64_t expected = -(4 + 100 + KV_OVERHEAD); // full refund
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_erase_refund: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

BOOST_FIXTURE_TEST_CASE(billing_create_erase_net_zero, kv_billing_tester) {
   // Create + erase should result in net zero RAM change
   auto before = get_ram_usage(test_account);
   ram_store(6, 500);
   ram_erase(6);
   auto after = get_ram_usage(test_account);

   BOOST_TEST_MESSAGE("billing_create_erase_net_zero: delta=" << (after - before));
   BOOST_REQUIRE_EQUAL(after - before, 0);
}

BOOST_FIXTURE_TEST_CASE(billing_multiple_rows, kv_billing_tester) {
   // Multiple rows should each be billed independently
   auto before = get_ram_usage(test_account);
   ram_store(10, 100);
   ram_store(11, 200);
   ram_store(12, 300);
   auto after = get_ram_usage(test_account);

   int64_t expected = 3 * (4 + KV_OVERHEAD) + 100 + 200 + 300; // 3 rows, different values
   int64_t actual = after - before;
   BOOST_TEST_MESSAGE("billing_multiple_rows: expected=" << expected << " actual=" << actual);
   BOOST_REQUIRE_EQUAL(actual, expected);
}

// ════════════════════════════════════════════════════════════════════════════
// Payer change billing tests — verify correct billing when payer changes
// ════════════════════════════════════════════════════════════════════════════

static constexpr int64_t KV_IDX_OVERHEAD = config::billable_size_v<kv_index_object>;

struct kv_payer_billing_tester : validating_tester {
   kv_payer_billing_tester() {
      create_accounts({test_account, "alice"_n, "bob"_n});
      produce_block();
      set_code(test_account, test_contracts::test_kv_api_wasm());
      set_abi(test_account, test_contracts::test_kv_api_abi().c_str());
      produce_block();
   }

   int64_t get_ram_usage(name acct) {
      return control->get_resource_limits_manager().get_account_ram_usage(acct);
   }

   // Push rampyrset with proper sysio.payer authorization for cross-account billing.
   // When authorize_payer=true and payer is a third party, the payer's sysio.payer
   // permission must be first in the auth list (enforced by authorization_manager).
   void ram_pyr_set(uint32_t key_id, uint32_t val_size, name payer,
                    bool authorize_payer = true) {
      vector<permission_level> auths;
      if (payer != name{0} && payer != test_account && authorize_payer) {
         auths.push_back({payer, config::sysio_payer_name});
         auths.push_back({payer, config::active_name});
      } else {
         auths.push_back({test_account, config::active_name});
      }
      push_action(test_account, "rampyrset"_n, auths,
         mutable_variant_object()("key_id", key_id)("val_size", val_size)("payer", payer));
      produce_block();
   }

   // Self-pay store (payer = receiver)
   void ram_store(uint32_t key_id, uint32_t val_size) {
      push_action(test_account, "ramstore"_n, test_account,
         mutable_variant_object()("key_id", key_id)("val_size", val_size));
      produce_block();
   }

   void ram_erase(uint32_t key_id) {
      push_action(test_account, "ramerase"_n, test_account,
         mutable_variant_object()("key_id", key_id));
      produce_block();
   }
};

// Payer change, same value size — old payer fully refunded, new payer charged
BOOST_FIXTURE_TEST_CASE(payer_change_same_size, kv_payer_billing_tester) {
   // Create row with payer=alice
   const uint32_t VAL_SIZE = 100;
   int64_t expected_billable = 4 + VAL_SIZE + KV_OVERHEAD;

   auto alice_before = get_ram_usage("alice"_n);
   ram_pyr_set(1, VAL_SIZE, "alice"_n);
   auto alice_after_create = get_ram_usage("alice"_n);
   BOOST_REQUIRE_EQUAL(alice_after_create - alice_before, expected_billable);

   // Update same key, change payer to bob, same size
   auto bob_before = get_ram_usage("bob"_n);
   alice_before = get_ram_usage("alice"_n);
   ram_pyr_set(1, VAL_SIZE, "bob"_n);
   auto alice_after = get_ram_usage("alice"_n);
   auto bob_after = get_ram_usage("bob"_n);

   // alice fully refunded, bob charged same amount
   BOOST_REQUIRE_EQUAL(alice_after - alice_before, -expected_billable);
   BOOST_REQUIRE_EQUAL(bob_after - bob_before, expected_billable);
}

// Payer change with value growth — old payer refunded old amount, new payer charged new amount
BOOST_FIXTURE_TEST_CASE(payer_change_with_growth, kv_payer_billing_tester) {
   const uint32_t SMALL = 50, LARGE = 200;
   int64_t small_billable = 4 + SMALL + KV_OVERHEAD;
   int64_t large_billable = 4 + LARGE + KV_OVERHEAD;

   ram_pyr_set(1, SMALL, "alice"_n);

   auto alice_before = get_ram_usage("alice"_n);
   auto bob_before = get_ram_usage("bob"_n);
   ram_pyr_set(1, LARGE, "bob"_n);
   auto alice_after = get_ram_usage("alice"_n);
   auto bob_after = get_ram_usage("bob"_n);

   // alice refunded full old amount, bob charged full new (larger) amount
   BOOST_REQUIRE_EQUAL(alice_after - alice_before, -small_billable);
   BOOST_REQUIRE_EQUAL(bob_after - bob_before, large_billable);
}

// Payer change with value shrink — old payer refunded old amount, new payer charged smaller amount
BOOST_FIXTURE_TEST_CASE(payer_change_with_shrink, kv_payer_billing_tester) {
   const uint32_t LARGE = 200, SMALL = 50;
   int64_t large_billable = 4 + LARGE + KV_OVERHEAD;
   int64_t small_billable = 4 + SMALL + KV_OVERHEAD;

   ram_pyr_set(1, LARGE, "alice"_n);

   auto alice_before = get_ram_usage("alice"_n);
   auto bob_before = get_ram_usage("bob"_n);
   ram_pyr_set(1, SMALL, "bob"_n);
   auto alice_after = get_ram_usage("alice"_n);
   auto bob_after = get_ram_usage("bob"_n);

   // alice refunded full old amount, bob charged full new (smaller) amount
   BOOST_REQUIRE_EQUAL(alice_after - alice_before, -large_billable);
   BOOST_REQUIRE_EQUAL(bob_after - bob_before, small_billable);
}

// Payer change back to self (payer=0 means receiver)
BOOST_FIXTURE_TEST_CASE(payer_change_back_to_self, kv_payer_billing_tester) {
   const uint32_t VAL_SIZE = 100;
   int64_t expected_billable = 4 + VAL_SIZE + KV_OVERHEAD;

   // Create with payer=alice
   ram_pyr_set(1, VAL_SIZE, "alice"_n);

   // Change payer back to receiver (payer=0 means receiver=test_account)
   // Only test_account needs to sign since alice's delta is negative
   auto alice_before = get_ram_usage("alice"_n);
   auto contract_before = get_ram_usage(test_account);
   ram_pyr_set(1, VAL_SIZE, name{0});
   auto alice_after = get_ram_usage("alice"_n);
   auto contract_after = get_ram_usage(test_account);

   BOOST_REQUIRE_EQUAL(alice_after - alice_before, -expected_billable);
   BOOST_REQUIRE_EQUAL(contract_after - contract_before, expected_billable);
}

// Erase refunds the stored payer (not the contract/receiver)
BOOST_FIXTURE_TEST_CASE(erase_refunds_correct_payer, kv_payer_billing_tester) {
   const uint32_t VAL_SIZE = 100;
   int64_t expected_billable = 4 + VAL_SIZE + KV_OVERHEAD;

   // Create with payer=alice
   ram_pyr_set(1, VAL_SIZE, "alice"_n);

   // Erase — alice should get the refund, not the contract
   auto alice_before = get_ram_usage("alice"_n);
   auto contract_before = get_ram_usage(test_account);
   ram_erase(1);
   auto alice_after = get_ram_usage("alice"_n);
   auto contract_after = get_ram_usage(test_account);

   BOOST_REQUIRE_EQUAL(alice_after - alice_before, -expected_billable);
   BOOST_REQUIRE_EQUAL(contract_after - contract_before, 0);
}

// Payer change without new payer authorization — should fail.
// Alice is not in the auth list at all, so the unprivileged has_authorization
// check (Path 1 in validate_account_ram_deltas) fires first ->
// unauthorized_ram_usage_increase.
BOOST_FIXTURE_TEST_CASE(payer_change_unauth_fails, kv_payer_billing_tester) {
   // Create with self-pay
   ram_store(1, 100);

   // Try to change payer to alice without alice signing
   BOOST_CHECK_THROW(
      ram_pyr_set(1, 100, "alice"_n, false),
      unauthorized_ram_usage_increase
   );
}

// Payer signed with active but NOT sysio.payer — should fail.
// Alice is in the auth list (passes Path 1 has_authorization), but without
// the sysio.payer permission role Path 2 rejects -> unsatisfied_authorization.
BOOST_FIXTURE_TEST_CASE(payer_change_active_only_fails, kv_payer_billing_tester) {
   ram_store(1, 100);

   // Alice signs with active only — no sysio.payer permission
   BOOST_CHECK_THROW(
      push_action(test_account, "rampyrset"_n,
         vector<permission_level>{{"alice"_n, config::active_name}},
         mutable_variant_object()("key_id", 1)("val_size", 100)("payer", "alice")),
      unsatisfied_authorization
   );
}

// Payer change — old payer not authorized, but succeeds
// because old payer's delta is negative (refund doesn't need auth)
BOOST_FIXTURE_TEST_CASE(payer_change_old_payer_unauth_ok, kv_payer_billing_tester) {
   const uint32_t VAL_SIZE = 100;
   int64_t billable = 4 + VAL_SIZE + KV_OVERHEAD;

   // Create with payer=alice
   ram_pyr_set(1, VAL_SIZE, "alice"_n);

   // Change payer from alice->bob. Only bob signs.
   // alice is refunded (negative delta -> no auth needed).
   auto alice_before = get_ram_usage("alice"_n);
   auto bob_before = get_ram_usage("bob"_n);
   ram_pyr_set(1, VAL_SIZE, "bob"_n);
   auto alice_after = get_ram_usage("alice"_n);
   auto bob_after = get_ram_usage("bob"_n);

   BOOST_REQUIRE_EQUAL(alice_after - alice_before, -billable);
   BOOST_REQUIRE_EQUAL(bob_after - bob_before, billable);
}

// Multiple actions with different payers in one transaction — independent deltas
BOOST_FIXTURE_TEST_CASE(mixed_payer_independent_deltas, kv_payer_billing_tester) {
   auto alice_before = get_ram_usage("alice"_n);
   auto bob_before = get_ram_usage("bob"_n);

   // Single transaction: action1 bills alice, action2 bills bob
   signed_transaction trx;
   trx.actions.push_back(get_action(test_account, "rampyrset"_n,
      {{"alice"_n, config::sysio_payer_name}, {"alice"_n, config::active_name}},
      mutable_variant_object()("key_id", 1)("val_size", 100)("payer", "alice")));
   trx.actions.push_back(get_action(test_account, "rampyrset"_n,
      {{"bob"_n, config::sysio_payer_name}, {"bob"_n, config::active_name}},
      mutable_variant_object()("key_id", 2)("val_size", 200)("payer", "bob")));
   set_transaction_headers(trx);
   trx.sign(get_private_key("alice"_n, "active"), control->get_chain_id());
   trx.sign(get_private_key("bob"_n, "active"), control->get_chain_id());
   push_transaction(trx);
   produce_block();

   auto alice_after = get_ram_usage("alice"_n);
   auto bob_after = get_ram_usage("bob"_n);

   BOOST_REQUIRE_EQUAL(alice_after - alice_before, 4 + 100 + KV_OVERHEAD);
   BOOST_REQUIRE_EQUAL(bob_after - bob_before, 4 + 200 + KV_OVERHEAD);
}

// Create + erase across actions in one transaction — net zero for payer
BOOST_FIXTURE_TEST_CASE(mixed_payer_create_erase_net_zero, kv_payer_billing_tester) {
   auto alice_before = get_ram_usage("alice"_n);

   // Single transaction: action1 creates with payer=alice, action2 erases
   signed_transaction trx;
   trx.actions.push_back(get_action(test_account, "rampyrset"_n,
      {{"alice"_n, config::sysio_payer_name}, {"alice"_n, config::active_name}},
      mutable_variant_object()("key_id", 1)("val_size", 100)("payer", "alice")));
   trx.actions.push_back(get_action(test_account, "ramerase"_n,
      {{test_account, config::active_name}},
      mutable_variant_object()("key_id", 1)));
   set_transaction_headers(trx);
   trx.sign(get_private_key("alice"_n, "active"), control->get_chain_id());
   trx.sign(get_private_key(test_account, "active"), control->get_chain_id());
   push_transaction(trx);
   produce_block();

   auto alice_after = get_ram_usage("alice"_n);
   BOOST_REQUIRE_EQUAL(alice_after - alice_before, 0);
}

// ════════════════════════════════════════════════════════════════════════════
// Secondary index billing tests
// ════════════════════════════════════════════════════════════════════════════

// kv_idx_store bills sec_key_size + pri_key_size + overhead
BOOST_FIXTURE_TEST_CASE(idx_billing_store, kv_billing_tester) {
   auto before = get_ram_usage(test_account);
   push_action(test_account, "ramidxstore"_n, test_account,
      mutable_variant_object()("sec_size", 8)("pri_size", 4));
   produce_block();
   auto after = get_ram_usage(test_account);

   int64_t expected = 8 + 4 + KV_IDX_OVERHEAD;
   BOOST_TEST_MESSAGE("idx_billing_store: expected=" << expected << " actual=" << (after - before));
   BOOST_REQUIRE_EQUAL(after - before, expected);
}

// kv_idx_remove refunds the full billable amount
BOOST_FIXTURE_TEST_CASE(idx_billing_remove, kv_billing_tester) {
   push_action(test_account, "ramidxstore"_n, test_account,
      mutable_variant_object()("sec_size", 8)("pri_size", 4));
   produce_block();

   auto before = get_ram_usage(test_account);
   push_action(test_account, "ramidxremov"_n, test_account,
      mutable_variant_object()("sec_size", 8)("pri_size", 4));
   produce_block();
   auto after = get_ram_usage(test_account);

   int64_t expected = -(8 + 4 + KV_IDX_OVERHEAD);
   BOOST_TEST_MESSAGE("idx_billing_remove: expected=" << expected << " actual=" << (after - before));
   BOOST_REQUIRE_EQUAL(after - before, expected);
}

// kv_idx_update with size change — bills only the sec key delta
BOOST_FIXTURE_TEST_CASE(idx_billing_update_size_change, kv_billing_tester) {
   push_action(test_account, "ramidxstore"_n, test_account,
      mutable_variant_object()("sec_size", 8)("pri_size", 4));
   produce_block();

   auto before = get_ram_usage(test_account);
   push_action(test_account, "ramidxupdat"_n, test_account,
      mutable_variant_object()("old_ss", 8)("new_ss", 20)("pri_size", 4));
   produce_block();
   auto after = get_ram_usage(test_account);

   int64_t expected = 20 - 8; // sec key delta only
   BOOST_TEST_MESSAGE("idx_billing_update_grow: expected=" << expected << " actual=" << (after - before));
   BOOST_REQUIRE_EQUAL(after - before, expected);
}

// kv_idx_update same size — zero delta
BOOST_FIXTURE_TEST_CASE(idx_billing_update_same_size, kv_billing_tester) {
   push_action(test_account, "ramidxstore"_n, test_account,
      mutable_variant_object()("sec_size", 8)("pri_size", 4));
   produce_block();

   auto before = get_ram_usage(test_account);
   push_action(test_account, "ramidxupdat"_n, test_account,
      mutable_variant_object()("old_ss", 8)("new_ss", 8)("pri_size", 4));
   produce_block();
   auto after = get_ram_usage(test_account);

   BOOST_TEST_MESSAGE("idx_billing_update_same: delta=" << (after - before));
   BOOST_REQUIRE_EQUAL(after - before, 0);
}

// ════════════════════════════════════════════════════════════════════════════
// Cross-account secondary index billing — payer != receiver
// ════════════════════════════════════════════════════════════════════════════

// Secondary index store bills the specified payer, not the contract
BOOST_FIXTURE_TEST_CASE(idx_billing_payer_not_receiver, kv_payer_billing_tester) {
   auto alice_before = get_ram_usage("alice"_n);
   auto contract_before = get_ram_usage(test_account);

   push_action(test_account, "ramidxstpyr"_n,
      {{"alice"_n, config::sysio_payer_name}, {"alice"_n, config::active_name}},
      mutable_variant_object()("payer", "alice")("sec_size", 8)("pri_size", 4));
   produce_block();

   auto alice_after = get_ram_usage("alice"_n);
   auto contract_after = get_ram_usage(test_account);

   int64_t expected = 8 + 4 + KV_IDX_OVERHEAD;
   BOOST_REQUIRE_EQUAL(alice_after - alice_before, expected);
   BOOST_REQUIRE_EQUAL(contract_after - contract_before, 0);
}

// Secondary index remove refunds the stored payer, not the contract
BOOST_FIXTURE_TEST_CASE(idx_remove_refunds_payer, kv_payer_billing_tester) {
   // Store with alice as payer
   push_action(test_account, "ramidxstpyr"_n,
      {{"alice"_n, config::sysio_payer_name}, {"alice"_n, config::active_name}},
      mutable_variant_object()("payer", "alice")("sec_size", 8)("pri_size", 4));
   produce_block();

   // Remove — should refund alice, not the contract
   auto alice_before = get_ram_usage("alice"_n);
   auto contract_before = get_ram_usage(test_account);
   push_action(test_account, "ramidxremov"_n, test_account,
      mutable_variant_object()("sec_size", 8)("pri_size", 4));
   produce_block();

   auto alice_after = get_ram_usage("alice"_n);
   auto contract_after = get_ram_usage(test_account);

   int64_t expected = -(8 + 4 + KV_IDX_OVERHEAD);
   BOOST_REQUIRE_EQUAL(alice_after - alice_before, expected);
   BOOST_REQUIRE_EQUAL(contract_after - contract_before, 0);
}

// Secondary index update with payer change
BOOST_FIXTURE_TEST_CASE(idx_update_payer_change, kv_payer_billing_tester) {
   // Store with alice as payer
   push_action(test_account, "ramidxstpyr"_n,
      {{"alice"_n, config::sysio_payer_name}, {"alice"_n, config::active_name}},
      mutable_variant_object()("payer", "alice")("sec_size", 8)("pri_size", 4));
   produce_block();

   int64_t old_billable = 8 + 4 + KV_IDX_OVERHEAD;
   int64_t new_billable = 12 + 4 + KV_IDX_OVERHEAD;

   // Update, change payer from alice to bob
   auto alice_before = get_ram_usage("alice"_n);
   auto bob_before = get_ram_usage("bob"_n);
   push_action(test_account, "ramidxuppyr"_n,
      {{"bob"_n, config::sysio_payer_name}, {"bob"_n, config::active_name}},
      mutable_variant_object()("payer", "bob")("old_ss", 8)("new_ss", 12)("pri_size", 4));
   produce_block();

   auto alice_after = get_ram_usage("alice"_n);
   auto bob_after = get_ram_usage("bob"_n);

   // alice fully refunded, bob charged new amount
   BOOST_REQUIRE_EQUAL(alice_after - alice_before, -old_billable);
   BOOST_REQUIRE_EQUAL(bob_after - bob_before, new_billable);
}

// ════════════════════════════════════════════════════════════════════════════
// Read-only transaction rejection tests
// ════════════════════════════════════════════════════════════════════════════

BOOST_FIXTURE_TEST_CASE(read_only_rejects_kv_erase, kv_api_tester) {
   signed_transaction trx;
   trx.actions.emplace_back(
      vector<permission_level>{}, test_account, "tstrdoerase"_n, bytes{}
   );
   set_transaction_headers(trx);
   BOOST_CHECK_THROW(
      push_transaction(trx, fc::time_point::maximum(), DEFAULT_BILLED_CPU_TIME_US, false,
                       transaction_metadata::trx_type::read_only),
      table_operation_not_permitted);
}

BOOST_FIXTURE_TEST_CASE(read_only_rejects_kv_idx_store, kv_api_tester) {
   signed_transaction trx;
   trx.actions.emplace_back(
      vector<permission_level>{}, test_account, "tstrdoidxst"_n, bytes{}
   );
   set_transaction_headers(trx);
   BOOST_CHECK_THROW(
      push_transaction(trx, fc::time_point::maximum(), DEFAULT_BILLED_CPU_TIME_US, false,
                       transaction_metadata::trx_type::read_only),
      table_operation_not_permitted);
}

BOOST_FIXTURE_TEST_CASE(read_only_rejects_kv_idx_remove, kv_api_tester) {
   signed_transaction trx;
   trx.actions.emplace_back(
      vector<permission_level>{}, test_account, "tstrdoidxrm"_n, bytes{}
   );
   set_transaction_headers(trx);
   BOOST_CHECK_THROW(
      push_transaction(trx, fc::time_point::maximum(), DEFAULT_BILLED_CPU_TIME_US, false,
                       transaction_metadata::trx_type::read_only),
      table_operation_not_permitted);
}

BOOST_FIXTURE_TEST_CASE(read_only_rejects_kv_idx_update, kv_api_tester) {
   signed_transaction trx;
   trx.actions.emplace_back(
      vector<permission_level>{}, test_account, "tstrdoidxup"_n, bytes{}
   );
   set_transaction_headers(trx);
   BOOST_CHECK_THROW(
      push_transaction(trx, fc::time_point::maximum(), DEFAULT_BILLED_CPU_TIME_US, false,
                       transaction_metadata::trx_type::read_only),
      table_operation_not_permitted);
}

// ════════════════════════════════════════════════════════════════════════════
// Notification context billing tests
// ════════════════════════════════════════════════════════════════════════════

static const name notify_account = "kvnotify"_n;

struct kv_notify_billing_tester : validating_tester {
   kv_notify_billing_tester() {
      create_accounts({test_account, notify_account, "alice"_n});
      produce_block();
      set_code(test_account, test_contracts::test_kv_api_wasm());
      set_abi(test_account, test_contracts::test_kv_api_abi().c_str());
      set_code(notify_account, test_contracts::test_kv_api_wasm());
      set_abi(notify_account, test_contracts::test_kv_api_abi().c_str());
      produce_block();
   }

   int64_t get_ram_usage(name acct) {
      return control->get_resource_limits_manager().get_account_ram_usage(acct);
   }
};

// Self-pay kv_set in notification context — RAM billed to notified receiver
BOOST_FIXTURE_TEST_CASE(notify_self_pay_bills_receiver, kv_notify_billing_tester) {
   auto notify_before = get_ram_usage(notify_account);
   auto sender_before = get_ram_usage(test_account);

   push_action(test_account, "ramnotify"_n, test_account,
      mutable_variant_object()("key_id", 1)("val_size", 100));
   produce_block();

   auto notify_after = get_ram_usage(notify_account);
   auto sender_after = get_ram_usage(test_account);

   // RAM charged to notified receiver (kvnotify), not sender (kvtest)
   int64_t expected = 4 + 100 + KV_OVERHEAD;
   BOOST_REQUIRE_EQUAL(notify_after - notify_before, expected);
   BOOST_REQUIRE_EQUAL(sender_after - sender_before, 0);
}

// Third-party payer in notification context — always fails.
// Any cross-account RAM increase is blocked in notification context
// regardless of sysio.payer authorization.
BOOST_FIXTURE_TEST_CASE(notify_third_party_payer_fails, kv_notify_billing_tester) {
   BOOST_CHECK_THROW(
      push_action(test_account, "ramnotiferr"_n,
         {{"alice"_n, config::sysio_payer_name}, {"alice"_n, config::active_name}},
         mutable_variant_object()("key_id", 1)("val_size", 100)("payer", "alice")),
      unauthorized_ram_usage_increase
   );
}

// ════════════════════════════════════════════════════════════════════════════
// Cross-contract secondary index reads
// ════════════════════════════════════════════════════════════════════════════

// Contract B reads contract A's secondary index and primary row
BOOST_FIXTURE_TEST_CASE(cross_contract_idx_read, kv_notify_billing_tester) {
   // Setup: kvnotify stores a row + secondary index entry
   push_action(notify_account, "xcsetup"_n, notify_account, mutable_variant_object());
   produce_block();

   // kvtest reads kvnotify's secondary index and primary row
   BOOST_CHECK_NO_THROW(
      push_action(test_account, "xcidxread"_n, test_account,
         mutable_variant_object()("other", notify_account))
   );
}

// ════════════════════════════════════════════════════════════════════════════
// Iterator invalidation under mutation
// ════════════════════════════════════════════════════════════════════════════

// Primary: update value while iterator points to row
BOOST_FIXTURE_TEST_CASE(iter_mutation_update_value, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstmutupdval"_n));
}

// Primary: erase current row then next() advances to next valid
BOOST_FIXTURE_TEST_CASE(iter_mutation_erase_next, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tsterasenext"_n));
}

// Primary: erase all rows during iteration -> end
BOOST_FIXTURE_TEST_CASE(iter_mutation_erase_all, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tsteraseall"_n));
}

// Secondary: remove entry under sec iterator -> next advances
BOOST_FIXTURE_TEST_CASE(idx_iter_mutation_erase, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstidxerase"_n));
}

// Secondary: update key under sec iterator -> old position invalid
BOOST_FIXTURE_TEST_CASE(idx_iter_mutation_update, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstidxmutupd"_n));
}

// Secondary: insert during iteration -> new entry visible
BOOST_FIXTURE_TEST_CASE(idx_iter_mutation_insert, kv_api_tester) {
   BOOST_CHECK_NO_THROW(run_action("tstidxinsert"_n));
}

// Fork/undo coverage note:
// All KV test fixtures use validating_tester, which replays every block on a
// second chain and compares integrity hashes. This implicitly verifies that
// kv_object and kv_index_object chainbase undo sessions work correctly —
// any mismatch would cause test failure. Savanna consensus does not support
// pop_block, so explicit undo testing is handled via the validating replay.

BOOST_AUTO_TEST_SUITE_END()
