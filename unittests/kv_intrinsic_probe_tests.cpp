// Boost.Test driver for the kv_intrinsic_probe contract.
// Exercises adversarial inputs against the raw kv_* host intrinsics --
// inputs CDT wrappers would never emit but a malicious contract can.

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <test_contracts.hpp>

#include <array>
#include "kv_write_driver.hpp"

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

static const name probe_account = "kvprobe"_n;
static const name notified_a    = "kvprobea"_n;
static const name notified_b    = "kvprobeb"_n;
// Sorts below every other account a test here creates, so its rows can be the first of the whole index.
static const name first_account = "aaaprobe"_n;
// Sorts above every account: the largest name an account can have, since a 13th character is refused.
static const name last_account  = "zzzzzzzzzzzz"_n;
static const name edge_account  = "kvedge"_n;
static const name cf_account    = "kvcfa"_n;

// The first and last bytes of a contract with one page of memory, and a 4-byte size output that just fits.
static constexpr uint32_t page_size     = 64 * 1024;
static constexpr uint32_t last_size_ptr = page_size - sizeof(uint32_t);

// Calls one KV intrinsic with an address and a size the action data names, against one page of memory. CDT code cannot
// name the end of its memory, so this is written by hand. The apply below has one branch per kv_edge_call.
static constexpr char kv_edge_wast[] = R"=====(
(module
  (import "env" "read_action_data" (func $read_action_data (param i32 i32) (result i32)))
  (import "env" "kv_set" (func $kv_set (param i32 i64 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_get" (func $kv_get (param i32 i64 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_store" (func $kv_idx_store (param i32 i64 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_create" (func $kv_it_create (param i32 i64 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_read" (func $kv_it_read (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (memory 1)
  (export "memory" (memory 0))
  (data (i32.const 64) "k")
  (func $first_row (param $receiver i64) (result i32)
    (call $kv_it_create (i32.const 1) (get_local $receiver) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0)))
  (func (export "apply") (param $receiver i64) (param i64 i64)
    (local $call i32) (local $addr i32) (local $size i32)
    (drop (call $read_action_data (i32.const 0) (i32.const 12)))
    (set_local $call (i32.load (i32.const 0)))
    (set_local $addr (i32.load (i32.const 4)))
    (set_local $size (i32.load (i32.const 8)))
    (if (i32.eq (get_local $call) (i32.const 0))
      (then (drop (call $kv_set (i32.const 1) (get_local $receiver) (get_local $addr) (get_local $size)
                     (i32.const 64) (i32.const 1) (i32.const 0)))))
    (if (i32.eq (get_local $call) (i32.const 1))
      (then (drop (call $kv_set (i32.const 1) (get_local $receiver) (i32.const 64) (i32.const 1)
                     (get_local $addr) (get_local $size) (i32.const 0)))))
    (if (i32.eq (get_local $call) (i32.const 2))
      (then (drop (call $kv_get (i32.const 1) (get_local $receiver) (i32.const 64) (i32.const 1) (i32.const 0)
                     (get_local $addr) (get_local $size)))))
    (if (i32.eq (get_local $call) (i32.const 3))
      (then (drop (call $kv_it_read (call $first_row (get_local $receiver)) (i32.const 0)
                     (get_local $addr) (get_local $size) (i32.const 0) (i32.const 0) (i32.const 72) (i32.const 76)))))
    (if (i32.eq (get_local $call) (i32.const 4))
      (then (drop (call $kv_it_read (call $first_row (get_local $receiver)) (i32.const 0)
                     (i32.const 0) (i32.const 0) (get_local $addr) (get_local $size) (i32.const 72) (i32.const 76)))))
    (if (i32.eq (get_local $call) (i32.const 5))
      (then (drop (call $kv_it_read (call $first_row (get_local $receiver)) (i32.const 0)
                     (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0) (get_local $addr) (i32.const 76)))))
    (if (i32.eq (get_local $call) (i32.const 6))
      (then (drop (call $kv_it_read (call $first_row (get_local $receiver)) (i32.const 0)
                     (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 72) (get_local $addr)))))
    (if (i32.eq (get_local $call) (i32.const 7))
      (then (drop (call $kv_idx_store (i32.const 1) (get_local $receiver) (get_local $addr) (get_local $size)
                     (i32.const 64) (i32.const 1)))))
  )
)
)=====";

// The argument of kv_edge_wast that an edge test moves around.
enum class kv_edge_call : uint32_t {
   key_span, value_span, get_output, read_key_buffer, read_value_buffer, read_first_size, read_second_size, pri_span
};

// Calls the KV intrinsic the action data names, one branch per intrinsic in declaration order. Id 19 calls nothing, and
// proves the module runs. Every argument is harmless, so only the context-free check can stop a call.
static constexpr char kv_context_free_wast[] = R"=====(
(module
  (import "env" "read_action_data" (func $read_action_data (param i32 i32) (result i32)))
  (import "env" "kv_set" (func $kv_set (param i32 i64 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_erase" (func $kv_erase (param i32 i32 i32) (result i32)))
  (import "env" "kv_get" (func $kv_get (param i32 i64 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_store" (func $kv_idx_store (param i32 i64 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_update" (func $kv_idx_update (param i32 i64 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_erase" (func $kv_idx_erase (param i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_create" (func $kv_it_create (param i32 i64 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_next" (func $kv_it_next (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_prev" (func $kv_it_prev (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_lower_bound" (func $kv_it_lower_bound (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_read" (func $kv_it_read (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_destroy" (func $kv_it_destroy (param i32)))
  (import "env" "kv_idx_create" (func $kv_idx_create (param i32 i64 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_next" (func $kv_idx_next (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_prev" (func $kv_idx_prev (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_lower_bound" (func $kv_idx_lower_bound (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_read" (func $kv_idx_read (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_destroy" (func $kv_idx_destroy (param i32)))
  (import "env" "kv_idx_find" (func $kv_idx_find (param i32 i64 i32 i32 i32 i32) (result i32)))
  (memory 1)
  (export "memory" (memory 0))
  (func (export "apply") (param i64 i64 i64)
    (local $call i32)
    (drop (call $read_action_data (i32.const 0) (i32.const 4)))
    (set_local $call (i32.load (i32.const 0)))
    (if (i32.eq (get_local $call) (i32.const 0)) (then (drop (call $kv_set (i32.const 1) (i64.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 0)))))
    (if (i32.eq (get_local $call) (i32.const 1)) (then (drop (call $kv_erase (i32.const 1) (i32.const 8) (i32.const 1)))))
    (if (i32.eq (get_local $call) (i32.const 2)) (then (drop (call $kv_get (i32.const 1) (i64.const 1) (i32.const 8) (i32.const 1) (i32.const 0) (i32.const 8) (i32.const 1)))))
    (if (i32.eq (get_local $call) (i32.const 3)) (then (drop (call $kv_idx_store (i32.const 1) (i64.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)))))
    (if (i32.eq (get_local $call) (i32.const 4)) (then (drop (call $kv_idx_update (i32.const 1) (i64.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)))))
    (if (i32.eq (get_local $call) (i32.const 5)) (then (drop (call $kv_idx_erase (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)))))
    (if (i32.eq (get_local $call) (i32.const 6)) (then (drop (call $kv_it_create (i32.const 1) (i64.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)))))
    (if (i32.eq (get_local $call) (i32.const 7)) (then (drop (call $kv_it_next (i32.const 0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16) (i32.const 20)))))
    (if (i32.eq (get_local $call) (i32.const 8)) (then (drop (call $kv_it_prev (i32.const 0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16) (i32.const 20)))))
    (if (i32.eq (get_local $call) (i32.const 9)) (then (drop (call $kv_it_lower_bound (i32.const 0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16) (i32.const 20)))))
    (if (i32.eq (get_local $call) (i32.const 10)) (then (drop (call $kv_it_read (i32.const 0) (i32.const 0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16) (i32.const 20)))))
    (if (i32.eq (get_local $call) (i32.const 11)) (then (call $kv_it_destroy (i32.const 0))))
    (if (i32.eq (get_local $call) (i32.const 12)) (then (drop (call $kv_idx_create (i32.const 1) (i64.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)))))
    (if (i32.eq (get_local $call) (i32.const 13)) (then (drop (call $kv_idx_next (i32.const 0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16) (i32.const 20)))))
    (if (i32.eq (get_local $call) (i32.const 14)) (then (drop (call $kv_idx_prev (i32.const 0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16) (i32.const 20)))))
    (if (i32.eq (get_local $call) (i32.const 15)) (then (drop (call $kv_idx_lower_bound (i32.const 0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16) (i32.const 20)))))
    (if (i32.eq (get_local $call) (i32.const 16)) (then (drop (call $kv_idx_read (i32.const 0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16) (i32.const 20)))))
    (if (i32.eq (get_local $call) (i32.const 17)) (then (call $kv_idx_destroy (i32.const 0))))
    (if (i32.eq (get_local $call) (i32.const 18)) (then (drop (call $kv_idx_find (i32.const 1) (i64.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)))))
  )
)
)=====";

// The intrinsic count, and the id that makes kv_context_free_wast call nothing.
static constexpr uint32_t kv_intrinsic_count = 19;

// Shared tester: deploy kv_intrinsic_probe once, reuse across cases.
// Each action uses distinct table_ids/keys so shared state is safe.
struct kv_probe_shared_tester : validating_tester {
   kv_probe_shared_tester() {
      create_accounts({probe_account, notified_a, notified_b, first_account, last_account, edge_account, cf_account});
      produce_block();
      for (name account : {probe_account, notified_a, notified_b, first_account, last_account}) {
         set_code(account, test_contracts::kv_intrinsic_probe_wasm());
         set_abi(account, test_contracts::kv_intrinsic_probe_abi().c_str());
      }
      set_code(edge_account, kv_edge_wast);
      set_code(cf_account, kv_context_free_wast);
      produce_block();
   }

   void run_action(name action_name) {
      signed_transaction trx;
      trx.actions.emplace_back(
         vector<permission_level>{{probe_account, config::active_name}},
         probe_account,
         action_name,
         bytes{}
      );
      set_transaction_headers(trx);
      trx.sign(get_private_key(probe_account, "active"), control->get_chain_id());
      push_transaction(trx);
      produce_block();
   }
};

struct kv_probe_tester {
   kv_probe_shared_tester& t;
   kv_probe_tester() : t(shared_instance()) {}
   void run_action(name n) { t.run_action(n); }
   int64_t ram_usage(name account = probe_account) const {
      return t.control->get_resource_limits_manager().get_account_ram_usage(account);
   }

   /// One call of kv_edge_wast, which passes `addr` and `size` as the argument `call` names.
   void edge(kv_edge_call call, uint32_t addr, uint32_t size) {
      signed_transaction trx;
      struct { uint32_t call, addr, size; } args{magic_enum::enum_integer(call), addr, size};
      trx.actions.emplace_back(vector<permission_level>{{edge_account, config::active_name}}, edge_account, "edge"_n,
                               bytes(reinterpret_cast<const char*>(&args), reinterpret_cast<const char*>(&args + 1)));
      t.set_transaction_headers(trx);
      trx.sign(t.get_private_key(edge_account, "active"), t.control->get_chain_id());
      t.push_transaction(trx);
      t.produce_block();
   }

   /// A context-free action that makes call `id` of kv_context_free_wast, and the ordinary action a transaction needs.
   void context_free_call(uint32_t id) {
      signed_transaction trx;
      trx.context_free_actions.emplace_back(vector<permission_level>{}, cf_account, "call"_n, fc::raw::pack(id));
      trx.context_free_data.emplace_back();
      trx.actions.emplace_back(vector<permission_level>{{cf_account, config::active_name}}, cf_account, "nothing"_n,
                               fc::raw::pack(kv_intrinsic_count));
      t.set_transaction_headers(trx);
      trx.sign(t.get_private_key(cf_account, "active"), t.control->get_chain_id());
      t.push_transaction(trx);
      t.produce_block();
   }

   /// What KV holds for the accounts an abort test compares. abrtseed ran first, so abort_work has rows to change.
   kv_write_driver::kv_state abort_seeded_state(name other = notified_b) {
      for (name account : {probe_account, notified_a}) {
         t.push_action(account, "abrtseed"_n, account, mutable_variant_object());
      }
      t.produce_block();
      return abort_state(other);
   }

   kv_write_driver::kv_state abort_state(name other = notified_b) const {
      return kv_write_driver::kv_state_of(t, {probe_account, notified_a, other});
   }
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


BOOST_FIXTURE_TEST_CASE(existence_probe_matches_read, kv_probe_tester) {
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
   BOOST_CHECK_THROW(run_action("zkeyset"_n), kv_empty_key);
}

BOOST_FIXTURE_TEST_CASE(kv_erase_empty_key_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("zkeyerz"_n), kv_empty_key);
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

BOOST_FIXTURE_TEST_CASE(kv_idx_store_oversize_pri_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("bigpri"_n), kv_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(kv_it_create_oversize_prefix_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("bigpfx"_n), kv_key_too_large);
}

BOOST_FIXTURE_TEST_CASE(table_id_above_uint16_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("tidmax"_n), kv_invalid_table_id);
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

BOOST_FIXTURE_TEST_CASE(iterator_slot_exhaustion, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("itlimit"_n), kv_iterator_limit_exceeded);
}

// =============================================================================
// D. Missing rows are a status, and the contract keeps running.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(kv_erase_missing_key_returns_no_row, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("erasmis"_n));
}

BOOST_FIXTURE_TEST_CASE(kv_idx_erase_missing_entry_returns_no_row, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("idxrmmis"_n));
}

BOOST_FIXTURE_TEST_CASE(kv_idx_update_missing_entry_returns_no_row, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("idxupmis"_n));
}

// kv_set creating a row with payer == 0 (the CDT same_payer sentinel) is rejected with
// invalid_table_payer: a brand-new row has no existing payer to keep. These drive a raw kv_set(tid, 0, ...)
// on a fresh key -- an input a CDT wrapper never emits, but the host must trap rather than silently bill
// the receiver.
BOOST_FIXTURE_TEST_CASE(kv_set_upsert_payer_zero_rejected_on_new_row, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("payzero"_n), invalid_table_payer);
}

BOOST_FIXTURE_TEST_CASE(kv_set_insert_payer_zero_rejected_on_new_row, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("payzeroins"_n), invalid_table_payer);
}

BOOST_FIXTURE_TEST_CASE(kv_set_unknown_mode_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("badmode"_n), kv_invalid_mode);
}

// Payer 0 on a row that exists keeps the row's payer in every mode, so RAM stays on that account.
BOOST_FIXTURE_TEST_CASE(kv_set_payer_zero_keeps_payer_in_every_mode, kv_probe_tester) {
   static constexpr uint32_t mode_upsert = 0, mode_insert = 1, mode_update = 2;
   const name payer = notified_a;
   const vector<permission_level> payer_auth{{payer, config::sysio_payer_name}, {payer, config::active_name}};
   const auto set_mode = [&](uint32_t mode, bool exists) {
      t.push_action(probe_account, "paymodes"_n, payer_auth, mutable_variant_object()("mode", mode)("exists", exists));
      t.produce_block();
   };

   const auto payer_ram_before    = ram_usage(payer);
   const auto receiver_ram_before = ram_usage();
   t.push_action(probe_account, "paysetup"_n, payer_auth, mutable_variant_object()("payer", payer));
   t.produce_block();
   const int64_t row_ram = 1 + 1 + config::kv_row_overhead_ram_bytes; // key, value
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_ram_before + row_ram);

   // paymodes writes a 3-byte value over the 1-byte one.
   set_mode(mode_insert, true);
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_ram_before + row_ram);
   set_mode(mode_update, true);
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_ram_before + row_ram + 2);
   set_mode(mode_upsert, true);
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_ram_before + row_ram + 2);
   BOOST_CHECK_EQUAL(ram_usage(), receiver_ram_before);

   // Erase refunds the row's payer, and update on the free key then reports no_row.
   run_action("payclean"_n);
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_ram_before);
   BOOST_CHECK_EQUAL(ram_usage(), receiver_ram_before);
   set_mode(mode_update, false);
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_ram_before);
   BOOST_CHECK_THROW(set_mode(mode_insert, false), invalid_table_payer);
   BOOST_CHECK_THROW(set_mode(mode_upsert, false), invalid_table_payer);
}

// =============================================================================
// E. Additional accepted-behavior probes.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(idx_find_miss_no_slot_leak, kv_probe_tester) {
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

BOOST_FIXTURE_TEST_CASE(kv_it_read_on_secondary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crshrd"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_prev_on_primary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crsiprv"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_lower_bound_on_primary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crsilb"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_read_on_primary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crsird"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(kv_idx_destroy_on_primary_handle_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("crsidst"_n), kv_invalid_iterator);
}

// -1 means "no handle", so it is not a value any handle call accepts.
BOOST_FIXTURE_TEST_CASE(no_handle_value_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("negdst"_n), kv_invalid_iterator);
   BOOST_CHECK_THROW(run_action("negidst"_n), kv_invalid_iterator);
   BOOST_CHECK_THROW(run_action("negrd"_n), kv_invalid_iterator);
   BOOST_CHECK_THROW(run_action("negird"_n), kv_invalid_iterator);
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

BOOST_FIXTURE_TEST_CASE(cross_table_pri_key_permitted, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("prixtab"_n));
}

// Inline action gets its own apply_context with an empty iterator pool. The parent leaves handle 0 open: the child
// cannot use it, and its own first allocation is slot 0.
BOOST_FIXTURE_TEST_CASE(inline_action_iterator_isolation, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("inlparnt"_n), kv_invalid_iterator);
   BOOST_CHECK_NO_THROW(run_action("inlpar2"_n));
}

// Notified contracts share the receiver's apply_context, but handles last for one receiver's execution.
BOOST_FIXTURE_TEST_CASE(notified_receiver_starts_from_slot_zero, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(t.push_action(probe_account, "leakiter"_n, probe_account,
                                      mutable_variant_object()("first", notified_a)("second", notified_b)));
   t.produce_block();
}

BOOST_FIXTURE_TEST_CASE(notified_receiver_cannot_use_primary_handle, kv_probe_tester) {
   BOOST_CHECK_THROW(t.push_action(probe_account, "leakuse"_n, probe_account,
                                   mutable_variant_object()("notified", notified_a)),
                     kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(notified_receiver_cannot_use_secondary_handle, kv_probe_tester) {
   BOOST_CHECK_THROW(t.push_action(probe_account, "leakusesec"_n, probe_account,
                                   mutable_variant_object()("notified", notified_a)),
                     kv_invalid_iterator);
}

// =============================================================================
// Index entries with an empty key, which a snapshot cannot hold.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(zero_length_sec_key, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("zseckey"_n), kv_empty_key);
}

BOOST_FIXTURE_TEST_CASE(zero_length_pri_key, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("zprikey"_n), kv_empty_key);
}

BOOST_FIXTURE_TEST_CASE(zero_length_new_sec_key, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("zsecupd"_n), kv_empty_key);
}

BOOST_FIXTURE_TEST_CASE(zero_length_pri_key_update, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("zpriupd"_n), kv_empty_key);
}

// =============================================================================
// Duplicate (sec_key, pri_key) pairs.
// =============================================================================

// The action stores the pair, tries twice more, and keeps running. Only the first store is billed.
BOOST_FIXTURE_TEST_CASE(duplicate_index_store_returns_exists, kv_probe_tester) {
   const auto ram_before = ram_usage();
   BOOST_CHECK_NO_THROW(run_action("dupstore"_n));
   BOOST_CHECK_EQUAL(ram_usage(), ram_before + 1 + 1 + config::kv_index_entry_overhead_ram_bytes);
   BOOST_CHECK_NO_THROW(run_action("dupclean"_n));
   BOOST_CHECK_EQUAL(ram_usage(), ram_before);
}

// Both entries were written in this transaction, so chainbase would remove the one being modified if the collision
// reached db.modify. The action erases both afterwards, so a lost entry fails it and shows in the RAM check.
BOOST_FIXTURE_TEST_CASE(colliding_index_update_returns_exists, kv_probe_tester) {
   const auto ram_before = ram_usage();
   BOOST_CHECK_NO_THROW(run_action("dupupdate"_n));
   BOOST_CHECK_EQUAL(ram_usage(), ram_before);
}

BOOST_FIXTURE_TEST_CASE(colliding_update_keeps_existing_entries, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("dupseed"_n));
   const auto ram_before = ram_usage();
   BOOST_CHECK_NO_THROW(run_action("dupupdkept"_n));
   BOOST_CHECK_EQUAL(ram_usage(), ram_before);
   BOOST_CHECK_NO_THROW(run_action("dupcheck"_n));
}

BOOST_FIXTURE_TEST_CASE(index_update_to_same_key_succeeds, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("updsamekey"_n));
}

// =============================================================================
// The approved interface: statuses, read-on-move, prefixes and position by key.
// =============================================================================

// Each write status, with no state left behind and no RAM billed.
BOOST_FIXTURE_TEST_CASE(every_status_from_every_write, kv_probe_tester) {
   const auto ram_before = ram_usage();
   BOOST_CHECK_NO_THROW(run_action("wrstatus"_n));
   BOOST_CHECK_EQUAL(ram_usage(), ram_before);
}

BOOST_FIXTURE_TEST_CASE(read_offsets_with_short_buffers, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("getoffs"_n));
}

BOOST_FIXTURE_TEST_CASE(primary_moves_read_the_row, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("itmoveread"_n));
}

BOOST_FIXTURE_TEST_CASE(secondary_moves_read_the_entry, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("idxmoveread"_n));
}

BOOST_FIXTURE_TEST_CASE(secondary_prefix_bounds_the_iterator, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("idxprefix"_n));
}

BOOST_FIXTURE_TEST_CASE(primary_position_is_by_key, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("itrekey"_n));
}

BOOST_FIXTURE_TEST_CASE(secondary_position_is_by_key, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("idxrekey"_n));
}

// =============================================================================
// RAM prices are protocol constants.
// =============================================================================

// Key sizes on both sides of every change in how a key is stored and compared. The price must not see any of them.
static constexpr std::array<uint32_t, 11> sized_key_lengths{1, 7, 8, 9, 16, 17, 24, 30, 31, 32, 256};

BOOST_FIXTURE_TEST_CASE(row_ram_price_does_not_depend_on_key_size, kv_probe_tester) {
   constexpr uint32_t value_size = 5;
   for (const uint32_t key_size : sized_key_lengths) {
      BOOST_TEST_CONTEXT("key size " << key_size) {
         const auto ram_before = ram_usage();
         t.push_action(probe_account, "sizedrow"_n, probe_account,
                       fc::mutable_variant_object()("key_size", key_size)("value_size", value_size));
         BOOST_CHECK_EQUAL(ram_usage(), ram_before + key_size + value_size + 112);
         t.push_action(probe_account, "sizedrowrm"_n, probe_account,
                       fc::mutable_variant_object()("key_size", key_size));
         BOOST_CHECK_EQUAL(ram_usage(), ram_before);
         t.produce_block();
      }
   }
}

// The two size outputs of a read must not overlap. A misaligned size writes back when the host call ends, in an order
// each compiler picks, so a contract reading the overlap could split clang and gcc nodes.
BOOST_FIXTURE_TEST_CASE(read_rejects_overlapping_size_pointers, kv_probe_tester) {
   constexpr uint32_t read_count = 8;
   constexpr std::array<std::pair<uint32_t, uint32_t>, 6> overlaps{
      {{1, 1}, {1, 3}, {3, 1}, {1, 4}, {8, 8}, {8, 10}}};
   constexpr std::array<std::pair<uint32_t, uint32_t>, 4> disjoint{{{1, 5}, {5, 1}, {3, 8}, {8, 12}}};

   const auto call = [&](uint32_t read, uint32_t first, uint32_t second) {
      t.push_action(probe_account, "sizeptrs"_n, probe_account,
                    fc::mutable_variant_object()("read", read)("first", first)("second", second));
   };
   for (uint32_t read = 0; read < read_count; ++read) {
      for (const auto& [first, second] : overlaps) {
         BOOST_TEST_CONTEXT("read " << read << ", sizes at " << first << " and " << second) {
            BOOST_CHECK_EXCEPTION(call(read, first, second), wasm_exception, [](const wasm_exception& e) {
               return e.to_detail_string().find("pointers not allowed to alias") != std::string::npos;
            });
         }
      }
      for (const auto& [first, second] : disjoint) {
         BOOST_TEST_CONTEXT("read " << read << ", sizes at " << first << " and " << second) {
            call(read, first, second);
            t.produce_block();
            // Each call stores the same row and entry, so clear them for the next one.
            t.push_action(probe_account, "clearsizes"_n, probe_account, fc::mutable_variant_object());
         }
      }
   }
}

// A payer with no account fails with a clear exception on every write that would bill it.
BOOST_FIXTURE_TEST_CASE(write_rejects_payer_with_no_account, kv_probe_tester) {
   constexpr uint32_t write_count = 4;
   const account_name no_account = "nosuchacct"_n;
   for (uint32_t write = 0; write < write_count; ++write) {
      BOOST_TEST_CONTEXT("write " << write) {
         BOOST_CHECK_EXCEPTION(t.push_action(probe_account, "payerwrite"_n, probe_account,
                                             fc::mutable_variant_object()("write", write)("payer", no_account)),
                               invalid_table_payer, [](const invalid_table_payer& e) {
                                  return e.to_detail_string().find("is not an account") != std::string::npos;
                               });
      }
   }
}

BOOST_FIXTURE_TEST_CASE(update_keeping_the_key_leaves_iterators_on_the_entry, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("idxsamekey"_n));
}

BOOST_FIXTURE_TEST_CASE(index_entry_ram_price_does_not_depend_on_key_size, kv_probe_tester) {
   for (const uint32_t sec_size : sized_key_lengths) {
      for (const uint32_t pri_size : {8u, 31u}) {
         BOOST_TEST_CONTEXT("secondary key size " << sec_size << ", primary key size " << pri_size) {
            const auto ram_before = ram_usage();
            t.push_action(probe_account, "sizedidx"_n, probe_account,
                          fc::mutable_variant_object()("sec_size", sec_size)("pri_size", pri_size));
            BOOST_CHECK_EQUAL(ram_usage(), ram_before + sec_size + pri_size + 112);

            // A move across the inline limit, in whichever direction this size allows, bills only the size change.
            constexpr uint32_t short_sec_size = 3;
            constexpr uint32_t long_sec_size  = 40;
            const uint32_t new_sec_size = sec_size > long_sec_size / 2 ? short_sec_size : long_sec_size;
            t.push_action(probe_account, "sizedidxup"_n, probe_account,
                          fc::mutable_variant_object()("sec_size", sec_size)("pri_size", pri_size)("new_sec_size",
                                                                                   new_sec_size));
            BOOST_CHECK_EQUAL(ram_usage(), ram_before + new_sec_size + pri_size + 112);

            t.push_action(probe_account, "sizedidxrm"_n, probe_account,
                          fc::mutable_variant_object()("sec_size", new_sec_size)("pri_size", pri_size));
            BOOST_CHECK_EQUAL(ram_usage(), ram_before);
            t.produce_block();
         }
      }
   }
}

// =============================================================================
// kv_it_prev and kv_idx_prev edges.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(it_prev_from_end_with_nothing_in_prefix, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("prvempty"_n));
}

BOOST_FIXTURE_TEST_CASE(it_prev_with_no_byte_successor, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("prvnosucc"_n));
}

BOOST_FIXTURE_TEST_CASE(it_prev_after_erase, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("prverased"_n));
}

// The seed is checked here: if something else ever sorts below it, this test stops reaching the branch it is for.
BOOST_FIXTURE_TEST_CASE(prev_at_the_first_row_of_the_whole_index, kv_probe_tester) {
   t.push_action(first_account, "firstseed"_n, first_account, mutable_variant_object());
   t.produce_block();
   kv_row         row;
   kv_index_entry entry;
   BOOST_REQUIRE(t.control->kv().open_row_cursor()->seek_lower({}, row));
   BOOST_REQUIRE(t.control->kv().open_index_cursor()->seek_lower({}, entry));
   BOOST_REQUIRE_EQUAL(row.code, first_account);
   BOOST_REQUIRE_EQUAL(entry.code, first_account);

   BOOST_CHECK_NO_THROW(t.push_action(first_account, "firstrun"_n, first_account, mutable_variant_object()));
   t.produce_block();
}

// With no row in the index at all, begin and end are one place. The bios and ROA contracts write rows, so this needs a
// chain with neither.
BOOST_AUTO_TEST_CASE(prev_on_an_empty_index) {
   validating_tester chain(flat_set<account_name>(), nullptr, setup_policy::none);
   chain.produce_block();
   chain.create_accounts({probe_account});
   chain.produce_block();
   chain.set_code(probe_account, test_contracts::kv_intrinsic_probe_wasm());
   chain.set_abi(probe_account, test_contracts::kv_intrinsic_probe_abi().c_str());
   chain.produce_block();
   BOOST_REQUIRE_EQUAL(chain.control->kv().row_count(), 0u);
   BOOST_REQUIRE_EQUAL(chain.control->kv().index_entry_count(), 0u);

   BOOST_CHECK_NO_THROW(chain.push_action(probe_account, "emptyindex"_n, probe_account, mutable_variant_object()));
   chain.produce_block();
}

// The other end of the index. The seed is checked for the same reason as at the first row.
BOOST_FIXTURE_TEST_CASE(moves_at_the_last_row_of_the_whole_index, kv_probe_tester) {
   t.push_action(last_account, "lastseed"_n, last_account, mutable_variant_object());
   t.produce_block();
   // A new cursor starts at end, so one step back is the last row.
   kv_row         row;
   kv_index_entry entry;
   BOOST_REQUIRE(t.control->kv().open_row_cursor()->prev(row));
   BOOST_REQUIRE(t.control->kv().open_index_cursor()->prev(entry));
   BOOST_REQUIRE_EQUAL(row.code, last_account);
   BOOST_REQUIRE_EQUAL(entry.code, last_account);
   BOOST_REQUIRE_EQUAL(row.table_id, std::numeric_limits<uint16_t>::max());
   BOOST_REQUIRE_EQUAL(entry.table_id, std::numeric_limits<uint16_t>::max());

   BOOST_CHECK_NO_THROW(t.push_action(last_account, "lastrun"_n, last_account, mutable_variant_object()));
   t.produce_block();
}

// A lookup that finds nothing in its table lands on whatever the index holds next. Each entry here differs from the one
// looked for in its table or its code and nothing else, so only those checks can refuse it. Needs a chain with no other
// rows, or something would sort between them.
BOOST_AUTO_TEST_CASE(lookups_stop_at_the_edge_of_their_table_and_code) {
   validating_tester chain(flat_set<account_name>(), nullptr, setup_policy::none);
   chain.produce_block();
   chain.create_accounts({probe_account, notified_a});
   chain.produce_block();
   for (name account : {probe_account, notified_a}) {
      chain.set_code(account, test_contracts::kv_intrinsic_probe_wasm());
      chain.set_abi(account, test_contracts::kv_intrinsic_probe_abi().c_str());
   }
   chain.produce_block();
   const auto one_key = [&](name account, uint32_t table_id) {
      chain.push_action(account, "onekey"_n, account, mutable_variant_object()("table_id", table_id));
   };
   const auto refused = [&](uint32_t table_id) {
      BOOST_CHECK_NO_THROW(chain.push_action(probe_account, "onekeymiss"_n, probe_account,
                                             mutable_variant_object()("table_id", table_id)("code", probe_account)));
      chain.produce_block();
   };

   one_key(probe_account, 8);
   refused(7);
   one_key(notified_a, 9);
   refused(9);
}

BOOST_FIXTURE_TEST_CASE(idx_prev_edges, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("ixprvedge"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_equal_secondary_keys_order_by_primary_key, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("ixdupkeys"_n));
}

// =============================================================================
// Seeks, reads, and other contracts' tables.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(it_lower_bound_stays_in_the_prefix, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("itlbclamp"_n));
}

BOOST_FIXTURE_TEST_CASE(it_read_rules, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("rdrules"_n));
}

BOOST_FIXTURE_TEST_CASE(idx_read_rules, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("ixrdrules"_n));
}

// A size argument needs no alignment, but it must not be null: the read aborts rather than skipping the write.
BOOST_FIXTURE_TEST_CASE(size_arguments_need_no_alignment, kv_probe_tester) {
   for (uint32_t addr : {1u, 2u, 3u, 4u}) {
      BOOST_TEST_CONTEXT("addr " << addr) {
         BOOST_CHECK_NO_THROW(t.push_action(probe_account, "rdsizeptr"_n, probe_account,
                                            mutable_variant_object()("addr", addr)));
         t.produce_block();
      }
   }
   BOOST_CHECK_EXCEPTION(t.push_action(probe_account, "rdsizeptr"_n, probe_account,
                                       mutable_variant_object()("addr", 0)),
                         wasm_execution_error,
                         fc_exception_message_is("references cannot be created for null pointers"));
}

// Null aborts on each of the 16 size arguments. A short read or a missed write would each go unnoticed.
BOOST_FIXTURE_TEST_CASE(no_size_argument_may_be_null, kv_probe_tester) {
   static constexpr uint32_t calls_with_sizes = 8; // the cases of rdnullsz
   for (uint32_t call = 0; call < calls_with_sizes; ++call) {
      for (uint32_t which : {0u, 1u}) {
         BOOST_TEST_CONTEXT("call " << call << " size " << which) {
            BOOST_CHECK_EXCEPTION(t.push_action(probe_account, "rdnullsz"_n, probe_account,
                                                mutable_variant_object()("call", call)("which", which)),
                                  wasm_execution_error,
                                  fc_exception_message_is("references cannot be created for null pointers"));
         }
      }
   }
}

// A size over a value buffer, at a uint32_t boundary, where the host writes through the pointer in call order. Two sizes
// over each other are refused, which read_rejects_overlapping_size_pointers pins.
BOOST_FIXTURE_TEST_CASE(aligned_overlapping_read_outputs_copy_in_call_order, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("rdoverlap"_n));
}

BOOST_FIXTURE_TEST_CASE(input_spans_may_overlap_output_buffers, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("rdinoverlap"_n));
}

BOOST_FIXTURE_TEST_CASE(moves_copy_the_second_field_over_the_first, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("rdoutoverlap"_n));
}

BOOST_FIXTURE_TEST_CASE(index_key_spans_may_overlap, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("ixalias"_n));
}

BOOST_FIXTURE_TEST_CASE(reads_follow_the_code_argument, kv_probe_tester) {
   t.push_action(notified_a, "xcseed"_n, notified_a, mutable_variant_object());
   t.produce_block();
   BOOST_CHECK_NO_THROW(t.push_action(probe_account, "xciter"_n, probe_account,
                                      mutable_variant_object()("other", notified_a)));
   t.produce_block();
}

// =============================================================================
// Handle numbers.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(lowest_free_slot_comes_back_first, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("slotreuse"_n));
}

BOOST_FIXTURE_TEST_CASE(kv_idx_destroy_twice_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("ixdbldest"_n), kv_invalid_iterator);
}

BOOST_FIXTURE_TEST_CASE(idx_use_after_destroy_rejected, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("ixuaf"_n), kv_invalid_iterator);
}

// The whole pool, through the calls that open handles. Slot 1023 is the top one, and the 1025th handle goes past it.
BOOST_FIXTURE_TEST_CASE(a_pool_at_its_limit_reuses_and_refuses, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("poolfull"_n));
   static constexpr uint32_t opening_calls = 3; // kv_it_create, kv_idx_create, kv_idx_find
   for (uint32_t call = 0; call < opening_calls; ++call) {
      BOOST_TEST_CONTEXT("call " << call) {
         BOOST_CHECK_THROW(t.push_action(probe_account, "poollimit"_n, probe_account,
                                         mutable_variant_object()("call", call)),
                           kv_iterator_limit_exceeded);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(two_iterators_on_one_row, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("twoslots"_n));
}

BOOST_FIXTURE_TEST_CASE(row_stored_while_an_iterator_sits_at_end, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("storeatend"_n));
}

BOOST_FIXTURE_TEST_CASE(index_update_edges, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("updedges"_n));
}

// Most probes use 2-byte keys, which never reach the integer compare for 8-byte keys. Getting that compare wrong keeps
// each index consistent with itself, so only a walk across the high bit and across several bytes shows it.
BOOST_FIXTURE_TEST_CASE(eight_byte_keys_walk_in_byte_order, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("keyorder"_n));
}

BOOST_FIXTURE_TEST_CASE(lookups_take_a_key_of_any_size, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("nolimit"_n));
}

// =============================================================================
// Aborts. Whatever stops a transaction, every write in it goes, on every account.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(abort_in_an_inline_action_undoes_the_action_before_it, kv_probe_tester) {
   const auto before = abort_seeded_state();
   BOOST_CHECK_EXCEPTION(t.push_action(probe_account, "abrtinline"_n, probe_account, mutable_variant_object()),
                         sysio_assert_message_exception, sysio_assert_message_is("abort in the inline action"));
   t.produce_block();
   kv_write_driver::check_same_kv_state(abort_state(), before);
}

BOOST_FIXTURE_TEST_CASE(abort_in_a_notified_contract_undoes_both_contracts, kv_probe_tester) {
   const auto before = abort_seeded_state();
   BOOST_CHECK_EXCEPTION(t.push_action(probe_account, "abrtnotify"_n, probe_account,
                                       mutable_variant_object()("notified", notified_a)),
                         sysio_assert_message_exception, sysio_assert_message_is("abort in the notified contract"));
   t.produce_block();
   kv_write_driver::check_same_kv_state(abort_state(), before);
}

// The action body has returned by the time validate_account_ram_deltas refuses the bill.
BOOST_FIXTURE_TEST_CASE(refused_ram_bill_undoes_finished_writes, kv_probe_tester) {
   const auto before = abort_seeded_state();
   BOOST_CHECK_EXCEPTION(t.push_action(probe_account, "abrtram"_n, probe_account,
                                       mutable_variant_object()("other", notified_b)),
                         unauthorized_ram_usage_increase,
                         fc_exception_message_contains("that has not authorized the action"));
   t.produce_block();
   kv_write_driver::check_same_kv_state(abort_state(), before);
}

BOOST_FIXTURE_TEST_CASE(refused_ram_bill_in_a_notified_contract_undoes_both_contracts, kv_probe_tester) {
   const auto before = abort_seeded_state();
   BOOST_CHECK_EXCEPTION(t.push_action(probe_account, "abrtramnote"_n, probe_account,
                                       mutable_variant_object()("notified", notified_a)("other", notified_b)),
                         unauthorized_ram_usage_increase, fc_exception_message_contains("within a notify context"));
   t.produce_block();
   kv_write_driver::check_same_kv_state(abort_state(), before);
}

// =============================================================================
// The ends of contract memory, and context-free actions. Both need WAST.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(context_free_action_cannot_call_any_kv_intrinsic, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(context_free_call(kv_intrinsic_count));
   for (uint32_t call = 0; call < kv_intrinsic_count; ++call) {
      BOOST_TEST_CONTEXT("call " << call) {
         BOOST_CHECK_EXCEPTION(context_free_call(call), unaccessible_api,
                               fc_exception_message_is("only context free api's can be used in this context"));
      }
   }
}

// A span may end exactly at the end of memory. The first byte past it traps, however the span gets there.
BOOST_FIXTURE_TEST_CASE(spans_stop_at_the_end_of_memory, kv_probe_tester) {
   edge(kv_edge_call::value_span, 64, 1); // the row kv_edge_call's reads find
   for (kv_edge_call call : {kv_edge_call::key_span, kv_edge_call::value_span, kv_edge_call::get_output,
                             kv_edge_call::read_key_buffer, kv_edge_call::read_value_buffer, kv_edge_call::pri_span}) {
      BOOST_TEST_CONTEXT("call " << magic_enum::enum_name(call)) {
         BOOST_CHECK_NO_THROW(edge(call, page_size - 1, 1));
         BOOST_CHECK_THROW(edge(call, page_size, 1), wasm_execution_error);
         BOOST_CHECK_THROW(edge(call, page_size - 1, 2), wasm_execution_error);
      }
   }
}

// An empty span at the very end touches nothing, and one a byte further on is still out of bounds. A key cannot be empty,
// so this runs on the spans that can.
BOOST_FIXTURE_TEST_CASE(empty_spans_at_the_end_of_memory, kv_probe_tester) {
   edge(kv_edge_call::value_span, 64, 1);
   for (kv_edge_call call : {kv_edge_call::value_span, kv_edge_call::get_output, kv_edge_call::read_key_buffer,
                             kv_edge_call::read_value_buffer}) {
      BOOST_TEST_CONTEXT("call " << magic_enum::enum_name(call)) {
         BOOST_CHECK_NO_THROW(edge(call, page_size, 0));
         BOOST_CHECK_THROW(edge(call, page_size + 1, 0), wasm_execution_error);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(size_pointers_stop_at_the_end_of_memory, kv_probe_tester) {
   edge(kv_edge_call::value_span, 64, 1);
   for (kv_edge_call call : {kv_edge_call::read_first_size, kv_edge_call::read_second_size}) {
      BOOST_TEST_CONTEXT("call " << magic_enum::enum_name(call)) {
         BOOST_CHECK_NO_THROW(edge(call, last_size_ptr, 0));
         BOOST_CHECK_THROW(edge(call, last_size_ptr + 1, 0), wasm_execution_error);
      }
   }
}

// =============================================================================
// table_id, prefix and empty-key limits.
// =============================================================================

// One step past the top, which a truncating cast would turn into table 0, and the largest value there is.
BOOST_FIXTURE_TEST_CASE(table_id_above_uint16_rejected_on_every_call, kv_probe_tester) {
   static constexpr uint32_t table_id_calls = 8; // the entries of table_id_call in the probe contract
   static constexpr uint32_t past_top = uint32_t{std::numeric_limits<uint16_t>::max()} + 1;
   for (uint32_t table_id : {past_top, std::numeric_limits<uint32_t>::max()}) {
      for (uint32_t call = 0; call < table_id_calls; ++call) {
         BOOST_TEST_CONTEXT("call " << call << " table_id " << table_id) {
            BOOST_CHECK_THROW(t.push_action(probe_account, "tidbad"_n, probe_account,
                                            mutable_variant_object()("call", call)("table_id", table_id)),
                              kv_invalid_table_id);
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(top_table_id_accepted_on_every_call, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("tidtop"_n));
}

BOOST_FIXTURE_TEST_CASE(prefix_at_the_cap_accepted, kv_probe_tester) {
   BOOST_CHECK_NO_THROW(run_action("pfxmax"_n));
}

BOOST_FIXTURE_TEST_CASE(prefix_over_the_cap_rejected_on_every_call, kv_probe_tester) {
   BOOST_CHECK_THROW(run_action("bigpfx"_n), kv_key_too_large);
   BOOST_CHECK_THROW(run_action("bigpfxidx"_n), kv_key_too_large);
   BOOST_CHECK_THROW(run_action("bigpfxfind"_n), kv_key_too_large);
}

// =============================================================================
// Payer 0 on a size change: the bill follows the row, whoever pays for it.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(same_payer_updates_that_grow_and_shrink, kv_probe_tester) {
   const name  payer = notified_b;
   const vector<permission_level> payer_auth{{payer, config::sysio_payer_name}, {payer, config::active_name}};
   const auto  payer_before    = ram_usage(payer);
   const auto  receiver_before = ram_usage();
   const auto  run = [&](name action, const vector<permission_level>& auth, const fc::variant_object& args) {
      t.push_action(probe_account, action, auth, args);
      t.produce_block();
   };
   const vector<permission_level> receiver_auth{{probe_account, config::active_name}};

   run("paysizeadd"_n, payer_auth, mutable_variant_object()("payer", payer));
   // Row: 1-byte key, 1-byte value. Entry: 1-byte primary key, 1-byte secondary key. Literal overheads on purpose.
   const int64_t billed = (1 + 1 + 112) + (1 + 1 + 112);
   BOOST_REQUIRE_EQUAL(ram_usage(payer), payer_before + billed);

   // A row that grows bills its payer, who must authorize it.
   BOOST_CHECK_THROW(run("paysize"_n, receiver_auth, mutable_variant_object()("size", 40)),
                     unauthorized_ram_usage_increase);
   run("paysize"_n, payer_auth, mutable_variant_object()("size", 40));
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_before + billed + 39);
   // A row that shrinks refunds its payer, and a refund needs no authorization.
   run("paysize"_n, receiver_auth, mutable_variant_object()("size", 10));
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_before + billed + 9);

   // The same for an index entry, whose secondary key changes size.
   BOOST_CHECK_THROW(run("paysizesec"_n, receiver_auth, mutable_variant_object()("old_size", 1)("new_size", 30)),
                     unauthorized_ram_usage_increase);
   run("paysizesec"_n, payer_auth, mutable_variant_object()("old_size", 1)("new_size", 30));
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_before + billed + 9 + 29);
   run("paysizesec"_n, receiver_auth, mutable_variant_object()("old_size", 30)("new_size", 20));
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_before + billed + 9 + 19);

   // Nothing ever moved onto the receiver, and erasing refunds the payer in full.
   BOOST_CHECK_EQUAL(ram_usage(), receiver_before);
   run("paysizedel"_n, receiver_auth, mutable_variant_object()("sec_size", 20));
   BOOST_CHECK_EQUAL(ram_usage(payer), payer_before);
   BOOST_CHECK_EQUAL(ram_usage(), receiver_before);
}

BOOST_AUTO_TEST_SUITE_END()

// A chain of its own: the limits are global, and these tests move them.
struct kv_limits_tester : validating_tester {
   kv_limits_tester() {
      create_accounts({probe_account});
      produce_block();
      set_code(probe_account, test_contracts::kv_intrinsic_probe_wasm());
      set_abi(probe_account, test_contracts::kv_intrinsic_probe_abi().c_str());
      produce_block();
   }

   void set_limits(uint32_t key, uint32_t value, uint32_t secondary) {
      auto params = control->get_global_properties().configuration;
      params.max_kv_key_size           = key;
      params.max_kv_value_size         = value;
      params.max_kv_secondary_key_size = secondary;
      push_action(config::system_account_name, "setparams"_n, config::system_account_name,
                  mutable_variant_object()("params", params));
      produce_block();
   }

   void run(name action, const fc::variant_object& args) {
      push_action(probe_account, action, probe_account, args);
      produce_block();
   }
   void row(uint32_t key_size, uint32_t value_size) {
      run("limrow"_n, mutable_variant_object()("key_size", key_size)("value_size", value_size));
   }
   void row_check(uint32_t key_size, uint32_t value_size) {
      run("limrowchk"_n, mutable_variant_object()("key_size", key_size)("value_size", value_size));
   }
   void row_erase(uint32_t key_size) {
      run("limrowdel"_n, mutable_variant_object()("key_size", key_size));
   }
   void entry(uint32_t pri_size, uint32_t sec_size) {
      run("limsec"_n, mutable_variant_object()("pri_size", pri_size)("sec_size", sec_size));
   }
   void entry_update(uint32_t pri_size, uint32_t old_size, uint32_t new_size) {
      run("limsecupd"_n, mutable_variant_object()("pri_size", pri_size)("old_size", old_size)("new_size", new_size));
   }
   void entry_check(uint32_t sec_size) {
      run("limsecchk"_n, mutable_variant_object()("sec_size", sec_size));
   }
   void entry_erase(uint32_t pri_size, uint32_t sec_size) {
      run("limsecdel"_n, mutable_variant_object()("pri_size", pri_size)("sec_size", sec_size));
   }
   int64_t ram_usage() const {
      return control->get_resource_limits_manager().get_account_ram_usage(probe_account);
   }

   static constexpr uint32_t key_default   = config::default_max_kv_key_size;
   static constexpr uint32_t value_default = config::default_max_kv_value_size;
   static constexpr uint32_t sec_default   = config::default_max_kv_secondary_key_size;
   static constexpr uint32_t key_cap       = config::max_kv_key_size_limit;
   static constexpr uint32_t value_cap     = config::max_kv_value_size_limit;
};

BOOST_AUTO_TEST_SUITE(kv_limits_tests)

// Each limit is enforced at its current value on every call that takes it, and goes up to its cap.
BOOST_FIXTURE_TEST_CASE(raised_limits_apply_to_every_call, kv_limits_tester) {
   const auto ram_before = ram_usage();

   BOOST_CHECK_THROW(row(key_default + 1, 1), kv_key_too_large);
   BOOST_CHECK_THROW(row(1, value_default + 1), kv_value_too_large);
   BOOST_CHECK_THROW(entry(1, sec_default + 1), kv_secondary_key_too_large);
   BOOST_CHECK_THROW(entry(key_default + 1, 1), kv_key_too_large);
   entry(1, sec_default);
   BOOST_CHECK_THROW(entry_update(1, sec_default, sec_default + 1), kv_secondary_key_too_large);

   set_limits(key_cap, value_cap, key_cap);

   row(key_cap, value_cap);
   row_check(key_cap, value_cap);
   BOOST_CHECK_THROW(row(key_cap + 1, 1), kv_key_too_large);
   BOOST_CHECK_THROW(row(2, value_cap + 1), kv_value_too_large);

   entry_update(1, sec_default, key_cap);
   entry_check(key_cap);
   entry(key_cap, key_cap);
   BOOST_CHECK_THROW(entry(1, key_cap + 1), kv_secondary_key_too_large);
   BOOST_CHECK_THROW(entry(key_cap + 1, 1), kv_key_too_large);
   BOOST_CHECK_THROW(entry_update(1, key_cap, key_cap + 1), kv_secondary_key_too_large);

   row_erase(key_cap);
   entry_erase(1, key_cap);
   entry_erase(key_cap, key_cap);
   BOOST_CHECK_EQUAL(ram_usage(), ram_before);
}

// Lowering a limit strands nothing: a row over the new limit reads and erases as before, and only a write that names
// an over-limit size is refused.
BOOST_FIXTURE_TEST_CASE(lowered_limits_keep_existing_rows_readable, kv_limits_tester) {
   static constexpr uint32_t value_over = value_default + 1;
   const auto ram_before = ram_usage();

   set_limits(key_cap, value_cap, key_cap);
   row(key_cap, 1);
   row(1, value_over);
   entry(key_cap, key_cap);
   entry(1, key_cap);
   set_limits(key_default, value_default, sec_default);

   // The rows and entries are still there, and a prefix stays bound by the fixed cap rather than by these limits.
   row_check(key_cap, 1);
   row_check(1, value_over);
   entry_check(key_cap);

   // Rewriting them at the old sizes is refused.
   BOOST_CHECK_THROW(row(key_cap, 1), kv_key_too_large);
   BOOST_CHECK_THROW(row(1, value_over), kv_value_too_large);
   BOOST_CHECK_THROW(entry_update(1, key_cap, key_cap - 1), kv_secondary_key_too_large);

   // A new size inside the limits is fine, and erase needs no size check.
   row(1, 1);
   row_check(1, 1);
   entry_update(1, key_cap, 1);
   row_erase(1);
   row_erase(key_cap);
   entry_erase(1, 1);
   entry_erase(key_cap, key_cap);
   BOOST_CHECK_EQUAL(ram_usage(), ram_before);
}

// The key limit and the secondary key limit are separate settings, each holding its own keys. The primary key of an entry
// is a key, so the key limit holds it too.
BOOST_FIXTURE_TEST_CASE(each_limit_holds_its_own_keys, kv_limits_tester) {
   const auto apart = [&](uint32_t key_limit, uint32_t sec_limit) {
      BOOST_TEST_CONTEXT("key limit " << key_limit << " secondary limit " << sec_limit) {
         set_limits(key_limit, value_default, sec_limit);

         entry(1, sec_limit);
         BOOST_CHECK_THROW(entry(2, sec_limit + 1), kv_secondary_key_too_large);
         entry(key_limit, 1);
         BOOST_CHECK_THROW(entry(key_limit + 1, 1), kv_key_too_large);
         row(key_limit, 1);
         BOOST_CHECK_THROW(row(key_limit + 1, 1), kv_key_too_large);
         BOOST_CHECK_THROW(entry_update(1, sec_limit, sec_limit + 1), kv_secondary_key_too_large);
         entry_update(1, sec_limit, sec_limit - 1);

         entry_erase(1, sec_limit - 1);
         entry_erase(key_limit, 1);
         row_erase(key_limit);
      }
   };
   apart(300, 200);
   apart(200, 300);
}

// The caps themselves cannot be raised. params_tests covers the two key limits through set_parameters_packed.
BOOST_FIXTURE_TEST_CASE(limits_stop_at_their_caps, kv_limits_tester) {
   BOOST_CHECK_THROW(set_limits(key_cap + 1, value_default, sec_default), action_validate_exception);
   BOOST_CHECK_THROW(set_limits(key_default, value_cap + 1, sec_default), action_validate_exception);
   BOOST_CHECK_THROW(set_limits(key_default, value_default, key_cap + 1), action_validate_exception);
   BOOST_CHECK_THROW(set_limits(0, value_default, sec_default), action_validate_exception);
   BOOST_CHECK_THROW(set_limits(key_default, 0, sec_default), action_validate_exception);
   BOOST_CHECK_THROW(set_limits(key_default, value_default, 0), action_validate_exception);

   const auto& limits = control->get_global_properties().configuration;
   BOOST_CHECK_EQUAL(limits.max_kv_key_size, key_default);
   BOOST_CHECK_EQUAL(limits.max_kv_value_size, value_default);
   BOOST_CHECK_EQUAL(limits.max_kv_secondary_key_size, sec_default);
}

BOOST_AUTO_TEST_SUITE_END()
