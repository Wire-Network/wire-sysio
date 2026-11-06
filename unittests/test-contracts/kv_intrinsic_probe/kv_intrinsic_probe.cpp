// Adversarial probe of the raw kv_* host intrinsics.
// Uses only extern "C" decls to bypass CDT's kv_multi_index / kv::table
// wrappers, so we can pass inputs CDT would never emit: zero sizes,
// oversize spans, stale/forged iterator handles, cross-pool handles,
// table_ids above the uint16 namespace, etc.
//
// Each action is self-contained and uses a distinct table_id so shared-state
// accumulation across actions is safe under a shared tester.

#include <sysio/sysio.hpp>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <limits>
#include <vector>

// Raw kv_* host intrinsic declarations. Listed here instead of via CDT's kv.h so every adversarial test is explicit about
// the ABI it is exercising.
extern "C" {
__attribute__((sysio_wasm_import))
int32_t  kv_set(uint32_t table_id, uint64_t payer, const void* key, uint32_t key_size,
                const void* value, uint32_t value_size, uint32_t mode);
__attribute__((sysio_wasm_import))
int32_t  kv_erase(uint32_t table_id, const void* key, uint32_t key_size);
__attribute__((sysio_wasm_import))
int32_t  kv_get(uint32_t table_id, uint64_t code, const void* key, uint32_t key_size, uint32_t offset,
                void* value, uint32_t value_size);

__attribute__((sysio_wasm_import))
int32_t  kv_idx_store(uint32_t table_id, uint64_t payer,
                      const void* pri_key, uint32_t pri_key_size,
                      const void* sec_key, uint32_t sec_key_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_update(uint32_t table_id, uint64_t payer,
                       const void* pri_key, uint32_t pri_key_size,
                       const void* old_sec_key, uint32_t old_sec_key_size,
                       const void* new_sec_key, uint32_t new_sec_key_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_erase(uint32_t table_id,
                      const void* pri_key, uint32_t pri_key_size,
                      const void* sec_key, uint32_t sec_key_size);

__attribute__((sysio_wasm_import))
int32_t  kv_it_create(uint32_t table_id, uint64_t code, const void* prefix, uint32_t prefix_size,
                      const void* seek, uint32_t seek_size);
__attribute__((sysio_wasm_import))
int32_t  kv_it_next(int32_t handle, void* key, uint32_t key_capacity, void* value, uint32_t value_capacity,
                    uint32_t* key_size, uint32_t* value_size);
__attribute__((sysio_wasm_import))
int32_t  kv_it_prev(int32_t handle, void* key, uint32_t key_capacity, void* value, uint32_t value_capacity,
                    uint32_t* key_size, uint32_t* value_size);
__attribute__((sysio_wasm_import))
int32_t  kv_it_lower_bound(int32_t handle, const void* seek, uint32_t seek_size, void* key, uint32_t key_capacity,
                           void* value, uint32_t value_capacity, uint32_t* key_size, uint32_t* value_size);
__attribute__((sysio_wasm_import))
int32_t  kv_it_read(int32_t handle, uint32_t offset, void* key, uint32_t key_capacity, void* value,
                    uint32_t value_capacity, uint32_t* key_size, uint32_t* value_size);
__attribute__((sysio_wasm_import))
void     kv_it_destroy(int32_t handle);

__attribute__((sysio_wasm_import))
int32_t  kv_idx_create(uint32_t table_id, uint64_t code, const void* prefix, uint32_t prefix_size,
                       const void* seek, uint32_t seek_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_next(int32_t handle, void* sec_key, uint32_t sec_capacity, void* pri_key, uint32_t pri_capacity,
                     uint32_t* sec_size, uint32_t* pri_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_prev(int32_t handle, void* sec_key, uint32_t sec_capacity, void* pri_key, uint32_t pri_capacity,
                     uint32_t* sec_size, uint32_t* pri_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_lower_bound(int32_t handle, const void* seek, uint32_t seek_size, void* sec_key, uint32_t sec_capacity,
                            void* pri_key, uint32_t pri_capacity, uint32_t* sec_size, uint32_t* pri_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_read(int32_t handle, void* sec_key, uint32_t sec_capacity, void* pri_key, uint32_t pri_capacity,
                     uint32_t* sec_size, uint32_t* pri_size);
__attribute__((sysio_wasm_import))
void     kv_idx_destroy(int32_t handle);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_find(uint32_t table_id, uint64_t code, const void* prefix, uint32_t prefix_size,
                     const void* sec_key, uint32_t sec_key_size);
}

using namespace sysio;

// Distinct table_id per action keeps probes independent under the shared tester.
static constexpr uint32_t tid_zval       = 501;
static constexpr uint32_t tid_zseck_pri  = 503;
static constexpr uint32_t tid_zseck_sec  = 504;
static constexpr uint32_t tid_contcns    = 505;
static constexpr uint32_t tid_itinvl     = 506;
static constexpr uint32_t tid_endstat    = 507;
static constexpr uint32_t tid_offsa      = 508;
static constexpr uint32_t tid_zkey       = 509;
static constexpr uint32_t tid_big        = 510;
static constexpr uint32_t tid_bigsec_pri = 511;
static constexpr uint32_t tid_bigsec_sec = 512;
static constexpr uint32_t tid_tidmax     = 513;
static constexpr uint32_t tid_crossh_pri = 514;
static constexpr uint32_t tid_crossh_sec = 515;
static constexpr uint32_t tid_bigpri     = 516;
static constexpr uint32_t tid_uaf        = 518;
static constexpr uint32_t tid_dbldest    = 519;
static constexpr uint32_t tid_alias      = 520;
static constexpr uint32_t tid_payzero    = 521;
static constexpr uint32_t tid_erasmis    = 522;
static constexpr uint32_t tid_idxrm_pri  = 523;
static constexpr uint32_t tid_idxrm_sec  = 524;
static constexpr uint32_t tid_idxup_pri  = 525;
static constexpr uint32_t tid_idxup_sec  = 526;
static constexpr uint32_t tid_idxupbig   = 527;
static constexpr uint32_t tid_findmis    = 528;
static constexpr uint32_t tid_lbpast_pri = 529;
static constexpr uint32_t tid_lbpast_sec = 530;
static constexpr uint32_t tid_lbempty    = 531;
static constexpr uint32_t tid_prmera_pri = 532;
static constexpr uint32_t tid_prmera_sec = 533;
static constexpr uint32_t tid_itprvbg    = 534;
static constexpr uint32_t tid_cpool      = 535;
static constexpr uint32_t tid_cpool_sec  = 536;
static constexpr uint32_t tid_secilim_p  = 537;
static constexpr uint32_t tid_secilim_s  = 538;
static constexpr uint32_t tid_maxok      = 539;
static constexpr uint32_t tid_dangl_pri  = 540;
static constexpr uint32_t tid_dangl_sec  = 541;
static constexpr uint32_t tid_prixtab_a  = 542;
static constexpr uint32_t tid_prixtab_s  = 543;
static constexpr uint32_t tid_inliso     = 544;
static constexpr uint32_t tid_minkey_pri = 545;
static constexpr uint32_t tid_minkey_sec = 546;
static constexpr uint32_t tid_dup        = 547;
static constexpr uint32_t tid_dupkept    = 548;
static constexpr uint32_t tid_leak_pri   = 549;
static constexpr uint32_t tid_leak_sec   = 550;
static constexpr uint32_t tid_writes     = 551;
static constexpr uint32_t tid_writes_sec = 552;
static constexpr uint32_t tid_reads      = 553;
static constexpr uint32_t tid_moves      = 554;
static constexpr uint32_t tid_moves_sec  = 555;
static constexpr uint32_t tid_prefix_sec = 556;
static constexpr uint32_t tid_rekey      = 557;
static constexpr uint32_t tid_rekey_sec  = 558;
static constexpr uint32_t tid_paymodes   = 559;
// These three are adjacent so that one step past the middle table lands in a neighbor.
static constexpr uint32_t tid_prev_low   = 560;
static constexpr uint32_t tid_prev_empty = 561;
static constexpr uint32_t tid_prev_high  = 562;
static constexpr uint32_t tid_prev_rows  = 563;
static constexpr uint32_t tid_prev_ff    = 564;
static constexpr uint32_t tid_prev_ff_up = 565;
static constexpr uint32_t tid_prev_succ  = 566;
static constexpr uint32_t tid_prev_erase = 567;
static constexpr uint32_t tid_ixprev_low = 568;
static constexpr uint32_t tid_ixprev     = 569;
static constexpr uint32_t tid_dupkeys    = 570;
static constexpr uint32_t tid_lbound     = 571;
static constexpr uint32_t tid_rdrules    = 572;
static constexpr uint32_t tid_rdrules_ix = 573;
static constexpr uint32_t tid_xc         = 574;
static constexpr uint32_t tid_xc_sec     = 575;
static constexpr uint32_t tid_slots      = 576;
static constexpr uint32_t tid_slots_sec  = 577;
static constexpr uint32_t tid_pfx        = 580;
static constexpr uint32_t tid_paysize    = 581;
static constexpr uint32_t tid_paysize_ix = 582;
static constexpr uint32_t tid_limits     = 583;
static constexpr uint32_t tid_limits_sec = 584;
static constexpr uint32_t tid_abort      = 585;
static constexpr uint32_t tid_abort_sec  = 586;
static constexpr uint32_t tid_atend      = 587;
static constexpr uint32_t tid_atend_sec  = 588;
static constexpr uint32_t tid_updedges   = 589;
static constexpr uint32_t tid_nolimit    = 590;
static constexpr uint32_t tid_nolimit_ix = 591;
static constexpr uint32_t tid_pool       = 592;
static constexpr uint32_t tid_pool_sec   = 593;
static constexpr uint32_t tid_spans      = 594;
static constexpr uint32_t tid_spans_sec  = 595;
static constexpr uint32_t tid_twoslots   = 596;
static constexpr uint32_t tid_twoslot_ix = 597;
static constexpr uint32_t tid_sizes      = 598;
static constexpr uint32_t tid_sizes_sec  = 599;
static constexpr uint32_t tid_order      = 600;
static constexpr uint32_t tid_order_sec  = 601;
static constexpr uint32_t tid_order_pri  = 602;
static constexpr uint32_t tid_sized      = 610;
static constexpr uint32_t tid_sized_sec  = 611;
static constexpr uint32_t tid_sizeptrs     = 612;
static constexpr uint32_t tid_sizeptrs_sec = 613;
static constexpr uint32_t tid_payerw       = 614;
static constexpr uint32_t tid_payerw_sec   = 615;
static constexpr uint32_t tid_samekey_sec  = 616;
// The lowest table id, for the actions that must reach the first row of the whole index.
static constexpr uint32_t tid_first      = 0;
// The highest table_id the intrinsics accept.
static constexpr uint32_t tid_top        = std::numeric_limits<uint16_t>::max();

// The intrinsics tidbad can call, which are all those that take a table_id other than kv_set. The host test walks this
// list by position, so keep kv_intrinsic_probe_tests.cpp in step.
enum table_id_call : uint32_t {
   call_kv_get,
   call_kv_erase,
   call_kv_idx_store,
   call_kv_idx_update,
   call_kv_idx_erase,
   call_kv_it_create,
   call_kv_idx_create,
   call_kv_idx_find
};

// kv_status values (from chain/kv_context.hpp).
static constexpr int32_t KV_OK     = 0;
static constexpr int32_t KV_END    = 1;
static constexpr int32_t KV_NO_ROW = 2;
static constexpr int32_t KV_EXISTS = 3;

// kv_set_mode values (from chain/kv_context.hpp).
static constexpr uint32_t KV_UPSERT = 0;
static constexpr uint32_t KV_INSERT = 1;
static constexpr uint32_t KV_UPDATE = 2;

// First handle of the secondary pool (chain/kv_context.hpp: slot 0 plus the secondary tag).
static constexpr int32_t FIRST_SECONDARY_HANDLE = 0x00010000;

// Slots in each iterator pool (chain/config.hpp: max_kv_iterators).
static constexpr uint32_t POOL_SLOTS = 1024;

// From chain/config.hpp. Defaults unless chain admin overrides.
static constexpr uint32_t MAX_KEY_SIZE        = 256;
static constexpr uint32_t MAX_VALUE_SIZE      = 256 * 1024;
static constexpr uint32_t MAX_SEC_KEY_SIZE    = 256;
static constexpr uint32_t MAX_PREFIX_LIMIT    = 512;

// These name the shapes most probes use: upsert writes, value reads from offset 0, and moves and reads that want one
// field or none. The probes that pin a call's own behavior use the intrinsics directly.

static void must(int32_t status) {
   check(status == KV_OK, "KV write must return ok");
}

static int32_t put_row(uint32_t table_id, uint64_t payer, const void* key, uint32_t key_size,
                       const void* value, uint32_t value_size) {
   return kv_set(table_id, payer, key, key_size, value, value_size, KV_UPSERT);
}

static int32_t get_row(uint32_t table_id, uint64_t code, const void* key, uint32_t key_size,
                       void* value, uint32_t value_size) {
   return kv_get(table_id, code, key, key_size, 0, value, value_size);
}

static bool has_row(uint32_t table_id, uint64_t code, const void* key, uint32_t key_size) {
   return get_row(table_id, code, key, key_size, nullptr, 0) >= 0;
}

static int32_t it_create(uint32_t table_id, uint64_t code, const void* prefix, uint32_t prefix_size) {
   return kv_it_create(table_id, code, prefix, prefix_size, nullptr, 0);
}

static int32_t it_next(int32_t handle) {
   uint32_t key_size = 0, value_size = 0;
   return kv_it_next(handle, nullptr, 0, nullptr, 0, &key_size, &value_size);
}

static int32_t it_prev(int32_t handle) {
   uint32_t key_size = 0, value_size = 0;
   return kv_it_prev(handle, nullptr, 0, nullptr, 0, &key_size, &value_size);
}

static int32_t it_seek(int32_t handle, const void* seek, uint32_t seek_size) {
   uint32_t key_size = 0, value_size = 0;
   return kv_it_lower_bound(handle, seek, seek_size, nullptr, 0, nullptr, 0, &key_size, &value_size);
}

static int32_t it_status(int32_t handle) {
   uint32_t key_size = 0, value_size = 0;
   return kv_it_read(handle, 0, nullptr, 0, nullptr, 0, &key_size, &value_size);
}

static int32_t it_key(int32_t handle, void* dest, uint32_t dest_size, uint32_t* key_size) {
   uint32_t value_size = 0;
   return kv_it_read(handle, 0, dest, dest_size, nullptr, 0, key_size, &value_size);
}

static int32_t it_value(int32_t handle, uint32_t offset, void* dest, uint32_t dest_size, uint32_t* value_size) {
   uint32_t key_size = 0;
   return kv_it_read(handle, offset, nullptr, 0, dest, dest_size, &key_size, value_size);
}

static int32_t idx_find(uint64_t code, uint32_t table_id, const void* sec_key, uint32_t sec_key_size) {
   return kv_idx_find(table_id, code, nullptr, 0, sec_key, sec_key_size);
}

static int32_t idx_seek(uint64_t code, uint32_t table_id, const void* seek, uint32_t seek_size) {
   return kv_idx_create(table_id, code, nullptr, 0, seek, seek_size);
}

static int32_t idx_next(int32_t handle) {
   uint32_t sec_size = 0, pri_size = 0;
   return kv_idx_next(handle, nullptr, 0, nullptr, 0, &sec_size, &pri_size);
}

static int32_t idx_prev(int32_t handle) {
   uint32_t sec_size = 0, pri_size = 0;
   return kv_idx_prev(handle, nullptr, 0, nullptr, 0, &sec_size, &pri_size);
}

static int32_t idx_key(int32_t handle, void* dest, uint32_t dest_size, uint32_t* sec_size) {
   uint32_t pri_size = 0;
   return kv_idx_read(handle, dest, dest_size, nullptr, 0, sec_size, &pri_size);
}

static int32_t idx_pri(int32_t handle, void* dest, uint32_t dest_size, uint32_t* pri_size) {
   uint32_t sec_size = 0;
   return kv_idx_read(handle, nullptr, 0, dest, dest_size, &sec_size, pri_size);
}

static void prev_onto(int32_t handle, const void* key, uint32_t key_size, const char* msg) {
   char     buf[16];
   uint32_t size = 0, value_size = 0;
   check(kv_it_prev(handle, buf, sizeof(buf), nullptr, 0, &size, &value_size) == KV_OK, msg);
   check(size == key_size && memcmp(buf, key, key_size) == 0, msg);
}

// The index probes use 2-byte keys throughout, so these helpers need no sizes.
static constexpr uint32_t IDX_KEY_SIZE = 2;

static void same_entry(const char* sec, const char* sec_want, const char* pri, const char* pri_want,
                       uint32_t sec_size, uint32_t pri_size, const char* msg) {
   check(sec_size == IDX_KEY_SIZE && pri_size == IDX_KEY_SIZE, msg);
   check(memcmp(sec, sec_want, IDX_KEY_SIZE) == 0 && memcmp(pri, pri_want, IDX_KEY_SIZE) == 0, msg);
}

static void idx_prev_onto(int32_t handle, const char* sec_want, const char* pri_want, const char* msg) {
   char     sec[IDX_KEY_SIZE], pri[IDX_KEY_SIZE];
   uint32_t sec_size = 0, pri_size = 0;
   check(kv_idx_prev(handle, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, msg);
   same_entry(sec, sec_want, pri, pri_want, sec_size, pri_size, msg);
}

static void idx_next_onto(int32_t handle, const char* sec_want, const char* pri_want, const char* msg) {
   char     sec[IDX_KEY_SIZE], pri[IDX_KEY_SIZE];
   uint32_t sec_size = 0, pri_size = 0;
   check(kv_idx_next(handle, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, msg);
   same_entry(sec, sec_want, pri, pri_want, sec_size, pri_size, msg);
}

static void idx_on(int32_t handle, const char* sec_want, const char* pri_want, const char* msg) {
   char     sec[IDX_KEY_SIZE], pri[IDX_KEY_SIZE];
   uint32_t sec_size = 0, pri_size = 0;
   check(kv_idx_read(handle, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, msg);
   same_entry(sec, sec_want, pri, pri_want, sec_size, pri_size, msg);
}

static void idx_put(uint32_t table_id, uint64_t payer, const char* sec, const char* pri) {
   must(kv_idx_store(table_id, payer, pri, IDX_KEY_SIZE, sec, IDX_KEY_SIZE));
}

static void idx_del(uint32_t table_id, const char* sec, const char* pri) {
   must(kv_idx_erase(table_id, pri, IDX_KEY_SIZE, sec, IDX_KEY_SIZE));
}

static void idx_move(uint32_t table_id, uint64_t payer, const char* pri, const char* old_sec, const char* new_sec) {
   must(kv_idx_update(table_id, payer, pri, IDX_KEY_SIZE, old_sec, IDX_KEY_SIZE, new_sec, IDX_KEY_SIZE));
}

// Eight byte keys in byte order. Neighbours differ across the high bit, or in bytes a little-endian load would swap.
static constexpr uint32_t ORDER_KEYS     = 6;
static constexpr uint32_t ORDER_KEY_SIZE = sizeof(uint64_t);
static constexpr char     order_keys[ORDER_KEYS][ORDER_KEY_SIZE] = {
   {'\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00'},
   {'\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\xff'},
   {'\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x01', '\x00'},
   {'\x7f', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff'},
   {'\x80', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00'},
   {'\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff'},
};

static void same_order_key(const char* key, uint32_t key_size, uint32_t want, const char* msg) {
   check(key_size == ORDER_KEY_SIZE && memcmp(key, order_keys[want], ORDER_KEY_SIZE) == 0, msg);
}

class [[sysio::contract("kv_intrinsic_probe")]] kv_intrinsic_probe : public contract {
public:
   using contract::contract;

   // ==========================================================================
   // A. Accepted-behavior probes (bundle multiple checks in one action).
   // ==========================================================================

   // Zero-length VALUE is legal end to end.
   [[sysio::action]]
   void zval() {
      const uint64_t self = get_self().value;
      const char key_a[]  = {'A'};
      char       marker   = 0;
      char       buf[16]  = {};

      check(put_row(tid_zval, self, key_a, sizeof(key_a), &marker, 0) == KV_OK, "kv_set(value_size=0) must succeed");

      int32_t sz = get_row(tid_zval, self, key_a, sizeof(key_a), buf, sizeof(buf));
      check(sz == 0, "kv_get on empty-value row must return 0 (found, empty)");
      check(has_row(tid_zval, self, key_a, sizeof(key_a)), "a zero-size kv_get must find an empty-value row");

      // dest_size=0 path: return actual size without touching dest.
      sz = get_row(tid_zval, self, key_a, sizeof(key_a), &marker, 0);
      check(sz == 0, "kv_get(dest_size=0) on empty-value row must return 0");

      int32_t h = it_create(tid_zval, self, &marker, 0);
      check(it_status(h) == KV_OK, "iterator must be positioned at the row");
      uint32_t actual = 99;
      int32_t  st     = it_value(h, 0, buf, sizeof(buf), &actual);
      check(st == KV_OK && actual == 0, "kv_it_read on empty value must report value_size=0");
      kv_it_destroy(h);

      // Grow then shrink back to 0.
      const char val[] = "XYZ";
      must(put_row(tid_zval, self, key_a, sizeof(key_a), val, sizeof(val)));
      must(put_row(tid_zval, self, key_a, sizeof(key_a), &marker, 0));
      sz = get_row(tid_zval, self, key_a, sizeof(key_a), buf, sizeof(buf));
      check(sz == 0, "shrink back to empty failed");
   }

   // An entry with an empty key would stop every later snapshot, so these three actions must abort.
   [[sysio::action]]
   void zseckey() {
      const char pri[]  = {'P'};
      char       marker = 0;
      must(kv_idx_store(tid_zseck_sec, get_self().value, pri, sizeof(pri), &marker, 0));
   }

   [[sysio::action]]
   void zprikey() {
      const char sec[]  = {'S'};
      char       marker = 0;
      must(kv_idx_store(tid_zseck_sec, get_self().value, &marker, 0, sec, sizeof(sec)));
   }

   [[sysio::action]]
   void zsecupd() {
      const uint64_t self   = get_self().value;
      const char     pri[]  = {'P'};
      const char     sec[]  = {'S'};
      char           marker = 0;
      must(kv_idx_store(tid_zseck_sec, self, pri, sizeof(pri), sec, sizeof(sec)));
      must(kv_idx_update(tid_zseck_sec, self, pri, sizeof(pri), sec, sizeof(sec), &marker, 0));
   }

   // No entry has an empty primary key, so an update naming one finds nothing.
   [[sysio::action]]
   void zpriupd() {
      const char old_sec[] = {'S'};
      const char new_sec[] = {'T'};
      char       marker    = 0;
      (void)kv_idx_update(tid_zseck_sec, get_self().value, &marker, 0, old_sec, sizeof(old_sec), new_sec,
                          sizeof(new_sec));
   }

   // Keeps a row and an index entry with the shortest legal keys, for the snapshot round-trip test.
   [[sysio::action]]
   void idxminkeep() {
      const uint64_t self  = get_self().value;
      const char     pri[] = {'P'};
      const char     sec[] = {'S'};
      must(put_row(tid_minkey_pri, self, pri, sizeof(pri), "v", 1));
      must(kv_idx_store(tid_minkey_sec, self, pri, sizeof(pri), sec, sizeof(sec)));
   }

   // A second store of the same (sec_key, pri_key) pair returns exists and leaves the entry alone. dupclean removes it.
   [[sysio::action]]
   void dupstore() {
      const uint64_t self  = get_self().value;
      const char     pri[] = {'P'};
      const char     sec[] = {'S'};
      check(kv_idx_store(tid_dup, self, pri, sizeof(pri), sec, sizeof(sec)) == KV_OK, "first store must succeed");
      check(kv_idx_store(tid_dup, self, pri, sizeof(pri), sec, sizeof(sec)) == KV_EXISTS, "second store must report exists");
      // Payer 0 has nothing to reject: no entry would be created.
      check(kv_idx_store(tid_dup, 0, pri, sizeof(pri), sec, sizeof(sec)) == KV_EXISTS, "payer 0 must also report exists");
   }

   [[sysio::action]]
   void dupclean() {
      const char pri[] = {'P'};
      const char sec[] = {'S'};
      check(kv_idx_erase(tid_dup, pri, sizeof(pri), sec, sizeof(sec)) == KV_OK, "the stored entry must erase");
   }

   // Updating an entry onto another entry's (sec_key, pri_key) pair returns exists and leaves both. These entries were
   // written earlier in this transaction, where chainbase has no undo copy of the row and a failed modify removes it,
   // so this is the case that a catch around db.modify would lose.
   [[sysio::action]]
   void dupupdate() {
      const uint64_t self     = get_self().value;
      const char     pri[]    = {'P'};
      const char     sec_a[]  = {'A'};
      const char     sec_b[]  = {'B'};
      check(kv_idx_store(tid_dup, self, pri, sizeof(pri), sec_a, sizeof(sec_a)) == KV_OK, "store A");
      check(kv_idx_store(tid_dup, self, pri, sizeof(pri), sec_b, sizeof(sec_b)) == KV_OK, "store B");
      check(kv_idx_update(tid_dup, self, pri, sizeof(pri), sec_a, sizeof(sec_a), sec_b, sizeof(sec_b)) == KV_EXISTS,
            "update onto B must report exists");
      check(kv_idx_erase(tid_dup, pri, sizeof(pri), sec_a, sizeof(sec_a)) == KV_OK, "entry A must survive");
      check(kv_idx_erase(tid_dup, pri, sizeof(pri), sec_b, sizeof(sec_b)) == KV_OK, "entry B must survive");
   }

   // dupseed, dupupdkept and dupcheck repeat the collision on entries stored by an earlier transaction.
   [[sysio::action]]
   void dupseed() {
      const uint64_t self     = get_self().value;
      const char     pri[]    = {'P'};
      const char     sec_a[]  = {'A'};
      const char     sec_b[]  = {'B'};
      check(kv_idx_store(tid_dupkept, self, pri, sizeof(pri), sec_a, sizeof(sec_a)) == KV_OK, "store A");
      check(kv_idx_store(tid_dupkept, self, pri, sizeof(pri), sec_b, sizeof(sec_b)) == KV_OK, "store B");
   }

   [[sysio::action]]
   void dupupdkept() {
      const char pri[]    = {'P'};
      const char sec_a[]  = {'A'};
      const char sec_b[]  = {'B'};
      check(kv_idx_update(tid_dupkept, get_self().value, pri, sizeof(pri), sec_a, sizeof(sec_a), sec_b, sizeof(sec_b)) ==
               KV_EXISTS,
            "update onto B must report exists");
   }

   [[sysio::action]]
   void dupcheck() {
      const uint64_t self     = get_self().value;
      const char     pri[]    = {'P'};
      const char     sec_a[]  = {'A'};
      const char     sec_b[]  = {'B'};
      check(idx_find(self, tid_dupkept, sec_a, sizeof(sec_a)) >= 0, "entry A must survive");
      check(idx_find(self, tid_dupkept, sec_b, sizeof(sec_b)) >= 0, "entry B must survive");
      check(kv_idx_erase(tid_dupkept, pri, sizeof(pri), sec_a, sizeof(sec_a)) == KV_OK, "erase A");
      check(kv_idx_erase(tid_dupkept, pri, sizeof(pri), sec_b, sizeof(sec_b)) == KV_OK, "erase B");
   }

   // Updating an entry to the key it already has collides with nothing.
   [[sysio::action]]
   void updsamekey() {
      const uint64_t self  = get_self().value;
      const char     pri[] = {'P'};
      const char     sec[] = {'S'};
      check(kv_idx_store(tid_dup, self, pri, sizeof(pri), sec, sizeof(sec)) == KV_OK, "store");
      check(kv_idx_update(tid_dup, self, pri, sizeof(pri), sec, sizeof(sec), sec, sizeof(sec)) == KV_OK,
            "update to the same key must succeed");
      check(kv_idx_erase(tid_dup, pri, sizeof(pri), sec, sizeof(sec)) == KV_OK, "erase");
   }

   // A zero-size kv_get is a presence test that agrees with a read into a buffer.
   [[sysio::action]]
   void contcns() {
      const uint64_t self = get_self().value;
      const char k1[] = {'K', '1'};
      const char k2[] = {'K', '2'};

      // Row present: both find it.
      must(put_row(tid_contcns, self, k1, sizeof(k1), "v", 1));
      check(has_row(tid_contcns, self, k1, sizeof(k1)), "contains: present row");
      char tmp[4];
      check(get_row(tid_contcns, self, k1, sizeof(k1), tmp, sizeof(tmp)) == 1,
            "get: present row returns size");

      // Row absent: both report -1.
      check(!has_row(tid_contcns, self, k2, sizeof(k2)), "contains: absent row");
      check(get_row(tid_contcns, self, k2, sizeof(k2), tmp, sizeof(tmp)) == -1,
            "get: absent row returns -1");

      // Wrong code: -1 even when the key exists in receiver's table.
      const uint64_t foreign_code = name{"sysio"_n}.value;
      check(!has_row(tid_contcns, foreign_code, k1, sizeof(k1)),
            "contains: cross-code must not find receiver's row");
      check(get_row(tid_contcns, foreign_code, k1, sizeof(k1), tmp, sizeof(tmp)) == -1,
            "get: cross-code must not find receiver's row");
   }

   // Position is by key, so a read after the row at the current key is erased reports no_row at once and touches no
   // erased bytes.
   [[sysio::action]]
   void itinvl() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      must(put_row(tid_itinvl, self, k, sizeof(k), "v", 1));

      char     marker = 0;
      char     buf[8] = {};
      uint32_t h = it_create(tid_itinvl, self, &marker, 0);
      check(it_status(h) == KV_OK, "iterator must start at the row");

      must(kv_erase(tid_itinvl, k, sizeof(k)));

      uint32_t actual = 99;
      int32_t  st     = it_key(h, buf, sizeof(buf), &actual);
      check(st == KV_NO_ROW, "kv_it_read after erase must report no_row");
      check(actual == 0, "kv_it_read after erase must not report a byte count");
      kv_it_destroy(h);
   }

   // Iterator in end state: reads and moves all behave deterministically.
   [[sysio::action]]
   void endstat() {
      const uint64_t self = get_self().value;
      char marker = 0;

      // Empty table -> iterator starts in end state.
      uint32_t h = it_create(tid_endstat, self, &marker, 0);
      check(it_status(h) == KV_END, "iter on empty (code,table) must be end");

      // All data reads in end state: return end, actual_size=0.
      char     buf[4];
      uint32_t actual = 99;
      check(it_key(h, buf, sizeof(buf), &actual) == KV_END, "key in end state");
      check(actual == 0, "key actual_size must be 0 in end state");
      actual = 99;
      check(it_value(h, 0, buf, sizeof(buf), &actual) == KV_END, "value in end state");
      check(actual == 0, "value actual_size must be 0 in end state");

      // next in end stays in end.
      check(it_next(h) == KV_END, "next in end must stay end");
      kv_it_destroy(h);
   }

   // kv_it_read offset handling: offset == value_size is the legit "read zero from the tail" boundary, and
   // offset > value_size must be safe.
   [[sysio::action]]
   void offsa() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      const char v[] = "abcdef"; // 7 bytes incl nul
      must(put_row(tid_offsa, self, k, sizeof(k), v, sizeof(v)));

      char     marker = 0;
      char     buf[8] = {};
      uint32_t h      = it_create(tid_offsa, self, &marker, 0);
      check(it_status(h) == KV_OK, "iter must find the row");

      // offset = actual_size: legal, reads 0 bytes, status stays ok, actual_size reported.
      uint32_t actual = 99;
      int32_t  st     = it_value(h, sizeof(v), buf, sizeof(buf), &actual);
      check(st == KV_OK, "offset=actual_size must keep status ok");
      check(actual == sizeof(v), "actual_size must be reported even at tail offset");

      // offset > actual_size: must not OOB-read.
      actual = 99;
      st     = it_value(h, sizeof(v) + 100, buf, sizeof(buf), &actual);
      check(st == KV_OK, "offset>actual_size must not change status");
      check(actual == sizeof(v), "actual_size must be reported even for over-read");

      // dest_size=0: return size, no write.
      actual = 99;
      st     = it_value(h, 0, &marker, 0, &actual);
      check(st == KV_OK && actual == sizeof(v),
            "dest_size=0 must return actual_size without writing");

      kv_it_destroy(h);
      must(kv_erase(tid_offsa, k, sizeof(k)));
   }

   // ==========================================================================
   // B. Rejection probes (each action calls the problematic intrinsic ONCE and
   //    expects the host to throw, aborting the action).
   // ==========================================================================

   // kv_set with empty key -> kv_empty_key
   [[sysio::action]]
   void zkeyset() {
      const uint64_t self = get_self().value;
      char marker = 0;
      must(put_row(tid_zkey, self, &marker, 0, "v", 1));
   }

   // kv_erase with empty key -> kv_empty_key
   [[sysio::action]]
   void zkeyerz() {
      char marker = 0;
      must(kv_erase(tid_zkey, &marker, 0));
   }

   // kv_set with oversize key -> kv_key_too_large
   [[sysio::action]]
   void bigkey() {
      const uint64_t self = get_self().value;
      static char huge[MAX_KEY_SIZE + 1] = {}; // 257 zero bytes
      must(put_row(tid_big, self, huge, sizeof(huge), "v", 1));
   }

   // kv_set with oversize value -> kv_value_too_large
   [[sysio::action]]
   void bigval() {
      const uint64_t self = get_self().value;
      const char k[] = {'B'};
      static char huge[MAX_VALUE_SIZE + 1] = {}; // 256 KiB + 1
      must(put_row(tid_big, self, k, sizeof(k), huge, sizeof(huge)));
   }

   // kv_idx_store with oversize sec_key -> kv_secondary_key_too_large
   [[sysio::action]]
   void bigsec() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      must(put_row(tid_bigsec_pri, self, pk, sizeof(pk), "v", 1));
      static char huge[MAX_SEC_KEY_SIZE + 1] = {}; // 257 bytes
      must(kv_idx_store(tid_bigsec_sec, self, pk, sizeof(pk), huge, sizeof(huge)));
   }

   // kv_idx_store with oversize pri_key -> kv_key_too_large.
   // Distinct SYS_ASSERT site from bigsec (pri_key check vs sec_key check).
   [[sysio::action]]
   void bigpri() {
      const uint64_t self = get_self().value;
      static char huge[MAX_KEY_SIZE + 1] = {}; // 257 bytes
      must(kv_idx_store(tid_bigpri, self, huge, sizeof(huge), "s", 1));
   }

   // kv_it_create with prefix over the absolute 512 B limit -> kv_key_too_large
   [[sysio::action]]
   void bigpfx() {
      const uint64_t self = get_self().value;
      static char huge[MAX_PREFIX_LIMIT + 1] = {}; // 513 bytes
      it_create(tid_big, self, huge, sizeof(huge));
   }

   // table_id > uint16 max -> kv_invalid_table_id via checked_table_id
   [[sysio::action]]
   void tidmax() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      must(put_row(static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1u, self, k, sizeof(k), "v", 1));
   }

   // kv_it_read called with reserved-bit-set handle -> kv_invalid_iterator
   [[sysio::action]]
   void badh() {
      it_status(0xDEADBEEFu); // bits outside slot-mask|tag-bit are set
   }

   // Primary kv_it_next called on a secondary handle -> kv_invalid_iterator
   [[sysio::action]]
   void crshp() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      must(put_row(tid_crossh_pri, self, pk, sizeof(pk), "v", 1));
      must(kv_idx_store(tid_crossh_sec, self, pk, sizeof(pk), "s", 1));

      int32_t sh = idx_find(self, tid_crossh_sec, "s", 1);
      check(sh >= 0, "secondary handle required to probe cross-pool rejection");
      // Cleanup will not run -- kv_it_next must throw here.
      it_next(sh);
   }

   // Secondary kv_idx_next called on a primary handle -> kv_invalid_iterator
   [[sysio::action]]
   void crshs() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      must(put_row(tid_crossh_pri, self, k, sizeof(k), "v", 1));
      char marker = 0;
      uint32_t ph = it_create(tid_crossh_pri, self, &marker, 0);
      // kv_idx_next expects a secondary handle -> must reject.
      idx_next(ph);
   }

   // kv_idx_update with oversize new_sec_key -> kv_secondary_key_too_large.
   // The size check runs before the entry-exists lookup, so the call site can
   // use any pri_key bytes and a never-stored old_sec_key and still trip the
   // independent SYS_ASSERT.
   [[sysio::action]]
   void idxupdbig() {
      const uint64_t self = get_self().value;
      const char dummy_pri[] = {'x'};
      static char huge[MAX_SEC_KEY_SIZE + 1] = {}; // 257 bytes
      must(kv_idx_update(tid_idxupbig, self, dummy_pri, sizeof(dummy_pri), "a", 1, huge, sizeof(huge)));
   }

   // kv_it_read on a handle whose slot was released -> kv_invalid_iterator.
   // kv_handle_check_reserved_zero passes (handle has clean bits); the slot
   // pool's in_use check catches it.
   [[sysio::action]]
   void uaf() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      must(put_row(tid_uaf, self, k, sizeof(k), "v", 1));
      char marker = 0;
      uint32_t h = it_create(tid_uaf, self, &marker, 0);
      kv_it_destroy(h);
      // Use after destroy: the handle is numerically valid but its slot is not in use.
      it_status(h);
   }

   // kv_it_destroy called twice on the same handle -> kv_invalid_iterator on the
   // second call (slot already released).
   [[sysio::action]]
   void dbldest() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      must(put_row(tid_dbldest, self, k, sizeof(k), "v", 1));
      char marker = 0;
      uint32_t h = it_create(tid_dbldest, self, &marker, 0);
      kv_it_destroy(h);
      kv_it_destroy(h);
   }

   // kv_set with overlapping key and value spans: host must deep-copy both
   // independently. This exercises whether the host reads key bytes before
   // any value processing could perturb them (or vice versa).
   [[sysio::action]]
   void alias() {
      const uint64_t self = get_self().value;
      // Shared buffer. key occupies [0..4), value occupies [2..7). They overlap
      // on bytes 2,3.
      char buf[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 0};
      check(put_row(tid_alias, self, buf, 4, buf + 2, 5) == KV_OK, "kv_set with aliased key/value must succeed");

      char out[8] = {};
      int32_t sz = get_row(tid_alias, self, buf, 4, out, sizeof(out));
      check(sz == 5, "stored value size must equal original span length");
      // Original key bytes survive whatever copy order the host chose.
      check(out[0] == 'c' && out[1] == 'd' && out[2] == 'e' && out[3] == 'f' && out[4] == 'g',
            "aliased value bytes must round-trip unchanged");
      must(kv_erase(tid_alias, buf, 4));
   }

   // Exhausts the 1024-slot primary iterator pool: the 1025th kv_it_create
   // must throw kv_iterator_limit_exceeded. Uses a single stored row so every
   // create positions successfully and reserves a slot (iterators in end state
   // still consume a slot).
   [[sysio::action]]
   void itlimit() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      must(put_row(tid_payzero /* reuse unrelated tid */, self, k, sizeof(k), "v", 1));
      char marker = 0;
      // Allocate all 1024 slots.
      for (uint32_t i = 0; i < 1024; ++i) {
         uint32_t h = it_create(tid_payzero, self, &marker, 0);
         (void)h;
      }
      // 1025th must trap.
      it_create(tid_payzero, self, &marker, 0);
   }

   // ==========================================================================
   // D. Missing rows are a status, not an abort.
   // ==========================================================================

   [[sysio::action]]
   void erasmis() {
      const char k[] = {'M'};
      check(kv_erase(tid_erasmis, k, sizeof(k)) == KV_NO_ROW, "kv_erase of a free key must report no_row");
   }

   // (sec_key, pri_key) pair never stored.
   [[sysio::action]]
   void idxrmmis() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      must(put_row(tid_idxrm_pri, self, pk, sizeof(pk), "v", 1));
      check(kv_idx_erase(tid_idxrm_sec, pk, sizeof(pk), "s", 1) == KV_NO_ROW,
            "kv_idx_erase of a missing entry must report no_row");
   }

   [[sysio::action]]
   void idxupmis() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      must(put_row(tid_idxup_pri, self, pk, sizeof(pk), "v", 1));
      check(kv_idx_update(tid_idxup_sec, self, pk, sizeof(pk), "old", 3, "new", 3) == KV_NO_ROW,
            "kv_idx_update of a missing entry must report no_row");
   }

   // ==========================================================================
   // E. Additional accepted-behavior probes.
   // ==========================================================================

   // kv_idx_find miss returns -1 and allocates NO slot. Spamming
   // misses must not exhaust the iterator pool.
   [[sysio::action]]
   void findmis() {
      const uint64_t self = get_self().value;
      // 2x the slot pool so a leak would blow up (1024 slots; try 2048).
      for (uint32_t i = 0; i < 2048; ++i) {
         int32_t h = idx_find(self, tid_findmis, "nope", 4);
         check(h == -1, "kv_idx_find miss must return -1");
      }
   }

   // kv_idx_create past the end of a non-empty table returns an end-state handle, so the caller can kv_idx_prev back
   // into the last entry.
   [[sysio::action]]
   void lbpast() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      must(put_row(tid_lbpast_pri, self, pk, sizeof(pk), "v", 1));
      must(kv_idx_store(tid_lbpast_sec, self, pk, sizeof(pk), "m", 1));

      // Seek past the only entry.
      int32_t h = idx_seek(self, tid_lbpast_sec, "z", 1);
      check(h >= 0, "kv_idx_create past the end must return a valid handle");
      // next from end stays at end.
      check(idx_next(h) == KV_END,
            "handle must be in end state (next from end must stay end)");

      // prev from end into the last entry.
      check(idx_prev(h) == KV_OK,
            "prev from end must land on last entry");
      char     buf[4] = {};
      uint32_t actual = 99;
      idx_key(h, buf, sizeof(buf), &actual);
      check(actual == 1 && buf[0] == 'm',
            "last entry sec_key must be the one inserted");
      kv_idx_destroy(h);

      // Cleanup.
      must(kv_idx_erase(tid_lbpast_sec, pk, sizeof(pk), "m", 1));
      must(kv_erase(tid_lbpast_pri, pk, sizeof(pk)));
   }

   // kv_idx_create on an empty (code, table_id) still returns a handle, at end. Destroying it frees the slot.
   [[sysio::action]]
   void lbempty() {
      const uint64_t self = get_self().value;
      // 2x slot pool to prove each destroy returns its slot.
      for (uint32_t i = 0; i < 2048; ++i) {
         int32_t h = idx_seek(self, tid_lbempty, "x", 1);
         check(h >= 0, "kv_idx_create must always return a handle");
         check(idx_next(h) == KV_END, "handle on an empty table must be at end");
         kv_idx_destroy(h);
      }
   }

   // kv_idx_read on a secondary iterator after the referenced primary
   // row is erased mid-txn: master stores pri_key bytes inline on the secondary
   // entry, so the lookup returns KV_OK with the original pri_key bytes -- the
   // host does NOT auto-invalidate dangling references. Auto-cascade would be a
   // protocol change. Pinning the no-cascade behavior so a future refactor
   // that swaps inline pri_key bytes for a chainbase-id lookup would fail here.
   [[sysio::action]]
   void prmera() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      must(put_row(tid_prmera_pri, self, pk, sizeof(pk), "v", 1));
      must(kv_idx_store(tid_prmera_sec, self, pk, sizeof(pk), "s", 1));

      int32_t h = idx_find(self, tid_prmera_sec, "s", 1);
      check(h >= 0, "kv_idx_find must return a valid handle");

      // Erase primary, leaving the secondary entry's pri_key bytes intact.
      must(kv_erase(tid_prmera_pri, pk, sizeof(pk)));

      char     buf[4] = {};
      uint32_t actual = 99;
      int32_t  st     = idx_pri(h, buf, sizeof(buf), &actual);
      check(st == KV_OK,
            "pri_key read after primary erase returns KV_OK (pri_key bytes are stored inline)");
      check(actual == sizeof(pk) && buf[0] == 'P',
            "pri_key read returns the original stored pri_key bytes");

      kv_idx_destroy(h);
      must(kv_idx_erase(tid_prmera_sec, pk, sizeof(pk), "s", 1));
   }

   // kv_it_prev called when iterator is positioned at begin transitions to
   // end. Symmetric to kv_it_next at end covered in endstat.
   [[sysio::action]]
   void itprvbgn() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      must(put_row(tid_itprvbg, self, k, sizeof(k), "v", 1));
      char     marker = 0;
      uint32_t h      = it_create(tid_itprvbg, self, &marker, 0);
      check(it_status(h) == KV_OK, "iter must start at the row");
      // Only one row under this (code, table_id); prev goes to end.
      check(it_prev(h) == KV_END, "prev at begin must transition to end");
      kv_it_destroy(h);
      must(kv_erase(tid_itprvbg, k, sizeof(k)));
   }

   // ==========================================================================
   // F. Cross-pool rejection matrix.
   //    Primary intrinsics must reject secondary handles, and vice versa.
   //    Each site has its own SYS_ASSERT(!kv_handle_is_secondary(...)) or
   //    SYS_ASSERT(kv_handle_is_secondary(...)). Tested here so no individual
   //    guard can rot independently.
   // ==========================================================================

   // Helper: returns a live secondary handle for the current action. The caller
   // must kv_idx_destroy it eventually (but rejection tests abort before that).
   int32_t make_sec_handle_() {
      const uint64_t self = get_self().value;
      const char pk[] = {'X'};
      must(put_row(tid_cpool, self, pk, sizeof(pk), "v", 1));
      must(kv_idx_store(tid_cpool_sec, self, pk, sizeof(pk), "s", 1));
      int32_t h = idx_find(self, tid_cpool_sec, "s", 1);
      check(h >= 0, "precondition: secondary handle must allocate");
      return h;
   }

   // Helper: returns a live primary handle.
   int32_t make_pri_handle_() {
      const uint64_t self = get_self().value;
      const char k[] = {'Y'};
      must(put_row(tid_cpool, self, k, sizeof(k), "v", 1));
      char marker = 0;
      return it_create(tid_cpool, self, &marker, 0);
   }

   [[sysio::action]] void crshdst()  { kv_it_destroy(make_sec_handle_()); }
   [[sysio::action]] void crshprv()  { it_prev(make_sec_handle_()); }
   [[sysio::action]] void crshlb()   { it_seek(make_sec_handle_(), "k", 1); }
   [[sysio::action]] void crshrd()   { it_status(make_sec_handle_()); }

   [[sysio::action]] void crsiprv()  { idx_prev(make_pri_handle_()); }
   [[sysio::action]] void crsilb()   {
      uint32_t sec_size = 0, pri_size = 0;
      kv_idx_lower_bound(make_pri_handle_(), "k", 1, nullptr, 0, nullptr, 0, &sec_size, &pri_size);
   }
   [[sysio::action]] void crsird()   {
      uint32_t sec_size = 0, pri_size = 0;
      kv_idx_read(make_pri_handle_(), nullptr, 0, nullptr, 0, &sec_size, &pri_size);
   }
   [[sysio::action]] void crsidst()  { kv_idx_destroy(make_pri_handle_()); }

   // -1 is what a call returns for "no handle". It is not a handle, so passing it on must abort.
   [[sysio::action]] void negdst()   { kv_it_destroy(-1); }
   [[sysio::action]] void negidst()  { kv_idx_destroy(-1); }
   [[sysio::action]] void negrd()    { it_status(-1); }
   [[sysio::action]] void negird()   { idx_next(-1); }

   // ==========================================================================
   // H. Resource and invariant probes a malicious contract could abuse.
   // ==========================================================================

   // Secondary iterator pool exhaustion: 1024 successful kv_idx_find
   // calls each reserve a slot. The 1025th must throw kv_iterator_limit_exceeded.
   // Symmetric to itlimit but on the independent secondary pool.
   [[sysio::action]]
   void secilim() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      must(put_row(tid_secilim_p, self, pk, sizeof(pk), "v", 1));
      must(kv_idx_store(tid_secilim_s, self, pk, sizeof(pk), "s", 1));
      // 1024 slots. Never destroying so the pool fills up.
      for (uint32_t i = 0; i < 1024; ++i) {
         int32_t h = idx_find(self, tid_secilim_s, "s", 1);
         (void)h;
      }
      // 1025th must trap.
      idx_find(self, tid_secilim_s, "s", 1);
   }

   // Exact-max-size key (256) and value (256 KiB) succeed end to end.
   // Pins the inclusive-upper-bound behavior of the max checks. bigkey and
   // bigval cover MAX+1; this pins MAX itself as accepted.
   [[sysio::action]]
   void maxok() {
      const uint64_t self = get_self().value;
      static char max_key[MAX_KEY_SIZE];       // 256 bytes
      static char max_val[MAX_VALUE_SIZE];     // 256 KiB
      for (uint32_t i = 0; i < MAX_KEY_SIZE; ++i) max_key[i] = static_cast<char>(i & 0xFF);
      max_val[0]                = 'H';
      max_val[MAX_VALUE_SIZE-1] = 'T';
      check(put_row(tid_maxok, self, max_key, MAX_KEY_SIZE, max_val, MAX_VALUE_SIZE) == KV_OK,
            "kv_set with max-size key and value must succeed");
      check(has_row(tid_maxok, self, max_key, MAX_KEY_SIZE), "max-size row must be found");
      // Spot-check round-trip at two offsets that would catch truncation.
      static char sink[MAX_VALUE_SIZE] = {};
      int32_t sz = get_row(tid_maxok, self, max_key, MAX_KEY_SIZE, sink, MAX_VALUE_SIZE);
      check(sz == static_cast<int32_t>(MAX_VALUE_SIZE), "kv_get must report full max size");
      check(sink[0] == 'H' && sink[MAX_VALUE_SIZE-1] == 'T', "kv_get must deliver full bytes");
      must(kv_erase(tid_maxok, max_key, MAX_KEY_SIZE));
   }

   // Dangling-secondary invariant: the host does NOT auto-remove secondary
   // entries when the referenced primary row is erased. A contract that erases
   // a primary without first removing its secondaries leaves state that can
   // still be found, updated, and read. Master stores pri_key bytes inline on
   // the secondary entry; kv_idx_read returns those bytes even after
   // the primary is gone. Pinning this behavior because "auto-cascade" would
   // be a protocol change. A malicious contract can't crash the chain with it
   // but can bloat its own RAM bill with orphan secondaries indefinitely --
   // worth documenting.
   [[sysio::action]]
   void danglng() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      must(put_row(tid_dangl_pri, self, pk, sizeof(pk), "v", 1));
      must(kv_idx_store(tid_dangl_sec, self, pk, sizeof(pk), "s", 1));
      must(kv_erase(tid_dangl_pri, pk, sizeof(pk)));

      // Dangling secondary still findable.
      int32_t h = idx_find(self, tid_dangl_sec, "s", 1);
      check(h >= 0, "dangling secondary entry must still be findable");

      // The read returns the stored pri_key bytes; host does not check
      // whether the referenced primary row still exists.
      char     buf[4] = {};
      uint32_t actual = 99;
      int32_t  st     = idx_pri(h, buf, sizeof(buf), &actual);
      check(st == KV_OK,
            "pri_key read on dangling secondary returns KV_OK (no auto-invalidation)");
      check(actual == sizeof(pk) && buf[0] == 'P',
            "pri_key read returns the originally stored pri_key bytes");
      kv_idx_destroy(h);

      // Mutation still succeeds: the (receiver, tid, old_sec, pri_key) composite
      // still resolves to a live kv_index_object even though the referenced
      // primary is gone.
      must(kv_idx_update(tid_dangl_sec, self, pk, sizeof(pk), "s", 1, "s2", 2));
      h = idx_find(self, tid_dangl_sec, "s2", 2);
      check(h >= 0, "post-update dangling entry must still be findable at new sec_key");
      kv_idx_destroy(h);

      // Cleanup (required, else RAM leaks).
      must(kv_idx_erase(tid_dangl_sec, pk, sizeof(pk), "s2", 2));
   }

   // Primary/secondary table_id coupling is CONVENTIONAL, not enforced.
   // kv_idx_store takes pri_key bytes as-is and stores them inline on the
   // secondary entry; no validation that the bytes correspond to any primary
   // row in any specific table. A contract can attach a secondary entry at
   // table S referencing pri_key bytes that happen to match a primary in
   // table A (same receiver), or that match no primary at all. Pinning this
   // behavior: if a future hardening cross-checks the pri_key against a
   // specific primary table, this test updates along with it.
   [[sysio::action]]
   void prixtab() {
      const uint64_t self = get_self().value;
      const char pk_a[] = {'A'};
      must(put_row(tid_prixtab_a, self, pk_a, sizeof(pk_a), "va", 2));

      // Secondary entry in tid_prixtab_s referencing pri_key bytes that live
      // in tid_prixtab_a.
      must(kv_idx_store(tid_prixtab_s, self, pk_a, sizeof(pk_a), "s", 1));

      int32_t h = idx_find(self, tid_prixtab_s, "s", 1);
      check(h >= 0, "cross-table pri_key reference must succeed");

      // The read returns the stored pri_key bytes verbatim.
      char     buf[4] = {};
      uint32_t actual = 99;
      int32_t  st     = idx_pri(h, buf, sizeof(buf), &actual);
      check(st == KV_OK && actual == sizeof(pk_a) && buf[0] == 'A',
            "cross-table pri_key read must return the stored pri_key bytes");
      kv_idx_destroy(h);

      // Cleanup.
      must(kv_idx_erase(tid_prixtab_s, pk_a, sizeof(pk_a), "s", 1));
      must(kv_erase(tid_prixtab_a, pk_a, sizeof(pk_a)));
   }

   // Inline actions get a fresh apply_context with independent iterator pools, so a handle the parent leaves open
   // means nothing to the child.
   [[sysio::action]]
   void inlparnt() {
      open_handle_and_send("inlchild"_n);
   }

   [[sysio::action]]
   void inlchild() {
      it_status(0); // must throw kv_invalid_iterator: this context has allocated nothing
   }

   [[sysio::action]]
   void inlpar2() {
      open_handle_and_send("inlchild2"_n);
   }

   [[sysio::action]]
   void inlchild2() {
      const uint64_t self   = get_self().value;
      const char     k[]    = {'K'};
      char           marker = 0;
      check(it_create(tid_inliso, self, &marker, 0) == 0, "child pool must start at slot 0");
      check(kv_erase(tid_inliso, k, sizeof(k)) == KV_OK, "child must erase the parent's row");
   }

   // leakiter leaves iterators open and notifies two contracts, which do the same. Each must start from slot 0.
   [[sysio::action]]
   void leakiter(name first, name second) {
      const uint64_t self  = get_self().value;
      const char     pri[] = {'P'};
      const char     sec[] = {'S'};
      must(kv_idx_store(tid_leak_sec, self, pri, sizeof(pri), sec, sizeof(sec)));
      open_both_pools();
      require_recipient(first);
      require_recipient(second);
   }

   [[sysio::on_notify("*::leakiter")]]
   void onleakiter(name first, name second) {
      open_both_pools();
   }

   // leakuse and leakusesec leave a handle open; using that handle from the notified contract must throw.
   [[sysio::action]]
   void leakuse(name notified) {
      char marker = 0;
      check(it_create(tid_leak_pri, get_self().value, &marker, 0) == 0, "first primary alloc must be slot 0");
      require_recipient(notified);
   }

   [[sysio::on_notify("*::leakuse")]]
   void onleakuse(name notified) {
      it_status(0);
   }

   [[sysio::action]]
   void leakusesec(name notified) {
      const uint64_t self  = get_self().value;
      const char     pri[] = {'Q'};
      const char     sec[] = {'T'};
      must(kv_idx_store(tid_leak_sec, self, pri, sizeof(pri), sec, sizeof(sec)));
      check(idx_find(self, tid_leak_sec, sec, sizeof(sec)) == FIRST_SECONDARY_HANDLE,
            "first secondary alloc must be slot 0");
      require_recipient(notified);
   }

   [[sysio::on_notify("*::leakusesec")]]
   void onleakusec(name notified) {
      idx_next(FIRST_SECONDARY_HANDLE);
   }

   // kv_set creating a row with payer=0 is rejected. 0 is the CDT `same_payer` sentinel (name{}),
   // which keeps an existing row's payer. A brand-new row has no existing payer to keep, so the
   // host throws invalid_table_payer rather than silently billing the receiver. CDT wrappers never
   // emit payer=0 for a new row; a raw intrinsic caller can -- this probes that boundary. kv_set
   // traps, so the transaction aborts on this call. payzero upserts, and payzeroins inserts.
   [[sysio::action]]
   void payzero() {
      const char k[] = {'K'};
      must(put_row(tid_payzero, /*payer=*/0, k, sizeof(k), "v", 1));
   }

   [[sysio::action]]
   void payzeroins() {
      const char k[] = {'L'};
      (void)kv_set(tid_payzero, /*payer=*/0, k, sizeof(k), "v", 1, KV_INSERT);
   }

   // A mode other than upsert, insert or update is misuse, whatever the row's state.
   [[sysio::action]]
   void badmode() {
      const char k[] = {'K'};
      (void)kv_set(tid_writes, get_self().value, k, sizeof(k), "v", 1, KV_UPDATE + 1);
   }

   // ==========================================================================
   // I. Every status from every write.
   // ==========================================================================

   [[sysio::action]]
   void wrstatus() {
      const uint64_t self = get_self().value;
      const char k[]      = {'K'};
      char       buf[4]   = {};

      // kv_set by mode, on a free key and on a taken one. A write that reports a miss leaves the row as it was.
      check(kv_set(tid_writes, self, k, sizeof(k), "u", 1, KV_UPDATE) == KV_NO_ROW, "update on a free key");
      check(!has_row(tid_writes, self, k, sizeof(k)), "update on a free key must not create it");
      check(kv_set(tid_writes, self, k, sizeof(k), "a", 1, KV_INSERT) == KV_OK, "insert on a free key");
      check(kv_set(tid_writes, self, k, sizeof(k), "b", 1, KV_INSERT) == KV_EXISTS, "insert on a taken key");
      check(get_row(tid_writes, self, k, sizeof(k), buf, sizeof(buf)) == 1 && buf[0] == 'a', "insert must not overwrite");
      check(kv_set(tid_writes, self, k, sizeof(k), "c", 1, KV_UPDATE) == KV_OK, "update on a taken key");
      check(get_row(tid_writes, self, k, sizeof(k), buf, sizeof(buf)) == 1 && buf[0] == 'c', "update must write");
      check(kv_set(tid_writes, self, k, sizeof(k), "d", 1, KV_UPSERT) == KV_OK, "upsert on a taken key");
      check(get_row(tid_writes, self, k, sizeof(k), buf, sizeof(buf)) == 1 && buf[0] == 'd', "upsert must write");

      check(kv_erase(tid_writes, k, sizeof(k)) == KV_OK, "erase on a taken key");
      check(kv_erase(tid_writes, k, sizeof(k)) == KV_NO_ROW, "erase on a free key");
      check(kv_set(tid_writes, self, k, sizeof(k), "e", 1, KV_UPSERT) == KV_OK, "upsert on a free key");
      check(kv_erase(tid_writes, k, sizeof(k)) == KV_OK, "the upserted row must erase");

      // Index entries.
      const char pri[] = {'P'};
      check(kv_idx_erase(tid_writes_sec, pri, sizeof(pri), "a", 1) == KV_NO_ROW, "index erase on a missing entry");
      check(kv_idx_update(tid_writes_sec, self, pri, sizeof(pri), "a", 1, "b", 1) == KV_NO_ROW,
            "index update on a missing entry");
      check(kv_idx_store(tid_writes_sec, self, pri, sizeof(pri), "a", 1) == KV_OK, "index store");
      check(kv_idx_store(tid_writes_sec, self, pri, sizeof(pri), "a", 1) == KV_EXISTS, "index store on a taken pair");
      check(kv_idx_store(tid_writes_sec, self, pri, sizeof(pri), "b", 1) == KV_OK, "index store of a second key");
      check(kv_idx_update(tid_writes_sec, self, pri, sizeof(pri), "a", 1, "b", 1) == KV_EXISTS,
            "index update onto a taken pair");
      check(kv_idx_update(tid_writes_sec, self, pri, sizeof(pri), "a", 1, "c", 1) == KV_OK, "index update");
      check(kv_idx_erase(tid_writes_sec, pri, sizeof(pri), "a", 1) == KV_NO_ROW, "the old key must be gone");
      check(kv_idx_erase(tid_writes_sec, pri, sizeof(pri), "b", 1) == KV_OK, "index erase b");
      check(kv_idx_erase(tid_writes_sec, pri, sizeof(pri), "c", 1) == KV_OK, "index erase c");
   }

   // Payer 0 on a row that exists: insert reports exists, and upsert and update keep the row's payer. The host test
   // pays with another account and checks both accounts' RAM. Update on a missing row reports no_row.
   [[sysio::action]]
   void paymodes(uint32_t mode, bool exists) {
      const char k[] = {'M'};
      const char v[] = "vv";
      const int32_t st = kv_set(tid_paymodes, 0, k, sizeof(k), v, sizeof(v), mode);
      if (mode == KV_INSERT)     check(exists && st == KV_EXISTS, "insert on a taken key must report exists");
      else if (exists)           check(st == KV_OK, "payer 0 on a taken key must write");
      else if (mode == KV_UPDATE) check(st == KV_NO_ROW, "update on a free key must report no_row");
   }

   // Bills the row to `payer`, so paymodes can check that payer 0 leaves it there.
   [[sysio::action]]
   void paysetup(name payer) {
      require_auth(payer);
      const char k[] = {'M'};
      check(put_row(tid_paymodes, payer.value, k, sizeof(k), "v", 1) == KV_OK, "paysetup must store the row");
   }

   [[sysio::action]]
   void payclean() {
      const char k[] = {'M'};
      check(kv_erase(tid_paymodes, k, sizeof(k)) == KV_OK, "payclean must erase the row");
   }

   // ==========================================================================
   // J. Read offsets and short buffers.
   // ==========================================================================

   [[sysio::action]]
   void getoffs() {
      const uint64_t self  = get_self().value;
      const char     k[]   = {'K'};
      const char     v[]   = {'a', 'b', 'c', 'd', 'e', 'f'};
      const int32_t  full  = sizeof(v);
      check(put_row(tid_reads, self, k, sizeof(k), v, sizeof(v)) == KV_OK, "store");

      char buf[3];
      auto reset = [&] { memset(buf, 'z', sizeof(buf)); };

      reset();
      check(kv_get(tid_reads, self, k, sizeof(k), 0, buf, 2) == full, "offset 0: full size");
      check(memcmp(buf, "abz", 3) == 0, "offset 0: short buffer truncates and writes nothing past it");

      reset();
      check(kv_get(tid_reads, self, k, sizeof(k), 3, buf, 2) == full, "middle: full size");
      check(memcmp(buf, "dez", 3) == 0, "middle: reads from the offset");

      reset();
      check(kv_get(tid_reads, self, k, sizeof(k), 5, buf, 2) == full, "near the end: full size");
      check(memcmp(buf, "fzz", 3) == 0, "near the end: copies only what is left");

      reset();
      check(kv_get(tid_reads, self, k, sizeof(k), full, buf, 2) == full, "at the end: full size");
      check(memcmp(buf, "zzz", 3) == 0, "at the end: copies nothing");

      reset();
      check(kv_get(tid_reads, self, k, sizeof(k), full + 100, buf, 2) == full, "past the end: full size");
      check(memcmp(buf, "zzz", 3) == 0, "past the end: copies nothing");

      check(kv_get(tid_reads, self, k, sizeof(k), 0xFFFFFFFFu, buf, 2) == full, "highest offset: full size");
      check(kv_get(tid_reads, self, k, sizeof(k), 2, nullptr, 0) == full, "zero-size buffer: full size");
      check(kv_get(tid_reads, self, "nope", 4, 2, buf, 2) == -1, "a free key: -1 at any offset");

      // kv_it_read takes the same offset rules for the value, and always reads the key from byte 0.
      const int32_t h = it_create(tid_reads, self, k, sizeof(k));
      char     key[1] = {};
      uint32_t key_size = 0, value_size = 0;
      reset();
      check(kv_it_read(h, 3, key, sizeof(key), buf, 2, &key_size, &value_size) == KV_OK, "it_read at an offset");
      check(key[0] == 'K' && memcmp(buf, "dez", 3) == 0, "it_read: key from byte 0, value from the offset");
      check(key_size == 1 && value_size == sizeof(v), "it_read: full sizes");
      reset();
      check(kv_it_read(h, full, key, sizeof(key), buf, 2, &key_size, &value_size) == KV_OK, "it_read at the end");
      check(memcmp(buf, "zzz", 3) == 0 && value_size == sizeof(v), "it_read at the end: nothing copied, full size");
      reset();
      check(kv_it_read(h, full + 1, key, sizeof(key), buf, 2, &key_size, &value_size) == KV_OK, "it_read past the end");
      check(memcmp(buf, "zzz", 3) == 0 && value_size == sizeof(v), "it_read past the end: nothing copied, full size");
      kv_it_destroy(h);

      check(kv_erase(tid_reads, k, sizeof(k)) == KV_OK, "cleanup");
   }

   // ==========================================================================
   // K. Moves read the row they land on, in the same call.
   // ==========================================================================

   [[sysio::action]]
   void itmoveread() {
      const uint64_t self = get_self().value;
      const char k1[] = {'p', '1'};
      const char k2[] = {'p', '2'};
      const char k3[] = {'p', '3'};
      check(put_row(tid_moves, self, k1, sizeof(k1), "one", 3) == KV_OK, "store 1");
      check(put_row(tid_moves, self, k2, sizeof(k2), "twotwo", 6) == KV_OK, "store 2");
      check(put_row(tid_moves, self, k3, sizeof(k3), "three", 5) == KV_OK, "store 3");

      char     key[4], val[8];
      uint32_t key_size = 99, val_size = 99;
      auto     reset = [&] { memset(key, 'z', sizeof(key)); memset(val, 'z', sizeof(val)); key_size = val_size = 99; };

      const int32_t h = kv_it_create(tid_moves, self, "p", 1, nullptr, 0);

      reset();
      check(kv_it_next(h, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK, "next lands on p2");
      check(memcmp(key, k2, 2) == 0 && key[2] == 'z' && key_size == 2, "next fills the key");
      check(memcmp(val, "twotwo", 6) == 0 && val[6] == 'z' && val_size == 6, "next fills the value");

      reset();
      check(kv_it_prev(h, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK, "prev lands on p1");
      check(memcmp(key, k1, 2) == 0 && key_size == 2, "prev fills the key");
      check(memcmp(val, "one", 3) == 0 && val[3] == 'z' && val_size == 3, "prev fills the value");

      reset();
      check(kv_it_lower_bound(h, k3, sizeof(k3), key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK,
            "lower_bound lands on p3");
      check(memcmp(key, k3, 2) == 0 && key_size == 2, "lower_bound fills the key");
      check(memcmp(val, "three", 5) == 0 && val[5] == 'z' && val_size == 5, "lower_bound fills the value");

      // A short buffer truncates and reports the full size. A zero-size one skips the copy and still reports it.
      reset();
      check(kv_it_prev(h, key, 1, val, 2, &key_size, &val_size) == KV_OK, "short buffers");
      check(key[0] == 'p' && key[1] == 'z' && key_size == 2, "key truncates, full size");
      check(memcmp(val, "tw", 2) == 0 && val[2] == 'z' && val_size == 6, "value truncates, full size");

      reset();
      check(kv_it_next(h, nullptr, 0, nullptr, 0, &key_size, &val_size) == KV_OK, "zero-size buffers on p3");
      check(key_size == 2 && val_size == 5, "zero-size buffers still report full sizes");
      check(key[0] == 'z' && val[0] == 'z', "zero-size buffers copy nothing");

      // Past the last row: end, sizes 0, nothing copied.
      reset();
      check(kv_it_next(h, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_END, "next past the last row");
      check(key_size == 0 && val_size == 0 && key[0] == 'z' && val[0] == 'z', "end reads nothing");

      // From end, prev reads the last row. Before the first row is end again.
      reset();
      check(kv_it_prev(h, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK, "prev from end");
      check(memcmp(key, k3, 2) == 0 && val_size == 5, "prev from end reads the last row");
      reset();
      check(kv_it_lower_bound(h, "q", 1, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_END,
            "lower_bound above the prefix");
      check(key_size == 0 && val_size == 0 && key[0] == 'z', "lower_bound at end reads nothing");
      kv_it_destroy(h);

      // kv_it_create seeks without reading, and a seek below the prefix starts at the prefix.
      int32_t c = kv_it_create(tid_moves, self, "p", 1, k2, sizeof(k2));
      check(it_key(c, key, sizeof(key), &key_size) == KV_OK && memcmp(key, k2, 2) == 0, "create seeks to p2");
      kv_it_destroy(c);
      c = kv_it_create(tid_moves, self, "p", 1, "a", 1);
      check(it_key(c, key, sizeof(key), &key_size) == KV_OK && memcmp(key, k1, 2) == 0, "seek below clamps to the prefix");
      kv_it_destroy(c);
      c = kv_it_create(tid_moves, self, "p", 1, "q", 1);
      check(it_status(c) == KV_END, "seek above the prefix is end");
      check(it_prev(c) == KV_OK && it_key(c, key, sizeof(key), &key_size) == KV_OK && memcmp(key, k3, 2) == 0,
            "prev from that end reaches the last row");
      kv_it_destroy(c);
   }

   [[sysio::action]]
   void idxmoveread() {
      const uint64_t self = get_self().value;
      check(kv_idx_store(tid_moves_sec, self, "p1", 2, "sa", 2) == KV_OK, "store a");
      check(kv_idx_store(tid_moves_sec, self, "p2", 2, "sbb", 3) == KV_OK, "store b");
      check(kv_idx_store(tid_moves_sec, self, "p3", 2, "scc", 3) == KV_OK, "store c");

      char     sec[8], pri[8];
      uint32_t sec_size = 99, pri_size = 99;
      auto     reset = [&] { memset(sec, 'z', sizeof(sec)); memset(pri, 'z', sizeof(pri)); sec_size = pri_size = 99; };

      const int32_t h = kv_idx_create(tid_moves_sec, self, nullptr, 0, nullptr, 0);

      reset();
      check(kv_idx_next(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "next lands on sbb");
      check(memcmp(sec, "sbb", 3) == 0 && sec[3] == 'z' && sec_size == 3, "next fills the secondary key");
      check(memcmp(pri, "p2", 2) == 0 && pri[2] == 'z' && pri_size == 2, "next fills the primary key");

      reset();
      check(kv_idx_prev(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "prev lands on sa");
      check(memcmp(sec, "sa", 2) == 0 && sec_size == 2 && memcmp(pri, "p1", 2) == 0 && pri_size == 2, "prev fills both");

      reset();
      check(kv_idx_lower_bound(h, "sc", 2, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK,
            "lower_bound lands on scc");
      check(memcmp(sec, "scc", 3) == 0 && sec_size == 3 && memcmp(pri, "p3", 2) == 0, "lower_bound fills both");

      reset();
      check(kv_idx_read(h, sec, 1, pri, 1, &sec_size, &pri_size) == KV_OK, "read with short buffers");
      check(sec[0] == 's' && sec[1] == 'z' && sec_size == 3, "secondary key truncates, full size");
      check(pri[0] == 'p' && pri[1] == 'z' && pri_size == 2, "primary key truncates, full size");

      reset();
      check(kv_idx_prev(h, nullptr, 0, nullptr, 0, &sec_size, &pri_size) == KV_OK, "zero-size buffers");
      check(sec_size == 3 && pri_size == 2, "zero-size buffers still report full sizes");

      reset();
      check(kv_idx_lower_bound(h, "t", 1, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_END,
            "lower_bound past the last entry");
      check(sec_size == 0 && pri_size == 0 && sec[0] == 'z', "end reads nothing");
      kv_idx_destroy(h);
   }

   // ==========================================================================
   // L. A secondary prefix bounds the iterator.
   // ==========================================================================

   [[sysio::action]]
   void idxprefix() {
      const uint64_t self = get_self().value;
      check(kv_idx_store(tid_prefix_sec, self, "p0", 2, "a9", 2) == KV_OK, "below the prefix");
      check(kv_idx_store(tid_prefix_sec, self, "p1", 2, "b1", 2) == KV_OK, "first in the prefix");
      check(kv_idx_store(tid_prefix_sec, self, "p2", 2, "b2", 2) == KV_OK, "last in the prefix");
      check(kv_idx_store(tid_prefix_sec, self, "p3", 2, "c1", 2) == KV_OK, "above the prefix");

      char     sec[2];
      uint32_t sec_size = 0;

      // Stepping past either edge reaches end, though entries lie just outside.
      int32_t h = kv_idx_create(tid_prefix_sec, self, "b", 1, nullptr, 0);
      check(idx_key(h, sec, sizeof(sec), &sec_size) == KV_OK && memcmp(sec, "b1", 2) == 0, "starts in the prefix");
      check(idx_next(h) == KV_OK, "second entry in the prefix");
      check(idx_next(h) == KV_END, "stepping past the top of the prefix is end");
      check(idx_prev(h) == KV_OK, "prev from end comes back in");
      check(idx_key(h, sec, sizeof(sec), &sec_size) == KV_OK && memcmp(sec, "b2", 2) == 0, "at the last in the prefix");
      check(idx_prev(h) == KV_OK && idx_prev(h) == KV_END, "stepping below the prefix is end");
      kv_idx_destroy(h);

      // A seek below the prefix starts at the prefix, and one above it is end.
      h = kv_idx_create(tid_prefix_sec, self, "b", 1, "a", 1);
      check(idx_key(h, sec, sizeof(sec), &sec_size) == KV_OK && memcmp(sec, "b1", 2) == 0, "seek below clamps up");
      uint32_t pri_size = 0;
      check(kv_idx_lower_bound(h, "c", 1, nullptr, 0, nullptr, 0, &sec_size, &pri_size) == KV_END, "seek above is end");
      check(kv_idx_lower_bound(h, "a", 1, sec, sizeof(sec), nullptr, 0, &sec_size, &pri_size) == KV_OK &&
               memcmp(sec, "b1", 2) == 0,
            "re-seek below clamps up too");
      kv_idx_destroy(h);

      // A prefix with no entries, and an all-0xFF prefix, which has no byte successor to bound it from above.
      h = kv_idx_create(tid_prefix_sec, self, "z", 1, nullptr, 0);
      check(idx_next(h) == KV_END && idx_prev(h) == KV_END, "an empty prefix has nothing to reach");
      kv_idx_destroy(h);
      const char ff[] = {char(0xFF), char(0xFF)};
      check(kv_idx_store(tid_prefix_sec, self, "p4", 2, ff, sizeof(ff)) == KV_OK, "an all-0xFF entry");
      h = kv_idx_create(tid_prefix_sec, self, ff, sizeof(ff), nullptr, 0);
      check(idx_next(h) == KV_END, "only entry in the all-0xFF prefix");
      check(idx_prev(h) == KV_OK, "prev from end finds it with no successor to seek");
      kv_idx_destroy(h);

      // kv_idx_find bounds its handle by the prefix.
      h = kv_idx_find(tid_prefix_sec, self, "b", 1, "b2", 2);
      check(h >= 0, "find inside the prefix");
      check(idx_next(h) == KV_END, "a found handle stops at the top of the prefix");
      kv_idx_destroy(h);
      h = kv_idx_find(tid_prefix_sec, self, "b", 1, "b1", 2);
      check(idx_prev(h) == KV_END, "a found handle stops at the bottom of the prefix");
      kv_idx_destroy(h);

      check(kv_idx_erase(tid_prefix_sec, "p0", 2, "a9", 2) == KV_OK, "cleanup");
      check(kv_idx_erase(tid_prefix_sec, "p1", 2, "b1", 2) == KV_OK, "cleanup");
      check(kv_idx_erase(tid_prefix_sec, "p2", 2, "b2", 2) == KV_OK, "cleanup");
      check(kv_idx_erase(tid_prefix_sec, "p3", 2, "c1", 2) == KV_OK, "cleanup");
      check(kv_idx_erase(tid_prefix_sec, "p4", 2, ff, sizeof(ff)) == KV_OK, "cleanup");
   }

   // ==========================================================================
   // M. Position is by key, through erase and re-insert.
   // ==========================================================================

   [[sysio::action]]
   void itrekey() {
      const uint64_t self = get_self().value;
      const char k1[] = {'r', '1'};
      const char k2[] = {'r', '2'};
      const char k3[] = {'r', '3'};
      check(put_row(tid_rekey, self, k1, sizeof(k1), "a", 1) == KV_OK, "store 1");
      check(put_row(tid_rekey, self, k2, sizeof(k2), "b", 1) == KV_OK, "store 2");

      char     key[4], val[8];
      uint32_t key_size = 0, val_size = 0;

      const int32_t h = it_create(tid_rekey, self, "r", 1);
      check(it_next(h) == KV_OK, "on r2");
      check(kv_erase(tid_rekey, k2, sizeof(k2)) == KV_OK, "erase r2");
      check(it_status(h) == KV_NO_ROW, "no row at the current key");

      // A new row elsewhere must not read through the erased row's memory.
      check(put_row(tid_rekey, self, k3, sizeof(k3), "c", 1) == KV_OK, "store r3 in the freed node");
      check(it_status(h) == KV_NO_ROW, "still no row at the current key");

      check(put_row(tid_rekey, self, k2, sizeof(k2), "newer", 5) == KV_OK, "re-insert r2");
      check(kv_it_read(h, 0, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK, "reads the new row");
      check(memcmp(key, k2, 2) == 0 && memcmp(val, "newer", 5) == 0 && val_size == 5, "the new row's bytes");

      // Erased again, a move goes strictly past the key in both directions, wherever the row went.
      check(kv_erase(tid_rekey, k2, sizeof(k2)) == KV_OK, "erase r2 again");
      check(put_row(tid_rekey, self, k2, sizeof(k2), "again", 5) == KV_OK, "store r2 again");
      check(kv_it_next(h, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK, "next from r2");
      check(memcmp(key, k3, 2) == 0, "next must not land on the re-inserted r2");
      check(it_prev(h) == KV_OK, "back to r2");
      check(kv_erase(tid_rekey, k2, sizeof(k2)) == KV_OK, "erase r2 a third time");
      check(put_row(tid_rekey, self, k2, sizeof(k2), "still", 5) == KV_OK, "store r2 a third time");
      check(kv_it_prev(h, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK, "prev from r2");
      check(memcmp(key, k1, 2) == 0, "prev must not land on the re-inserted r2");

      // A write that keeps the key keeps the position, and the read sees the new value.
      check(put_row(tid_rekey, self, k1, sizeof(k1), "grown value", 11) == KV_OK, "update r1 under the iterator");
      check(kv_it_read(h, 0, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK, "read after update");
      check(val_size == 11 && memcmp(val, "grown va", 8) == 0, "the updated value");
      kv_it_destroy(h);
   }

   [[sysio::action]]
   void idxrekey() {
      const uint64_t self = get_self().value;
      check(kv_idx_store(tid_rekey_sec, self, "p1", 2, "s1", 2) == KV_OK, "store 1");
      check(kv_idx_store(tid_rekey_sec, self, "p2", 2, "s2", 2) == KV_OK, "store 2");

      char     sec[4], pri[4];
      uint32_t sec_size = 0, pri_size = 0;

      const int32_t h = idx_find(self, tid_rekey_sec, "s1", 2);
      check(h >= 0, "on s1");
      check(kv_idx_erase(tid_rekey_sec, "p1", 2, "s1", 2) == KV_OK, "erase s1");
      check(kv_idx_read(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_NO_ROW, "no entry here");
      check(sec_size == 0 && pri_size == 0, "no_row reports no sizes");

      check(kv_idx_store(tid_rekey_sec, self, "p3", 2, "s3", 2) == KV_OK, "store s3 in the freed node");
      check(kv_idx_read(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_NO_ROW, "still no entry here");

      check(kv_idx_store(tid_rekey_sec, self, "p1", 2, "s1", 2) == KV_OK, "re-insert s1");
      check(kv_idx_read(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "reads the new entry");
      check(memcmp(sec, "s1", 2) == 0 && memcmp(pri, "p1", 2) == 0, "the new entry's bytes");

      // An update moves the entry away from the position, which stays by key.
      check(kv_idx_update(tid_rekey_sec, self, "p1", 2, "s1", 2, "s9", 2) == KV_OK, "update s1 to s9");
      check(kv_idx_read(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_NO_ROW, "the entry moved away");
      check(kv_idx_next(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "next from s1");
      check(memcmp(sec, "s2", 2) == 0, "next goes to s2, not to where the entry moved");

      // A failed update leaves other iterators on the entry where they were.
      check(kv_idx_store(tid_rekey_sec, self, "p2", 2, "s4", 2) == KV_OK, "store s4 under p2");
      check(kv_idx_update(tid_rekey_sec, self, "p2", 2, "s2", 2, "s4", 2) == KV_EXISTS, "s4 is taken");
      check(kv_idx_read(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "still on s2");
      check(memcmp(sec, "s2", 2) == 0 && memcmp(pri, "p2", 2) == 0, "s2 is unchanged");
      kv_idx_destroy(h);
   }

   /// Stores a row of the given sizes, for the test that RAM prices do not depend on where a key's bytes live.
   [[sysio::action]]
   void sizedrow(uint32_t key_size, uint32_t value_size) {
      const std::vector<char> key(key_size, 'k');
      const std::vector<char> value(value_size, 'v');
      must(put_row(tid_sized, get_self().value, key.data(), key_size, value.data(), value_size));
   }

   /// Erases the row sizedrow stored.
   [[sysio::action]]
   void sizedrowrm(uint32_t key_size) {
      const std::vector<char> key(key_size, 'k');
      must(kv_erase(tid_sized, key.data(), key_size));
   }

   /// Stores an index entry of the given sizes.
   [[sysio::action]]
   void sizedidx(uint32_t sec_size, uint32_t pri_size) {
      const std::vector<char> sec(sec_size, 's');
      const std::vector<char> pri(pri_size, 'p');
      must(kv_idx_store(tid_sized_sec, get_self().value, pri.data(), pri_size, sec.data(), sec_size));
   }

   /// Moves the entry sizedidx stored to a secondary key of `new_sec_size` bytes.
   [[sysio::action]]
   void sizedidxup(uint32_t sec_size, uint32_t pri_size, uint32_t new_sec_size) {
      const std::vector<char> sec(sec_size, 's');
      const std::vector<char> pri(pri_size, 'p');
      const std::vector<char> new_sec(new_sec_size, 's');
      must(kv_idx_update(tid_sized_sec, get_self().value, pri.data(), pri_size, sec.data(), sec_size, new_sec.data(),
                         new_sec_size));
   }

   /// Rewrites the entry sizedidx stored with a new payer, at the same position.
   [[sysio::action]]
   void sizedidxpay(uint32_t sec_size, uint32_t pri_size, name payer) {
      const std::vector<char> sec(sec_size, 's');
      const std::vector<char> pri(pri_size, 'p');
      must(kv_idx_update(tid_sized_sec, payer.value, pri.data(), pri_size, sec.data(), sec_size, sec.data(), sec_size));
   }

   /// Runs one of the eight reads that return two sizes, with the size pointers at these addresses.
   [[sysio::action]]
   void sizeptrs(uint32_t read, uint32_t first, uint32_t second) {
      const uint64_t self = get_self().value;
      const char key[] = {'k', 'e', 'y'};
      const char val[] = {'v', 'a', 'l', 'u', 'e'};
      const char sec[] = {'s', 'e', 'c', 'o', 'n', 'd'};
      must(put_row(tid_sizeptrs, self, key, sizeof(key), val, sizeof(val)));
      must(kv_idx_store(tid_sizeptrs_sec, self, key, sizeof(key), sec, sizeof(sec)));
      const int32_t row   = it_create(tid_sizeptrs, self, nullptr, 0);
      const int32_t entry = kv_idx_create(tid_sizeptrs_sec, self, nullptr, 0, nullptr, 0);
      auto* first_size  = reinterpret_cast<uint32_t*>(first);
      auto* second_size = reinterpret_cast<uint32_t*>(second);
      switch (read) {
      case 0: kv_it_next(row, nullptr, 0, nullptr, 0, first_size, second_size); break;
      case 1: kv_it_prev(row, nullptr, 0, nullptr, 0, first_size, second_size); break;
      case 2: kv_it_lower_bound(row, nullptr, 0, nullptr, 0, nullptr, 0, first_size, second_size); break;
      case 3: kv_it_read(row, 0, nullptr, 0, nullptr, 0, first_size, second_size); break;
      case 4: kv_idx_next(entry, nullptr, 0, nullptr, 0, first_size, second_size); break;
      case 5: kv_idx_prev(entry, nullptr, 0, nullptr, 0, first_size, second_size); break;
      case 6: kv_idx_lower_bound(entry, nullptr, 0, nullptr, 0, nullptr, 0, first_size, second_size); break;
      default: kv_idx_read(entry, nullptr, 0, nullptr, 0, first_size, second_size); break;
      }
      // The reads that land on a row: two sizes that do not overlap arrive intact wherever they sit.
      if (read == 2 || read == 3 || read == 6 || read == 7) {
         uint32_t first_value  = 0;
         uint32_t second_value = 0;
         std::memcpy(&first_value, first_size, sizeof(first_value));
         std::memcpy(&second_value, second_size, sizeof(second_value));
         check(first_value == (read < 4 ? sizeof(key) : sizeof(sec)), "first size must land intact");
         check(second_value == (read < 4 ? sizeof(val) : sizeof(key)), "second size must land intact");
      }
   }

   /// Removes what sizeptrs stored.
   [[sysio::action]]
   void clearsizes() {
      const char key[] = {'k', 'e', 'y'};
      const char sec[] = {'s', 'e', 'c', 'o', 'n', 'd'};
      must(kv_erase(tid_sizeptrs, key, sizeof(key)));
      must(kv_idx_erase(tid_sizeptrs_sec, key, sizeof(key), sec, sizeof(sec)));
   }

   /// One write with a payer the test names: a new row, a row given to it, a new entry, or an entry given to it.
   [[sysio::action]]
   void payerwrite(uint32_t write, name payer) {
      const uint64_t self = get_self().value;
      const char key[] = {'k'};
      switch (write) {
      case 0:
         put_row(tid_payerw, payer.value, key, sizeof(key), "v", 1);
         break;
      case 1:
         must(put_row(tid_payerw, self, key, sizeof(key), "v", 1));
         put_row(tid_payerw, payer.value, key, sizeof(key), "v", 1);
         break;
      case 2:
         kv_idx_store(tid_payerw_sec, payer.value, key, sizeof(key), "s", 1);
         break;
      default:
         must(kv_idx_store(tid_payerw_sec, self, key, sizeof(key), "s", 1));
         kv_idx_update(tid_payerw_sec, payer.value, key, sizeof(key), "s", 1, "s", 1);
         break;
      }
   }

   /// An update that keeps the key leaves an iterator on the entry, reading and moving as before.
   [[sysio::action]]
   void idxsamekey() {
      const uint64_t self = get_self().value;
      idx_put(tid_samekey_sec, self, "s1", "p1");
      idx_put(tid_samekey_sec, self, "s2", "p2");
      const int32_t h = kv_idx_create(tid_samekey_sec, self, nullptr, 0, nullptr, 0);
      must(kv_idx_update(tid_samekey_sec, self, "p1", 2, "s1", 2, "s1", 2));

      char     sec[2], pri[2];
      uint32_t sec_size = 0, pri_size = 0;
      check(kv_idx_read(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "still on s1");
      check(memcmp(sec, "s1", 2) == 0 && memcmp(pri, "p1", 2) == 0, "reads s1");
      check(kv_idx_next(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "next finds s2");
      check(memcmp(sec, "s2", 2) == 0 && memcmp(pri, "p2", 2) == 0, "reads s2");
      kv_idx_destroy(h);
   }

   /// Erases the entry at those sizes.
   [[sysio::action]]
   void sizedidxrm(uint32_t sec_size, uint32_t pri_size) {
      const std::vector<char> sec(sec_size, 's');
      const std::vector<char> pri(pri_size, 'p');
      must(kv_idx_erase(tid_sized_sec, pri.data(), pri_size, sec.data(), sec_size));
   }

   // ==========================================================================
   // N. kv_it_prev with nothing to reach, with no byte successor, and after an erase.
   // ==========================================================================

   // From end with no row in the prefix. The neighbors are one index step away, so landing on one must still be end.
   [[sysio::action]]
   void prvempty() {
      const uint64_t self = get_self().value;
      must(put_row(tid_prev_low, self, "z", 1, "v", 1));
      must(put_row(tid_prev_high, self, "a", 1, "v", 1));

      int32_t h = it_create(tid_prev_empty, self, nullptr, 0);
      check(it_status(h) == KV_END, "an empty table starts at end");
      check(it_prev(h) == KV_END, "empty prefix: prev must not step into the table below");
      check(it_status(h) == KV_END, "a prev that finds nothing stays at end");
      kv_it_destroy(h);

      h = it_create(tid_prev_empty, self, "m", 1);
      check(it_prev(h) == KV_END, "prefix in an empty table: prev must not step into the table below");
      kv_it_destroy(h);

      // Rows on both sides of the prefix, in the same table.
      must(put_row(tid_prev_rows, self, "a", 1, "v", 1));
      must(put_row(tid_prev_rows, self, "z", 1, "v", 1));
      h = it_create(tid_prev_rows, self, "m", 1);
      check(it_status(h) == KV_END, "no row in the prefix");
      check(it_prev(h) == KV_END, "prev must not land on the row below the prefix");
      kv_it_destroy(h);

      // Rows only below the prefix.
      h = it_create(tid_prev_rows, self, "zz", 2);
      check(it_prev(h) == KV_END, "prev must not land on the last row of the table, which is below the prefix");
      kv_it_destroy(h);

      must(kv_erase(tid_prev_low, "z", 1));
      must(kv_erase(tid_prev_high, "a", 1));
      must(kv_erase(tid_prev_rows, "a", 1));
      must(kv_erase(tid_prev_rows, "z", 1));
   }

   // A prefix with no byte successor cannot bound a reverse scan from above, so the scan starts at the end of the
   // table. An empty prefix takes the same branch, and every unscoped kv::table reverse scan uses it.
   [[sysio::action]]
   void prvnosucc() {
      const uint64_t self = get_self().value;
      const char low[] = {0x10};
      const char one[] = {char(0xFF), 0x01};
      const char two[] = {char(0xFF), char(0xFF)};
      const char six[] = {char(0xFF), char(0xFF), 0x01};
      const char top[] = {char(0xFF), char(0xFF), char(0xFF), char(0xFF)};
      must(put_row(tid_prev_ff, self, low, sizeof(low), "v", 1));
      must(put_row(tid_prev_ff, self, one, sizeof(one), "v", 1));
      must(put_row(tid_prev_ff, self, two, sizeof(two), "v", 1));
      must(put_row(tid_prev_ff, self, six, sizeof(six), "v", 1));
      must(put_row(tid_prev_ff_up, self, low, sizeof(low), "v", 1));

      // An empty prefix covers the table and stops there, with a table above it to overrun into.
      int32_t h = kv_it_create(tid_prev_ff, self, nullptr, 0, top, sizeof(top));
      check(it_status(h) == KV_END, "a seek above every key is end");
      prev_onto(h, six, sizeof(six), "empty prefix: first prev is the last row of the table");
      prev_onto(h, two, sizeof(two), "empty prefix: second row");
      prev_onto(h, one, sizeof(one), "empty prefix: third row");
      prev_onto(h, low, sizeof(low), "empty prefix: fourth row");
      check(it_prev(h) == KV_END, "empty prefix: prev from the first row is end");
      kv_it_destroy(h);

      // All-0xFF prefixes of two lengths. The row at FF 01 lies below the longer one.
      h = kv_it_create(tid_prev_ff, self, two, sizeof(two), top, sizeof(top));
      check(it_status(h) == KV_END, "FF FF prefix: above every key is end");
      prev_onto(h, six, sizeof(six), "FF FF prefix: last row");
      prev_onto(h, two, sizeof(two), "FF FF prefix: first row");
      check(it_prev(h) == KV_END, "FF FF prefix: the row below it is out of reach");
      kv_it_destroy(h);

      h = kv_it_create(tid_prev_ff, self, two, 1, top, sizeof(top));
      prev_onto(h, six, sizeof(six), "FF prefix: last row");
      prev_onto(h, two, sizeof(two), "FF prefix: second row");
      prev_onto(h, one, sizeof(one), "FF prefix: first row");
      check(it_prev(h) == KV_END, "FF prefix: the row below it is out of reach");
      kv_it_destroy(h);

      // A trailing 0xFF is dropped and the byte before it incremented, so the bound is the shorter key 0x62.
      const char pre[] = {0x61, char(0xFF)};
      const char in1[] = {0x61, char(0xFF), 0x01};
      const char in2[] = {0x61, char(0xFF), 0x02};
      const char out[] = {0x62};
      must(put_row(tid_prev_succ, self, in1, sizeof(in1), "v", 1));
      must(put_row(tid_prev_succ, self, in2, sizeof(in2), "v", 1));
      must(put_row(tid_prev_succ, self, out, sizeof(out), "v", 1));
      h = kv_it_create(tid_prev_succ, self, pre, sizeof(pre), out, sizeof(out));
      check(it_status(h) == KV_END, "a seek at the bound is end");
      prev_onto(h, in2, sizeof(in2), "trailing FF: last row in the prefix");
      prev_onto(h, in1, sizeof(in1), "trailing FF: first row in the prefix");
      check(it_prev(h) == KV_END, "trailing FF: nothing below the prefix");
      kv_it_destroy(h);

      // Key bytes compare unsigned, so 0x80 sorts above the prefix 0x7F and is its successor.
      const char hi_pre[] = {0x7F};
      const char hi_in[]  = {0x7F, 0x01};
      const char hi_out[] = {char(0x80)};
      must(put_row(tid_prev_succ, self, hi_in, sizeof(hi_in), "v", 1));
      must(put_row(tid_prev_succ, self, hi_out, sizeof(hi_out), "v", 1));
      h = kv_it_create(tid_prev_succ, self, hi_pre, sizeof(hi_pre), hi_out, sizeof(hi_out));
      check(it_status(h) == KV_END, "0x80 is above the prefix 0x7F");
      prev_onto(h, hi_in, sizeof(hi_in), "high bit: last row in the prefix");
      check(it_prev(h) == KV_END, "high bit: nothing else in the prefix");
      kv_it_destroy(h);

      must(kv_erase(tid_prev_ff, low, sizeof(low)));
      must(kv_erase(tid_prev_ff, one, sizeof(one)));
      must(kv_erase(tid_prev_ff, two, sizeof(two)));
      must(kv_erase(tid_prev_ff, six, sizeof(six)));
      must(kv_erase(tid_prev_ff_up, low, sizeof(low)));
      must(kv_erase(tid_prev_succ, in1, sizeof(in1)));
      must(kv_erase(tid_prev_succ, in2, sizeof(in2)));
      must(kv_erase(tid_prev_succ, out, sizeof(out)));
      must(kv_erase(tid_prev_succ, hi_in, sizeof(hi_in)));
      must(kv_erase(tid_prev_succ, hi_out, sizeof(hi_out)));
   }

   // tstpreveras in test_kv_api covers erasing a row with a neighbor below it in the prefix. These are the other cases.
   [[sysio::action]]
   void prverased() {
      const uint64_t self = get_self().value;
      must(put_row(tid_prev_erase, self, "a", 1, "v", 1));
      must(put_row(tid_prev_erase, self, "p1", 2, "v", 1));
      must(put_row(tid_prev_erase, self, "p2", 2, "v", 1));
      must(put_row(tid_prev_erase, self, "z", 1, "v", 1));

      // The first row in the prefix: the row before it is outside.
      int32_t h = it_create(tid_prev_erase, self, "p", 1);
      must(kv_erase(tid_prev_erase, "p1", 2));
      check(it_prev(h) == KV_END, "prev after erasing the first row in the prefix is end");
      kv_it_destroy(h);

      // The only row left in the prefix.
      h = it_create(tid_prev_erase, self, "p", 1);
      must(kv_erase(tid_prev_erase, "p2", 2));
      check(it_prev(h) == KV_END, "prev after erasing the only row in the prefix is end");
      kv_it_destroy(h);

      // The last row of the table: the position is above everything left in it.
      h = kv_it_create(tid_prev_erase, self, nullptr, 0, "z", 1);
      must(kv_erase(tid_prev_erase, "z", 1));
      prev_onto(h, "a", 1, "prev after erasing the last row lands on the row before it");
      kv_it_destroy(h);

      // The only row of the table.
      h = it_create(tid_prev_erase, self, nullptr, 0);
      must(kv_erase(tid_prev_erase, "a", 1));
      check(it_prev(h) == KV_END, "prev after erasing the only row of the table is end");
      kv_it_destroy(h);
   }

   // firstseed and firstrun belong to the account whose rows sort first in the whole index. No row lies before the
   // first one, so kv_it_prev and kv_idx_prev need a branch of their own there. The host test checks the seed is in
   // that spot.
   [[sysio::action]]
   void firstseed() {
      const uint64_t self = get_self().value;
      must(put_row(tid_first, self, "bb", 2, "v", 1));
      must(put_row(tid_first, self, "cc", 2, "v", 1));
      idx_put(tid_first, self, "bb", "p1");
      idx_put(tid_first, self, "cc", "p2");
   }

   [[sysio::action]]
   void firstrun() {
      const uint64_t self = get_self().value;

      // On the first row of the index.
      int32_t h = it_create(tid_first, self, nullptr, 0);
      check(it_prev(h) == KV_END, "prev from the first row of the index is end");
      check(it_status(h) == KV_END, "and it stays at end");
      prev_onto(h, "cc", 2, "prev from that end reaches the last row");
      kv_it_destroy(h);

      // From end, with no row below the prefix's bound.
      h = it_create(tid_first, self, "a", 1);
      check(it_status(h) == KV_END, "nothing in the prefix");
      check(it_prev(h) == KV_END, "from end, with no row in the index before the bound");
      kv_it_destroy(h);

      // On a row that was erased, so the step works from the key bytes.
      h = it_create(tid_first, self, nullptr, 0);
      must(kv_erase(tid_first, "bb", 2));
      check(it_prev(h) == KV_END, "prev after erasing the first row of the index is end");
      kv_it_destroy(h);

      // The table is empty and no other table sorts below it.
      must(kv_erase(tid_first, "cc", 2));
      h = it_create(tid_first, self, nullptr, 0);
      check(it_prev(h) == KV_END, "an empty table at the very start of the index");
      kv_it_destroy(h);

      // The same three for the index entries.
      h = kv_idx_create(tid_first, self, nullptr, 0, nullptr, 0);
      check(idx_prev(h) == KV_END, "prev from the first entry of the index is end");
      idx_prev_onto(h, "cc", "p2", "prev from that end reaches the last entry");
      kv_idx_destroy(h);

      h = kv_idx_create(tid_first, self, "a", 1, nullptr, 0);
      check(idx_prev(h) == KV_END, "from end, with no entry in the index before the bound");
      kv_idx_destroy(h);

      h = kv_idx_create(tid_first, self, nullptr, 0, nullptr, 0);
      idx_del(tid_first, "bb", "p1");
      check(idx_prev(h) == KV_END, "prev after removing the first entry of the index is end");
      kv_idx_destroy(h);

      idx_del(tid_first, "cc", "p2");
      h = kv_idx_create(tid_first, self, nullptr, 0, nullptr, 0);
      check(idx_prev(h) == KV_END, "an empty table at the very start of the index");
      kv_idx_destroy(h);
   }

   // For a chain with no KV rows at all, where begin and end of the index are the same place.
   [[sysio::action]]
   void emptyindex() {
      const uint64_t self = get_self().value;
      int32_t h = it_create(tid_first, self, nullptr, 0);
      check(it_prev(h) == KV_END, "prev on an empty primary index");
      kv_it_destroy(h);
      h = kv_idx_create(tid_first, self, nullptr, 0, nullptr, 0);
      check(idx_prev(h) == KV_END, "prev on an empty secondary index");
      kv_idx_destroy(h);
      check(kv_idx_find(tid_first, self, nullptr, 0, "k", 1) == -1, "find on an empty secondary index");
   }

   // lastseed and lastrun are firstseed and firstrun for the other end: the account whose rows sort last in the whole
   // index, at the highest table_id, so a step off the last row and a seek past it both reach the end of chainbase's
   // index rather than a neighbor. The host test checks the seed is in that spot.
   [[sysio::action]]
   void lastseed() {
      const uint64_t self = get_self().value;
      must(put_row(tid_top, self, "bb", 2, "v", 1));
      must(put_row(tid_top, self, "cc", 2, "v", 1));
      idx_put(tid_top, self, "bb", "p1");
      idx_put(tid_top, self, "cc", "p2");
   }

   [[sysio::action]]
   void lastrun() {
      const uint64_t self = get_self().value;

      // Off the last row of the index.
      int32_t h = kv_it_create(tid_top, self, nullptr, 0, "cc", 2);
      check(it_next(h) == KV_END, "next off the last row of the index is end");
      check(it_next(h) == KV_END, "and stays there");
      prev_onto(h, "cc", 2, "prev from that end reaches the last row");
      kv_it_destroy(h);

      // From end with a prefix whose successor is past every row, and with no prefix at all.
      h = kv_it_create(tid_top, self, "c", 1, "d", 1);
      check(it_status(h) == KV_END, "a seek above the prefix is end");
      prev_onto(h, "cc", 2, "prev from end, successor past the index");
      kv_it_destroy(h);
      h = kv_it_create(tid_top, self, nullptr, 0, "zz", 2);
      check(it_status(h) == KV_END, "a seek above every row is end");
      prev_onto(h, "cc", 2, "prev from end, no prefix");
      kv_it_destroy(h);

      // A prefix above every row: prev finds a row, and the row is not in the prefix.
      h = it_create(tid_top, self, "d", 1);
      check(it_prev(h) == KV_END, "prev from end, nothing in the prefix at the end of the index");
      kv_it_destroy(h);

      // Off a row that was erased, the last one in the index, with the step working from the key bytes.
      h = kv_it_create(tid_top, self, nullptr, 0, "cc", 2);
      must(kv_erase(tid_top, "cc", 2));
      check(it_next(h) == KV_END, "next from the erased last row of the index is end");
      kv_it_destroy(h);
      must(put_row(tid_top, self, "cc", 2, "v", 1));

      // The same for the index entries.
      h = kv_idx_create(tid_top, self, nullptr, 0, "cc", 2);
      check(idx_next(h) == KV_END, "next off the last entry of the index is end");
      idx_prev_onto(h, "cc", "p2", "prev from that end reaches the last entry");
      kv_idx_destroy(h);

      h = kv_idx_create(tid_top, self, "c", 1, "d", 1);
      idx_prev_onto(h, "cc", "p2", "prev from end, successor past the index");
      kv_idx_destroy(h);
      h = kv_idx_create(tid_top, self, nullptr, 0, "zz", 2);
      idx_prev_onto(h, "cc", "p2", "prev from end, no prefix");
      kv_idx_destroy(h);
      h = kv_idx_create(tid_top, self, "d", 1, nullptr, 0);
      check(idx_prev(h) == KV_END, "prev from end, nothing in the prefix at the end of the index");
      kv_idx_destroy(h);

      // lower_bound runs off the end of the index, which kv_idx_find has to stop at.
      check(idx_find(self, tid_top, "zz", 2) == -1, "find above the last entry of the index");

      h = kv_idx_create(tid_top, self, nullptr, 0, "cc", 2);
      idx_del(tid_top, "cc", "p2");
      check(idx_next(h) == KV_END, "next from the removed last entry of the index is end");
      kv_idx_destroy(h);
      idx_put(tid_top, self, "cc", "p2");
   }

   // ==========================================================================
   // O. kv_idx_prev, and how entries sharing a secondary key order.
   // ==========================================================================

   [[sysio::action]]
   void ixprvedge() {
      const uint64_t self = get_self().value;
      idx_put(tid_ixprev_low, self, "s9", "p0"); // one index step below the table under test
      idx_put(tid_ixprev, self, "s1", "p1");
      idx_put(tid_ixprev, self, "s2", "p2");
      idx_put(tid_ixprev, self, "s3", "p3");

      // From the first entry of the table.
      int32_t  h        = kv_idx_create(tid_ixprev, self, nullptr, 0, nullptr, 0);
      uint32_t sec_size = 99, pri_size = 99;
      check(idx_prev(h) == KV_END, "prev from the first entry is end");
      check(kv_idx_read(h, nullptr, 0, nullptr, 0, &sec_size, &pri_size) == KV_END, "and it stays at end");
      idx_prev_onto(h, "s3", "p3", "prev from that end reaches the last entry");
      kv_idx_destroy(h);

      // After removing the entry under the iterator, in the middle and at the start.
      h = idx_find(self, tid_ixprev, "s2", IDX_KEY_SIZE);
      idx_del(tid_ixprev, "s2", "p2");
      idx_prev_onto(h, "s1", "p1", "prev after remove lands on the entry before");
      kv_idx_destroy(h);

      h = idx_find(self, tid_ixprev, "s1", IDX_KEY_SIZE);
      idx_del(tid_ixprev, "s1", "p1");
      check(idx_prev(h) == KV_END, "prev after removing the first entry is end");
      kv_idx_destroy(h);
      idx_put(tid_ixprev, self, "s1", "p1");
      idx_put(tid_ixprev, self, "s2", "p2");

      // An update moves the entry away from the position, which stays at the old key whichever way the entry went.
      h = idx_find(self, tid_ixprev, "s2", IDX_KEY_SIZE);
      idx_move(tid_ixprev, self, "p2", "s2", "s8");
      idx_prev_onto(h, "s1", "p1", "prev after an update upward starts from the old key");
      kv_idx_destroy(h);

      h = idx_find(self, tid_ixprev, "s8", IDX_KEY_SIZE);
      idx_move(tid_ixprev, self, "p2", "s8", "s0");
      idx_prev_onto(h, "s3", "p3", "prev after an update downward starts from the old key");
      kv_idx_destroy(h);

      // Remove and re-store of the same entry: prev goes strictly below the key.
      h = idx_find(self, tid_ixprev, "s3", IDX_KEY_SIZE);
      idx_del(tid_ixprev, "s3", "p3");
      idx_put(tid_ixprev, self, "s3", "p3");
      idx_prev_onto(h, "s1", "p1", "prev after re-store must not land on the entry again");
      kv_idx_destroy(h);

      // The same with a read in between, which puts the iterator back on the new entry.
      h = idx_find(self, tid_ixprev, "s3", IDX_KEY_SIZE);
      idx_del(tid_ixprev, "s3", "p3");
      idx_put(tid_ixprev, self, "s3", "p3");
      idx_on(h, "s3", "p3", "a read finds the re-stored entry");
      idx_prev_onto(h, "s1", "p1", "prev after that read steps from the new entry");
      kv_idx_destroy(h);

      idx_del(tid_ixprev, "s0", "p2");
      idx_del(tid_ixprev, "s1", "p1");
      idx_del(tid_ixprev, "s3", "p3");
      idx_del(tid_ixprev_low, "s9", "p0");
   }

   // Entries sharing a secondary key sort by primary key, whatever order they were stored in, and a removed entry's
   // position is the (sec_key, pri_key) pair.
   [[sysio::action]]
   void ixdupkeys() {
      const uint64_t self = get_self().value;
      idx_put(tid_dupkeys, self, "ss", "p3");
      idx_put(tid_dupkeys, self, "ss", "p1");
      idx_put(tid_dupkeys, self, "ss", "p2");

      int32_t h = idx_find(self, tid_dupkeys, "ss", IDX_KEY_SIZE);
      idx_on(h, "ss", "p1", "find lands on the lowest primary key");
      idx_next_onto(h, "ss", "p2", "next goes up the primary keys");
      idx_next_onto(h, "ss", "p3", "next reaches the highest primary key");
      check(idx_next(h) == KV_END, "and then end");
      idx_prev_onto(h, "ss", "p3", "prev from end reaches the highest primary key");
      idx_prev_onto(h, "ss", "p2", "prev goes down the primary keys");

      idx_del(tid_dupkeys, "ss", "p2");
      idx_prev_onto(h, "ss", "p1", "prev from a removed entry keeps to the pair");
      kv_idx_destroy(h);

      idx_put(tid_dupkeys, self, "ss", "p2");
      h = kv_idx_find(tid_dupkeys, self, nullptr, 0, "ss", IDX_KEY_SIZE);
      idx_next_onto(h, "ss", "p2", "on the re-stored entry");
      idx_del(tid_dupkeys, "ss", "p2");
      idx_next_onto(h, "ss", "p3", "next from a removed entry keeps to the pair");
      kv_idx_destroy(h);

      idx_del(tid_dupkeys, "ss", "p1");
      idx_del(tid_dupkeys, "ss", "p3");
   }

   // ==========================================================================
   // P. Seeks.
   // ==========================================================================

   [[sysio::action]]
   void itlbclamp() {
      const uint64_t self = get_self().value;
      must(put_row(tid_lbound, self, "a", 1, "v", 1));
      must(put_row(tid_lbound, self, "p1", 2, "v", 1));
      must(put_row(tid_lbound, self, "p2", 2, "v", 1));
      must(put_row(tid_lbound, self, "z", 1, "v", 1));

      char     key[4];
      uint32_t key_size = 99, val_size = 99;
      int32_t  h = it_create(tid_lbound, self, "p", 1);
      auto     seek = [&](const char* to, uint32_t to_size) {
         memset(key, 'x', sizeof(key));
         key_size = val_size = 99;
         return kv_it_lower_bound(h, to, to_size, key, sizeof(key), nullptr, 0, &key_size, &val_size);
      };

      check(seek("a", 1) == KV_OK, "a seek below the prefix clamps up");
      check(key_size == 2 && memcmp(key, "p1", 2) == 0, "and lands on the first row in the prefix");
      check(seek(nullptr, 0) == KV_OK, "an empty seek is the start of the prefix");
      check(key_size == 2 && memcmp(key, "p1", 2) == 0, "and lands on the first row in the prefix");
      check(seek("p", 1) == KV_OK, "a seek at the prefix itself");
      check(key_size == 2 && memcmp(key, "p1", 2) == 0, "and lands on the first row in the prefix");
      check(seek("p2", 2) == KV_OK, "an exact hit");
      check(key_size == 2 && memcmp(key, "p2", 2) == 0, "lands on that row");

      check(seek("q", 1) == KV_END, "a seek above the prefix is end, with a row above it to overrun onto");
      check(key_size == 0 && val_size == 0 && key[0] == 'x', "end reads nothing");
      check(seek("p21", 3) == KV_END, "a seek past the last row in the prefix is end");
      check(seek("a", 1) == KV_OK, "a seek from end comes back in");
      check(key_size == 2 && memcmp(key, "p1", 2) == 0, "and lands on the first row in the prefix");
      kv_it_destroy(h);

      // Seeks have no size limit: CDT positions at end with a key longer than any row's.
      static char over[MAX_PREFIX_LIMIT + 1];
      memset(over, 0xFF, sizeof(over));
      h = kv_it_create(tid_lbound, self, nullptr, 0, over, sizeof(over));
      check(it_status(h) == KV_END, "a seek above every key is end, at any length");
      prev_onto(h, "z", 1, "prev from that end reaches the last row");
      check(seek(over, sizeof(over)) == KV_END, "lower_bound takes the same seek");
      kv_it_destroy(h);
      idx_put(tid_lbound, self, "ss", "p1");
      h = kv_idx_create(tid_lbound, self, nullptr, 0, over, sizeof(over));
      idx_prev_onto(h, "ss", "p1", "a secondary seek above every key is end, at any length");
      kv_idx_destroy(h);

      idx_del(tid_lbound, "ss", "p1");
      must(kv_erase(tid_lbound, "a", 1));
      must(kv_erase(tid_lbound, "p1", 2));
      must(kv_erase(tid_lbound, "p2", 2));
      must(kv_erase(tid_lbound, "z", 1));
   }

   // ==========================================================================
   // Q. Read rules: offsets on kv_it_read, and what a read writes when it finds nothing.
   // ==========================================================================

   [[sysio::action]]
   void rdrules() {
      const uint64_t self = get_self().value;
      const char     k[]  = {'k', 'e', 'y'};
      must(put_row(tid_rdrules, self, k, sizeof(k), "abcdef", 6));

      char     key[4], val[8];
      uint32_t key_size = 0, val_size = 0;
      auto     reset = [&] {
         memset(key, 'z', sizeof(key));
         memset(val, 'z', sizeof(val));
         key_size = val_size = 99;
      };
      auto full_sizes = [&] { return key_size == sizeof(k) && val_size == 6; };

      const int32_t h = it_create(tid_rdrules, self, k, sizeof(k));

      reset();
      check(kv_it_read(h, 5, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK,
            "offset near the end");
      check(memcmp(key, "keyz", 4) == 0, "offset leaves the key alone");
      check(memcmp(val, "fzzzzzzz", 8) == 0, "copies only what is left");
      check(full_sizes(), "offset near the end: full sizes");

      reset();
      check(kv_it_read(h, 0xFFFFFFFFu, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK,
            "top offset");
      check(memcmp(key, "keyz", 4) == 0 && val[0] == 'z', "top offset copies no value");
      check(full_sizes(), "top offset: full sizes");

      // Each field has its own capacity.
      reset();
      check(kv_it_read(h, 2, nullptr, 0, val, 2, &key_size, &val_size) == KV_OK, "no key buffer");
      check(memcmp(val, "cdz", 3) == 0 && full_sizes(), "value at an offset, no key buffer");
      reset();
      check(kv_it_read(h, 2, key, 1, nullptr, 0, &key_size, &val_size) == KV_OK, "no value buffer");
      check(memcmp(key, "kz", 2) == 0 && full_sizes(), "short key, no value buffer");

      // A read that re-finds the row by key takes the offset too.
      must(kv_erase(tid_rdrules, k, sizeof(k)));
      must(put_row(tid_rdrules, self, k, sizeof(k), "ghijkl", 6));
      reset();
      check(kv_it_read(h, 4, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK,
            "re-found by key");
      check(memcmp(val, "klzz", 4) == 0 && full_sizes(), "re-found by key: value from the offset");

      // After erase: no_row, sizes 0 and nothing copied, for both fields.
      must(kv_erase(tid_rdrules, k, sizeof(k)));
      reset();
      check(kv_it_read(h, 0, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_NO_ROW, "erased");
      check(key_size == 0 && val_size == 0, "erased: sizes are written as 0");
      check(key[0] == 'z' && val[0] == 'z', "erased: nothing copied");
      kv_it_destroy(h);
   }

   [[sysio::action]]
   void ixrdrules() {
      const uint64_t self = get_self().value;
      idx_put(tid_rdrules_ix, self, "s1", "p1");

      char     sec[4], pri[4];
      uint32_t sec_size = 0, pri_size = 0;
      auto     reset = [&] {
         memset(sec, 'z', sizeof(sec));
         memset(pri, 'z', sizeof(pri));
         sec_size = pri_size = 99;
      };

      int32_t h = idx_find(self, tid_rdrules_ix, "s1", IDX_KEY_SIZE);
      reset();
      check(kv_idx_read(h, sec, 1, nullptr, 0, &sec_size, &pri_size) == KV_OK, "short secondary, no primary buffer");
      check(memcmp(sec, "sz", 2) == 0 && pri[0] == 'z', "each field has its own capacity");
      check(sec_size == IDX_KEY_SIZE && pri_size == IDX_KEY_SIZE, "full sizes");

      // At end.
      check(idx_next(h) == KV_END, "past the only entry");
      reset();
      check(kv_idx_read(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_END, "read at end");
      check(sec_size == 0 && pri_size == 0, "at end: sizes are written as 0");
      check(sec[0] == 'z' && pri[0] == 'z', "at end: nothing copied");
      kv_idx_destroy(h);

      // After removal.
      h = idx_find(self, tid_rdrules_ix, "s1", IDX_KEY_SIZE);
      idx_del(tid_rdrules_ix, "s1", "p1");
      reset();
      check(kv_idx_read(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_NO_ROW,
            "read after removal");
      check(sec_size == 0 && pri_size == 0, "after removal: sizes are written as 0");
      check(sec[0] == 'z' && pri[0] == 'z', "after removal: nothing copied");
      kv_idx_destroy(h);
   }

   // The size arguments go wherever `addr` says, which the host test picks. Most of its picks are misaligned, so the
   // read-back goes through memcpy.
   [[sysio::action]]
   void rdsizeptr(uint32_t addr) {
      const uint64_t self = get_self().value;
      must(put_row(tid_rdrules, self, "abc", 3, "abcdefg", 7));
      must(kv_idx_store(tid_rdrules_ix, self, "pppp", 4, "ss", 2));

      auto* const key_size = reinterpret_cast<uint32_t*>(addr);
      auto* const val_size = reinterpret_cast<uint32_t*>(addr + sizeof(uint32_t));
      auto        sizes_are = [&](uint32_t first, uint32_t second) {
         uint32_t got[2];
         memcpy(got, key_size, sizeof(got));
         return got[0] == first && got[1] == second;
      };

      const int32_t h = it_create(tid_rdrules, self, "abc", 3);
      check(kv_it_read(h, 0, nullptr, 0, nullptr, 0, key_size, val_size) == KV_OK, "primary read");
      check(sizes_are(3, 7), "primary read writes both sizes");
      check(kv_it_next(h, nullptr, 0, nullptr, 0, key_size, val_size) == KV_END, "primary move");
      check(sizes_are(0, 0), "a move that finds nothing writes both sizes as 0");
      check(kv_it_prev(h, nullptr, 0, nullptr, 0, key_size, val_size) == KV_OK, "primary prev");
      check(sizes_are(3, 7), "primary prev writes both sizes");
      check(kv_it_lower_bound(h, "abc", 3, nullptr, 0, nullptr, 0, key_size, val_size) == KV_OK, "primary seek");
      check(sizes_are(3, 7), "primary seek writes both sizes");
      kv_it_destroy(h);

      const int32_t x = idx_find(self, tid_rdrules_ix, "ss", 2);
      check(kv_idx_read(x, nullptr, 0, nullptr, 0, key_size, val_size) == KV_OK, "secondary read");
      check(sizes_are(2, 4), "secondary read writes both sizes");
      check(kv_idx_next(x, nullptr, 0, nullptr, 0, key_size, val_size) == KV_END, "secondary next");
      check(sizes_are(0, 0), "a secondary move that finds nothing writes both sizes as 0");
      check(kv_idx_prev(x, nullptr, 0, nullptr, 0, key_size, val_size) == KV_OK, "secondary prev");
      check(sizes_are(2, 4), "secondary prev writes both sizes");
      check(kv_idx_lower_bound(x, "ss", 2, nullptr, 0, nullptr, 0, key_size, val_size) == KV_OK, "secondary seek");
      check(sizes_are(2, 4), "secondary seek writes both sizes");
      kv_idx_destroy(x);

      must(kv_erase(tid_rdrules, "abc", 3));
      must(kv_idx_erase(tid_rdrules_ix, "pppp", 4, "ss", 2));
   }

   // A read copies the key, then the value, then the sizes. Each size pointer here is aligned, so the host writes
   // through it in that order. Two size pointers over each other are refused.
   [[sysio::action]]
   void rdoverlap() {
      const uint64_t self = get_self().value;
      must(put_row(tid_rdrules, self, "KKK", 3, "vvvvvv", 6));
      const int32_t h = it_create(tid_rdrules, self, "KKK", 3);

      uint32_t words[3] = {};
      char*    bytes    = reinterpret_cast<char*>(words);
      uint32_t key_size = 0, val_size = 0;

      check(kv_it_read(h, 0, bytes, 3, bytes, 6, &key_size, &val_size) == KV_OK, "shared buffer");
      check(memcmp(bytes, "vvvvvv", 6) == 0, "the value is copied after the key, over it");

      check(kv_it_read(h, 0, nullptr, 0, bytes, sizeof(words), &words[0], &words[1]) == KV_OK, "sizes in the buffer");
      check(words[0] == 3 && words[1] == 6, "the sizes are written after the value, over it");

      kv_it_destroy(h);
      must(kv_erase(tid_rdrules, "KKK", 3));
   }

   // ==========================================================================
   // R. Reading another contract's tables.
   // ==========================================================================

   [[sysio::action]]
   void xcseed() {
      const uint64_t self = get_self().value;
      must(put_row(tid_xc, self, "a", 1, "one", 3));
      must(put_row(tid_xc, self, "b", 1, "twotwo", 6));
      idx_put(tid_xc_sec, self, "s1", "p1");
      idx_put(tid_xc_sec, self, "s2", "p2");
   }

   // Every read names a code, and that decides whose rows come back. The receiver holds a row at the same key with a
   // different value, and no index entries, so a read that ignores `code` shows up.
   [[sysio::action]]
   void xciter(name other) {
      const uint64_t self = get_self().value;
      const uint64_t code = other.value;
      must(put_row(tid_xc, self, "a", 1, "mine", 4));

      char     key[2], val[8];
      uint32_t key_size = 0, val_size = 0;

      int32_t h = it_create(tid_xc, code, nullptr, 0);
      check(kv_it_read(h, 0, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK,
            "other's first row");
      check(key_size == 1 && key[0] == 'a' && val_size == 3 && memcmp(val, "one", 3) == 0, "other's row, not mine");
      check(kv_it_next(h, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK, "other's second row");
      check(key[0] == 'b' && val_size == 6 && memcmp(val, "twotwo", 6) == 0, "next stays in other's table");
      check(kv_it_read(h, 3, nullptr, 0, val, sizeof(val), &key_size, &val_size) == KV_OK, "other's row at an offset");
      check(memcmp(val, "two", 3) == 0, "offset applies to other's value");
      check(it_next(h) == KV_END, "other has two rows");
      prev_onto(h, "b", 1, "prev from end stays in other's table");
      check(kv_it_lower_bound(h, "a", 1, key, sizeof(key), val, sizeof(val), &key_size, &val_size) == KV_OK,
            "seek in other's table");
      check(key[0] == 'a' && memcmp(val, "one", 3) == 0, "seek stays in other's table");
      kv_it_destroy(h);

      // kv_get: present, at an offset, absent, and the receiver's own row at the same key.
      check(kv_get(tid_xc, code, "b", 1, 3, val, sizeof(val)) == 6, "other's row by key");
      check(memcmp(val, "two", 3) == 0, "other's value from the offset");
      check(get_row(tid_xc, code, "b", 1, nullptr, 0) == 6, "a zero-size read finds other's row");
      check(get_row(tid_xc, code, "z", 1, nullptr, 0) == -1, "a key other does not have");
      check(get_row(tid_xc, self, "b", 1, nullptr, 0) == -1, "a key only other has is absent here");
      check(get_row(tid_xc, self, "a", 1, val, sizeof(val)) == 4 && memcmp(val, "mine", 4) == 0,
            "my row is still mine");

      // Index entries.
      h = kv_idx_create(tid_xc_sec, code, nullptr, 0, nullptr, 0);
      idx_on(h, "s1", "p1", "other's first entry");
      idx_next_onto(h, "s2", "p2", "next stays in other's index");
      check(idx_next(h) == KV_END, "other has two entries");
      idx_prev_onto(h, "s2", "p2", "prev from end stays in other's index");
      kv_idx_destroy(h);
      h = kv_idx_find(tid_xc_sec, code, "s", 1, "s2", IDX_KEY_SIZE);
      idx_on(h, "s2", "p2", "find reaches other's entry");
      kv_idx_destroy(h);
      h = kv_idx_create(tid_xc_sec, self, nullptr, 0, nullptr, 0);
      check(it_status_of_idx(h) == KV_END, "my index is empty");
      kv_idx_destroy(h);
      check(idx_find(self, tid_xc_sec, "s2", IDX_KEY_SIZE) == -1, "other's entry is not in my index");

      // Writes take no code, so they stay on the receiver's rows.
      check(kv_erase(tid_xc, "b", 1) == KV_NO_ROW, "erase cannot reach other's row");
      check(get_row(tid_xc, code, "b", 1, nullptr, 0) == 6, "other's row is untouched");
      must(kv_erase(tid_xc, "a", 1));
      check(get_row(tid_xc, code, "a", 1, nullptr, 0) == 3, "erasing mine leaves other's");
   }

   // ==========================================================================
   // S. Handle numbers through the intrinsics. Contracts can see them, so they are consensus.
   // ==========================================================================

   // Close two slots, highest first and then out of order, and the lowest comes back first.
   [[sysio::action]]
   void slotreuse() {
      const uint64_t self = get_self().value;
      must(put_row(tid_slots, self, "K", 1, "v", 1));
      idx_put(tid_slots_sec, self, "ss", "pp");

      const auto open_pri = [&] { return it_create(tid_slots, self, nullptr, 0); };
      const auto open_sec = [&] { return kv_idx_create(tid_slots_sec, self, nullptr, 0, nullptr, 0); };

      for (int32_t i = 0; i < 4; ++i) check(open_pri() == i, "primary handles count up from 0");
      kv_it_destroy(3);
      kv_it_destroy(1);
      kv_it_destroy(2);
      check(open_pri() == 1, "the lowest free primary slot comes back first");
      check(open_pri() == 2, "then the next");
      check(open_pri() == 3, "then the one closed first");
      check(open_pri() == 4, "then a new one");

      // The secondary pool counts on its own, whatever the primary pool holds.
      for (int32_t i = 0; i < 4; ++i) {
         check(open_sec() == FIRST_SECONDARY_HANDLE + i, "secondary handles count up from 0");
      }
      kv_idx_destroy(FIRST_SECONDARY_HANDLE + 2);
      kv_idx_destroy(FIRST_SECONDARY_HANDLE);
      check(idx_find(self, tid_slots_sec, "ss", IDX_KEY_SIZE) == FIRST_SECONDARY_HANDLE,
            "find reuses the lowest slot");
      check(open_sec() == FIRST_SECONDARY_HANDLE + 2, "create reuses the next");
      check(open_sec() == FIRST_SECONDARY_HANDLE + 4, "then a new one");

      idx_del(tid_slots_sec, "ss", "pp");
      must(kv_erase(tid_slots, "K", 1));
   }

   [[sysio::action]]
   void ixdbldest() {
      idx_put(tid_slots_sec, get_self().value, "dd", "pp");
      const int32_t h = idx_find(get_self().value, tid_slots_sec, "dd", IDX_KEY_SIZE);
      kv_idx_destroy(h);
      kv_idx_destroy(h);
   }

   [[sysio::action]]
   void ixuaf() {
      idx_put(tid_slots_sec, get_self().value, "uu", "pp");
      const int32_t h = idx_find(get_self().value, tid_slots_sec, "uu", IDX_KEY_SIZE);
      kv_idx_destroy(h);
      uint32_t sec_size = 0, pri_size = 0;
      kv_idx_read(h, nullptr, 0, nullptr, 0, &sec_size, &pri_size);
   }

   // ==========================================================================
   // T. Limits.
   // ==========================================================================

   // Calls one intrinsic, picked from table_id_call, with a table_id the host test expects it to reject.
   [[sysio::action]]
   void tidbad(uint32_t call, uint32_t table_id) {
      const uint64_t self = get_self().value;
      switch (call) {
         case call_kv_get:        get_row(table_id, self, "k", 1, nullptr, 0); break;
         case call_kv_erase:      (void)kv_erase(table_id, "k", 1); break;
         case call_kv_idx_store:  (void)kv_idx_store(table_id, self, "p", 1, "s", 1); break;
         case call_kv_idx_update: (void)kv_idx_update(table_id, self, "p", 1, "s", 1, "t", 1); break;
         case call_kv_idx_erase:  (void)kv_idx_erase(table_id, "p", 1, "s", 1); break;
         case call_kv_it_create:  it_create(table_id, self, nullptr, 0); break;
         case call_kv_idx_create: kv_idx_create(table_id, self, nullptr, 0, nullptr, 0); break;
         case call_kv_idx_find:   kv_idx_find(table_id, self, nullptr, 0, "s", 1); break;
         default:                 check(false, "unknown call"); break;
      }
   }

   // The top table_id is usable everywhere.
   [[sysio::action]]
   void tidtop() {
      const uint64_t self = get_self().value;
      must(put_row(tid_top, self, "k", 1, "v", 1));
      check(get_row(tid_top, self, "k", 1, nullptr, 0) == 1, "kv_get at the top table_id");
      int32_t h = it_create(tid_top, self, nullptr, 0);
      check(it_status(h) == KV_OK, "kv_it_create at the top table_id");
      kv_it_destroy(h);

      idx_put(tid_top, self, "ss", "p1");
      idx_move(tid_top, self, "p1", "ss", "tt");
      h = kv_idx_create(tid_top, self, nullptr, 0, nullptr, 0);
      idx_on(h, "tt", "p1", "kv_idx_create at the top table_id");
      kv_idx_destroy(h);
      h = idx_find(self, tid_top, "tt", IDX_KEY_SIZE);
      check(h >= 0, "kv_idx_find at the top table_id");
      kv_idx_destroy(h);
      idx_del(tid_top, "tt", "p1");
      must(kv_erase(tid_top, "k", 1));
   }

   // The prefix cap is exact on all three calls that take a prefix. bigpfx, bigpfxidx and bigpfxfind go one over it.
   [[sysio::action]]
   void pfxmax() {
      const uint64_t self = get_self().value;
      static char at_cap[MAX_PREFIX_LIMIT] = {};
      int32_t h = it_create(tid_pfx, self, at_cap, sizeof(at_cap));
      check(it_status(h) == KV_END, "kv_it_create takes a prefix at the cap");
      kv_it_destroy(h);
      h = kv_idx_create(tid_pfx, self, at_cap, sizeof(at_cap), nullptr, 0);
      check(idx_next(h) == KV_END, "kv_idx_create takes a prefix at the cap");
      kv_idx_destroy(h);
      check(kv_idx_find(tid_pfx, self, at_cap, sizeof(at_cap), at_cap, sizeof(at_cap)) == -1,
            "kv_idx_find takes a prefix at the cap");
   }

   [[sysio::action]]
   void bigpfxidx() {
      static char huge[MAX_PREFIX_LIMIT + 1] = {};
      kv_idx_create(tid_pfx, get_self().value, huge, sizeof(huge), nullptr, 0);
   }

   [[sysio::action]]
   void bigpfxfind() {
      static char huge[MAX_PREFIX_LIMIT + 1] = {};
      kv_idx_find(tid_pfx, get_self().value, huge, sizeof(huge), huge, sizeof(huge));
   }

   // paysizeadd bills a row and an entry to `payer`. paysize and paysizesec then rewrite them with payer 0 at a size
   // the host test picks, and paysizedel removes both, so the test can follow every byte on either account.
   [[sysio::action]]
   void paysizeadd(name payer) {
      require_auth(payer);
      must(put_row(tid_paysize, payer.value, "M", 1, "v", 1));
      must(kv_idx_store(tid_paysize_ix, payer.value, "P", 1, "s", 1));
   }

   [[sysio::action]]
   void paysize(uint32_t size) {
      char value[64] = {};
      check(size <= sizeof(value), "paysize: value too large for this probe");
      check(kv_set(tid_paysize, 0, "M", 1, value, size, KV_UPDATE) == KV_OK, "update with payer 0");
   }

   [[sysio::action]]
   void paysizesec(uint32_t old_size, uint32_t new_size) {
      char sec[64];
      memset(sec, 's', sizeof(sec));
      check(old_size <= sizeof(sec) && new_size <= sizeof(sec), "paysizesec: key too large for this probe");
      check(kv_idx_update(tid_paysize_ix, 0, "P", 1, sec, old_size, sec, new_size) == KV_OK,
            "index update with payer 0");
   }

   [[sysio::action]]
   void paysizedel(uint32_t sec_size) {
      char sec[64];
      memset(sec, 's', sizeof(sec));
      must(kv_erase(tid_paysize, "M", 1));
      must(kv_idx_erase(tid_paysize_ix, "P", 1, sec, sec_size));
   }

   // The lim* actions work at sizes the host test picks, around limits it moves with setparams. The buffers come from
   // the heap so that these sizes do not enlarge the memory of every other action.
   [[sysio::action]]
   void limrow(uint32_t key_size, uint32_t value_size) {
      must(put_row(tid_limits, get_self().value, filled('k', key_size), key_size, filled('v', value_size), value_size));
   }

   // Reads the row back two ways. The whole key serves as the prefix, so this also runs a prefix at the cap on a real
   // row.
   [[sysio::action]]
   void limrowchk(uint32_t key_size, uint32_t value_size) {
      const uint64_t self = get_self().value;
      const char*    key  = filled('k', key_size);
      char*          buf  = static_cast<char*>(malloc(value_size));
      check(get_row(tid_limits, self, key, key_size, buf, value_size) == static_cast<int32_t>(value_size),
            "kv_get size");
      check(buf[0] == 'v' && buf[value_size - 1] == 'v', "kv_get bytes");

      const int32_t h = it_create(tid_limits, self, key, key_size);
      uint32_t got_key = 0, got_val = 0;
      check(kv_it_read(h, 0, nullptr, 0, nullptr, 0, &got_key, &got_val) == KV_OK, "the prefix finds the row");
      check(got_key == key_size && got_val == value_size, "kv_it_read sizes");
      kv_it_destroy(h);
   }

   [[sysio::action]]
   void limrowdel(uint32_t key_size) {
      must(kv_erase(tid_limits, filled('k', key_size), key_size));
   }

   [[sysio::action]]
   void limsec(uint32_t pri_size, uint32_t sec_size) {
      must(kv_idx_store(tid_limits_sec, get_self().value, filled('p', pri_size), pri_size, filled('s', sec_size),
                        sec_size));
   }

   [[sysio::action]]
   void limsecupd(uint32_t pri_size, uint32_t old_size, uint32_t new_size) {
      must(kv_idx_update(tid_limits_sec, get_self().value, filled('p', pri_size), pri_size, filled('s', old_size),
                         old_size, filled('s', new_size), new_size));
   }

   // As limrowchk: the whole key serves as the prefix.
   [[sysio::action]]
   void limsecchk(uint32_t sec_size) {
      const char*   sec = filled('s', sec_size);
      const int32_t h   = kv_idx_find(tid_limits_sec, get_self().value, sec, sec_size, sec, sec_size);
      check(h >= 0, "the entry is found by a prefix as long as its key");
      kv_idx_destroy(h);
   }

   [[sysio::action]]
   void limsecdel(uint32_t pri_size, uint32_t sec_size) {
      must(kv_idx_erase(tid_limits_sec, filled('p', pri_size), pri_size, filled('s', sec_size), sec_size));
   }

   // ==========================================================================
   // Writes that an abort further on takes back.
   // ==========================================================================

   // Rows and entries for abort_work to change, under each key tag. Run on every account a test compares, and more than
   // once on the shared tester, so a row or entry already there is fine.
   [[sysio::action]]
   void abrtseed() {
      const uint64_t self = get_self().value;
      for (const char tag : {'a', 'b'}) {
         const char upd[] = {tag, 'u'};
         const char del[] = {tag, 'd'};
         must(put_row(tid_abort, self, upd, sizeof(upd), "old", 3));
         must(put_row(tid_abort, self, del, sizeof(del), "old", 3));
         kv_idx_store(tid_abort_sec, self, upd, sizeof(upd), upd, sizeof(upd));
         kv_idx_store(tid_abort_sec, self, del, sizeof(del), del, sizeof(del));
      }
   }

   // The action finishes its writes, and the inline action it sends fails after writing too.
   [[sysio::action]]
   void abrtinline() {
      abort_work('a');
      sysio::action(std::vector<sysio::permission_level>{{get_self(), "active"_n}}, get_self(), "abrtchild"_n,
                    std::vector<char>{})
         .send();
   }

   [[sysio::action]]
   void abrtchild() {
      abort_work('b');
      check(false, "abort in the inline action");
   }

   // The same through a notification, on the notified contract's tables and RAM.
   [[sysio::action]]
   void abrtnotify(name notified) {
      abort_work('a');
      require_recipient(notified);
   }

   [[sysio::on_notify("*::abrtnotify")]]
   void onabrtnotify(name notified) {
      abort_work('b');
      check(false, "abort in the notified contract");
   }

   // The body returns normally, so every write has finished. validate_account_ram_deltas then refuses the bill on `other`,
   // who did not authorize this action.
   [[sysio::action]]
   void abrtram(name other) {
      abort_work('a');
      must(put_row(tid_abort, other.value, "o", 1, "billed", 6));
   }

   // The same refusal in a notified contract, which may not grow anyone else's RAM at all.
   [[sysio::action]]
   void abrtramnote(name notified, name other) {
      abort_work('a');
      require_recipient(notified);
   }

   [[sysio::on_notify("*::abrtramnote")]]
   void onabrtram(name notified, name other) {
      abort_work('b');
      must(put_row(tid_abort, other.value, "o", 1, "billed", 6));
   }

   // ==========================================================================
   // A row stored while an iterator sits at end.
   // ==========================================================================

   // An iterator at end has no key, so nothing it can do finds a row stored since, except a move that starts over.
   [[sysio::action]]
   void storeatend() {
      const uint64_t self = get_self().value;
      char     key[2];
      uint32_t size = 0, value_size = 0;

      const int32_t h = it_create(tid_atend, self, "a", 1);
      const int32_t other = it_create(tid_atend, self, "a", 1);
      check(it_status(h) == KV_END, "nothing in the prefix yet");
      must(put_row(tid_atend, self, "ab", 2, "v", 1));
      check(it_status(h) == KV_END, "a read stays at end");
      check(it_next(h) == KV_END, "next stays at end");
      prev_onto(h, "ab", 2, "prev starts from the end of the prefix, so it finds the row");
      check(kv_it_lower_bound(other, "a", 1, key, sizeof(key), nullptr, 0, &size, &value_size) == KV_OK, "a seek finds it");
      check(size == 2 && memcmp(key, "ab", 2) == 0, "the seek lands on the row");
      kv_it_destroy(h);
      kv_it_destroy(other);
      must(kv_erase(tid_atend, "ab", 2));

      const int32_t x = kv_idx_create(tid_atend_sec, self, "a", 1, nullptr, 0);
      const int32_t other_x = kv_idx_create(tid_atend_sec, self, "a", 1, nullptr, 0);
      check(it_status_of_idx(x) == KV_END, "nothing in the prefix yet");
      idx_put(tid_atend_sec, self, "ab", "p1");
      check(it_status_of_idx(x) == KV_END, "a read stays at end");
      check(idx_next(x) == KV_END, "next stays at end");
      idx_prev_onto(x, "ab", "p1", "prev starts from the end of the prefix, so it finds the entry");
      char     pri[IDX_KEY_SIZE];
      check(kv_idx_lower_bound(other_x, "a", 1, key, sizeof(key), pri, sizeof(pri), &size, &value_size) == KV_OK,
            "a seek finds it");
      same_entry(key, "ab", pri, "p1", size, value_size, "the seek lands on the entry");
      kv_idx_destroy(x);
      kv_idx_destroy(other_x);
      idx_del(tid_atend_sec, "ab", "p1");
   }

   // ==========================================================================
   // kv_idx_update edges.
   // ==========================================================================

   [[sysio::action]]
   void updedges() {
      const uint64_t self = get_self().value;
      idx_put(tid_updedges, self, "bb", "p1");

      // The old pair is missing and the new pair is taken: the missing entry is what the call reports.
      check(kv_idx_update(tid_updedges, self, "p1", IDX_KEY_SIZE, "aa", IDX_KEY_SIZE, "bb", IDX_KEY_SIZE) == KV_NO_ROW,
            "a missing entry wins over a taken new pair");

      // A primary key that matches nothing, beside a real entry at the old secondary key.
      check(kv_idx_update(tid_updedges, self, "p9", IDX_KEY_SIZE, "bb", IDX_KEY_SIZE, "cc", IDX_KEY_SIZE) == KV_NO_ROW,
            "an unknown primary key matches no entry");
      check(idx_find(self, tid_updedges, "cc", IDX_KEY_SIZE) == -1, "and nothing moved");

      // A secondary key another primary key holds is free as a pair, and the entry sorts by primary key among them.
      idx_put(tid_updedges, self, "aa", "p0");
      idx_put(tid_updedges, self, "ab", "p2");
      must(kv_idx_update(tid_updedges, self, "p0", IDX_KEY_SIZE, "aa", IDX_KEY_SIZE, "bb", IDX_KEY_SIZE));
      must(kv_idx_update(tid_updedges, self, "p2", IDX_KEY_SIZE, "ab", IDX_KEY_SIZE, "bb", IDX_KEY_SIZE));

      const int32_t h = idx_find(self, tid_updedges, "bb", IDX_KEY_SIZE);
      idx_on(h, "bb", "p0", "find lands on the lowest primary key");
      idx_next_onto(h, "bb", "p1", "then the entry that was already there");
      idx_next_onto(h, "bb", "p2", "then the higher one");
      check(idx_next(h) == KV_END, "and no more");
      kv_idx_destroy(h);

      idx_del(tid_updedges, "bb", "p0");
      idx_del(tid_updedges, "bb", "p1");
      idx_del(tid_updedges, "bb", "p2");
   }

   // ==========================================================================
   // Lookups take a key of any size. Only a write is held to a limit.
   // ==========================================================================

   [[sysio::action]]
   void nolimit() {
      const uint64_t self = get_self().value;
      const char*    big  = filled('k', MAX_PREFIX_LIMIT + 1);
      const uint32_t size = MAX_PREFIX_LIMIT + 1;
      must(put_row(tid_nolimit, self, "K", 1, "v", 1));
      idx_put(tid_nolimit_ix, self, "ss", "p1");
      idx_put(tid_nolimit_ix, self, "ss", "p2");

      check(get_row(tid_nolimit, self, big, size, nullptr, 0) == -1, "kv_get with a long key");
      check(kv_erase(tid_nolimit, big, size) == KV_NO_ROW, "kv_erase with a long key");
      check(kv_idx_erase(tid_nolimit_ix, big, size, "ss", IDX_KEY_SIZE) == KV_NO_ROW, "index erase, long primary");
      check(kv_idx_erase(tid_nolimit_ix, "p1", IDX_KEY_SIZE, big, size) == KV_NO_ROW, "index erase, long secondary");
      check(kv_idx_update(tid_nolimit_ix, self, big, size, "ss", IDX_KEY_SIZE, "tt", IDX_KEY_SIZE) == KV_NO_ROW,
            "index update, long primary");
      check(kv_idx_update(tid_nolimit_ix, self, "p1", IDX_KEY_SIZE, big, size, "tt", IDX_KEY_SIZE) == KV_NO_ROW,
            "index update, long old secondary");
      check(kv_idx_find(tid_nolimit_ix, self, "k", 1, big, size) == -1, "kv_idx_find with a long key");

      // A seek of the same size, below and above the entries. Equal secondary keys are a run, and a seek lands on the
      // start of it from anywhere.
      const char*   low = filled('a', size);
      char          sec[IDX_KEY_SIZE], pri[IDX_KEY_SIZE];
      uint32_t      sec_size = 0, pri_size = 0;
      const int32_t x = kv_idx_create(tid_nolimit_ix, self, nullptr, 0, nullptr, 0);
      check(kv_idx_lower_bound(x, low, size, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK,
            "a long seek below the entries");
      same_entry(sec, "ss", pri, "p1", sec_size, pri_size, "lands on the first entry");
      idx_next_onto(x, "ss", "p2", "the next entry");
      check(kv_idx_lower_bound(x, "ss", IDX_KEY_SIZE, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK,
            "a seek from the end of a run of equal keys");
      same_entry(sec, "ss", pri, "p1", sec_size, pri_size, "goes back to the start of the run");
      const char* high = filled('z', size);
      check(kv_idx_lower_bound(x, high, size, nullptr, 0, nullptr, 0, &sec_size, &pri_size) == KV_END,
            "a long seek above the entries");
      kv_idx_destroy(x);

      must(kv_erase(tid_nolimit, "K", 1));
      idx_del(tid_nolimit_ix, "ss", "p1");
      idx_del(tid_nolimit_ix, "ss", "p2");
   }

   // ==========================================================================
   // A pool at its limit, through the intrinsics.
   // ==========================================================================

   // Each pool holds 1024 handles of its own. A slot given back comes out next, and with everything closed numbering
   // starts again. Secondary handles open through both of the calls that make one.
   [[sysio::action]]
   void poolfull() {
      const uint64_t self = get_self().value;
      idx_put(tid_pool_sec, self, "ss", "pp");

      for (uint32_t i = 0; i < POOL_SLOTS; ++i) {
         check(it_create(tid_pool, self, nullptr, 0) == static_cast<int32_t>(i), "primary slots fill in order");
      }
      kv_it_destroy(500);
      check(it_create(tid_pool, self, nullptr, 0) == 500, "the slot given back at the limit comes out next");

      for (uint32_t i = 0; i < POOL_SLOTS; ++i) {
         const int32_t want = FIRST_SECONDARY_HANDLE + static_cast<int32_t>(i);
         const int32_t got  = i % 2 == 0 ? kv_idx_create(tid_pool_sec, self, nullptr, 0, nullptr, 0)
                                         : idx_find(self, tid_pool_sec, "ss", IDX_KEY_SIZE);
         check(got == want, "a full primary pool leaves every secondary slot free");
      }
      kv_idx_destroy(FIRST_SECONDARY_HANDLE + 500);
      check(idx_find(self, tid_pool_sec, "ss", IDX_KEY_SIZE) == FIRST_SECONDARY_HANDLE + 500,
            "the secondary slot given back comes out next");

      for (uint32_t i = 0; i < POOL_SLOTS; ++i) {
         kv_it_destroy(static_cast<int32_t>(i));
         kv_idx_destroy(FIRST_SECONDARY_HANDLE + static_cast<int32_t>(i));
      }
      check(it_create(tid_pool, self, nullptr, 0) == 0, "an empty primary pool starts at slot 0");
      check(kv_idx_create(tid_pool_sec, self, nullptr, 0, nullptr, 0) == FIRST_SECONDARY_HANDLE,
            "an empty secondary pool starts at slot 0");

      idx_del(tid_pool_sec, "ss", "pp");
   }

   // Fills one pool and gives one slot back, so the call after the refill is the one past the limit. `call` picks how
   // the handles open, which the host test walks by position.
   [[sysio::action]]
   void poollimit(uint32_t call) {
      const uint64_t self = get_self().value;
      idx_put(tid_pool_sec, self, "ss", "pp");
      const auto open = [&]() {
         if (call == 0) return it_create(tid_pool, self, nullptr, 0);
         if (call == 1) return kv_idx_create(tid_pool_sec, self, nullptr, 0, nullptr, 0);
         return idx_find(self, tid_pool_sec, "ss", IDX_KEY_SIZE);
      };
      int32_t last = -1;
      for (uint32_t i = 0; i < POOL_SLOTS; ++i) last = open();
      if (call == 0) kv_it_destroy(last);
      else kv_idx_destroy(last);
      check(open() == last, "the slot at the top comes back");
      open();
   }

   // ==========================================================================
   // Spans and size pointers in the corners.
   // ==========================================================================

   // A size pointer is a reference, so null aborts on every one of the 16. `call` is one of the eight calls with size
   // outputs and `which` is its first or second size, which the host test walks by position.
   [[sysio::action]]
   void rdnullsz(uint32_t call, uint32_t which) {
      const uint64_t self = get_self().value;
      must(put_row(tid_rdrules, self, "abc", 3, "abcdefg", 7));
      must(kv_idx_store(tid_rdrules_ix, self, "pppp", 4, "ss", 2));
      const int32_t h = it_create(tid_rdrules, self, "abc", 3);
      const int32_t x = idx_find(self, tid_rdrules_ix, "ss", 2);

      uint32_t  size   = 0;
      uint32_t* first  = which == 0 ? nullptr : &size;
      uint32_t* second = which == 0 ? &size : nullptr;
      switch (call) {
         case 0: kv_it_read(h, 0, nullptr, 0, nullptr, 0, first, second); break;
         case 1: kv_it_next(h, nullptr, 0, nullptr, 0, first, second); break;
         case 2: kv_it_prev(h, nullptr, 0, nullptr, 0, first, second); break;
         case 3: kv_it_lower_bound(h, "abc", 3, nullptr, 0, nullptr, 0, first, second); break;
         case 4: kv_idx_read(x, nullptr, 0, nullptr, 0, first, second); break;
         case 5: kv_idx_next(x, nullptr, 0, nullptr, 0, first, second); break;
         case 6: kv_idx_prev(x, nullptr, 0, nullptr, 0, first, second); break;
         case 7: kv_idx_lower_bound(x, "ss", 2, nullptr, 0, nullptr, 0, first, second); break;
         default: check(false, "rdnullsz: unknown call");
      }
   }

   // Every input is read before any output is written, so a key or seek may sit inside the buffer the call fills.
   [[sysio::action]]
   void rdinoverlap() {
      const uint64_t self = get_self().value;
      must(put_row(tid_spans, self, "KKK", 3, "vvvvvv", 6));
      idx_put(tid_spans_sec, self, "ss", "pp");
      char     buf[8];
      uint32_t first = 0, second = 0;

      memcpy(buf, "KKK", 3);
      check(kv_get(tid_spans, self, buf, 3, 0, buf, sizeof(buf)) == 6, "kv_get, key inside the value buffer");
      check(memcmp(buf, "vvvvvv", 6) == 0, "kv_get found the row, then wrote over its key");

      const int32_t h = it_create(tid_spans, self, nullptr, 0);
      memcpy(buf, "KKK", 3);
      check(kv_it_lower_bound(h, buf, 3, nullptr, 0, buf, sizeof(buf), &first, &second) == KV_OK,
            "kv_it_lower_bound, seek inside the value buffer");
      check(first == 3 && second == 6 && memcmp(buf, "vvvvvv", 6) == 0, "the seek found the row, then the value landed");
      kv_it_destroy(h);

      const int32_t x = kv_idx_create(tid_spans_sec, self, nullptr, 0, nullptr, 0);
      memcpy(buf, "ss", 2);
      check(kv_idx_lower_bound(x, buf, 2, nullptr, 0, buf, sizeof(buf), &first, &second) == KV_OK,
            "kv_idx_lower_bound, seek inside the primary key buffer");
      check(first == 2 && second == 2 && memcmp(buf, "pp", 2) == 0, "the seek found the entry, then the key landed");
      kv_idx_destroy(x);

      must(kv_erase(tid_spans, "KKK", 3));
      idx_del(tid_spans_sec, "ss", "pp");
   }

   // Every call that fills two buffers copies the first field, then the second, so with one buffer the second wins.
   [[sysio::action]]
   void rdoutoverlap() {
      const uint64_t self = get_self().value;
      must(put_row(tid_spans, self, "a", 1, "first", 5));
      must(put_row(tid_spans, self, "b", 1, "second", 6));
      idx_put(tid_spans_sec, self, "aa", "11");
      idx_put(tid_spans_sec, self, "bb", "22");
      char     buf[8];
      uint32_t first = 0, second = 0;

      const int32_t h = it_create(tid_spans, self, nullptr, 0);
      check(kv_it_next(h, buf, sizeof(buf), buf, sizeof(buf), &first, &second) == KV_OK, "kv_it_next, one buffer");
      check(memcmp(buf, "second", 6) == 0, "kv_it_next copies the value over the key");
      check(kv_it_prev(h, buf, sizeof(buf), buf, sizeof(buf), &first, &second) == KV_OK, "kv_it_prev, one buffer");
      check(memcmp(buf, "first", 5) == 0, "kv_it_prev copies the value over the key");
      check(kv_it_lower_bound(h, "b", 1, buf, sizeof(buf), buf, sizeof(buf), &first, &second) == KV_OK,
            "kv_it_lower_bound, one buffer");
      check(memcmp(buf, "second", 6) == 0, "kv_it_lower_bound copies the value over the key");
      kv_it_destroy(h);

      const int32_t x = kv_idx_create(tid_spans_sec, self, nullptr, 0, nullptr, 0);
      check(kv_idx_read(x, buf, sizeof(buf), buf, sizeof(buf), &first, &second) == KV_OK, "kv_idx_read, one buffer");
      check(memcmp(buf, "11", 2) == 0, "kv_idx_read copies the primary key over the secondary key");
      check(kv_idx_next(x, buf, sizeof(buf), buf, sizeof(buf), &first, &second) == KV_OK, "kv_idx_next, one buffer");
      check(memcmp(buf, "22", 2) == 0, "kv_idx_next copies the primary key over the secondary key");
      check(kv_idx_prev(x, buf, sizeof(buf), buf, sizeof(buf), &first, &second) == KV_OK, "kv_idx_prev, one buffer");
      check(memcmp(buf, "11", 2) == 0, "kv_idx_prev copies the primary key over the secondary key");
      check(kv_idx_lower_bound(x, "bb", 2, buf, sizeof(buf), buf, sizeof(buf), &first, &second) == KV_OK,
            "kv_idx_lower_bound, one buffer");
      check(memcmp(buf, "22", 2) == 0, "kv_idx_lower_bound copies the primary key over the secondary key");
      kv_idx_destroy(x);

      must(kv_erase(tid_spans, "a", 1));
      must(kv_erase(tid_spans, "b", 1));
      idx_del(tid_spans_sec, "aa", "11");
      idx_del(tid_spans_sec, "bb", "22");
   }

   // A primary and a secondary key may overlap in memory, as a key and a value may. Both are stored as they were.
   [[sysio::action]]
   void ixalias() {
      const uint64_t self  = get_self().value;
      const char*    bytes = "abcdef";
      must(kv_idx_store(tid_spans_sec, self, bytes, 4, bytes + 2, 4));

      const int32_t h = idx_find(self, tid_spans_sec, "cdef", 4);
      check(h >= 0, "the secondary key is its own bytes");
      char     pri[4];
      uint32_t sec_size = 0, pri_size = 0;
      check(kv_idx_read(h, nullptr, 0, pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "read the entry");
      check(pri_size == 4 && memcmp(pri, "abcd", 4) == 0, "the primary key is its own bytes");
      kv_idx_destroy(h);

      must(kv_idx_erase(tid_spans_sec, bytes, 4, bytes + 2, 4));
      check(idx_find(self, tid_spans_sec, "cdef", 4) == -1, "the same two spans erase it");
   }

   // ==========================================================================
   // Two iterators on one row.
   // ==========================================================================

   // forget_row has to reach every slot on the row, not just the first. Each iterator reports no_row and then steps from
   // the key it was on.
   [[sysio::action]]
   void twoslots() {
      const uint64_t self = get_self().value;
      must(put_row(tid_twoslots, self, "a", 1, "v", 1));
      must(put_row(tid_twoslots, self, "b", 1, "v", 1));
      must(put_row(tid_twoslots, self, "c", 1, "v", 1));

      const int32_t up   = kv_it_create(tid_twoslots, self, nullptr, 0, "b", 1);
      const int32_t down = kv_it_create(tid_twoslots, self, nullptr, 0, "b", 1);
      must(kv_erase(tid_twoslots, "b", 1));
      check(it_status(up) == KV_NO_ROW, "the first iterator on an erased row reads no_row");
      check(it_status(down) == KV_NO_ROW, "and so does the second");
      char     key[1];
      uint32_t key_size = 0, value_size = 0;
      check(kv_it_next(up, key, sizeof(key), nullptr, 0, &key_size, &value_size) == KV_OK, "next from the erased key");
      check(key[0] == 'c', "lands past the old key");
      prev_onto(down, "a", 1, "prev from the erased key lands below it");
      kv_it_destroy(up);
      kv_it_destroy(down);

      idx_put(tid_twoslot_ix, self, "aa", "p1");
      idx_put(tid_twoslot_ix, self, "bb", "p2");
      idx_put(tid_twoslot_ix, self, "cc", "p3");

      // Moved by an update, and then gone by an erase, with both iterators on the entry each time.
      int32_t one = kv_idx_create(tid_twoslot_ix, self, nullptr, 0, "bb", IDX_KEY_SIZE);
      int32_t two = kv_idx_create(tid_twoslot_ix, self, nullptr, 0, "bb", IDX_KEY_SIZE);
      must(kv_idx_update(tid_twoslot_ix, self, "p2", IDX_KEY_SIZE, "bb", IDX_KEY_SIZE, "dd", IDX_KEY_SIZE));
      check(it_status_of_idx(one) == KV_NO_ROW, "the first iterator on a moved entry reads no_row");
      check(it_status_of_idx(two) == KV_NO_ROW, "and so does the second");
      idx_next_onto(one, "cc", "p3", "next from the old key, not from where the entry went");
      idx_prev_onto(two, "aa", "p1", "prev from the old key");
      kv_idx_destroy(one);
      kv_idx_destroy(two);

      one = kv_idx_create(tid_twoslot_ix, self, nullptr, 0, "cc", IDX_KEY_SIZE);
      two = kv_idx_create(tid_twoslot_ix, self, nullptr, 0, "cc", IDX_KEY_SIZE);
      idx_del(tid_twoslot_ix, "cc", "p3");
      check(it_status_of_idx(one) == KV_NO_ROW, "the first iterator on an erased entry reads no_row");
      check(it_status_of_idx(two) == KV_NO_ROW, "and so does the second");
      idx_next_onto(one, "dd", "p2", "next from the erased key");
      idx_prev_onto(two, "aa", "p1", "prev from the erased key");
      kv_idx_destroy(one);
      kv_idx_destroy(two);

      must(kv_erase(tid_twoslots, "a", 1));
      must(kv_erase(tid_twoslots, "c", 1));
      idx_del(tid_twoslot_ix, "aa", "p1");
      idx_del(tid_twoslot_ix, "dd", "p2");
   }

   // Stores one key of an empty table, for a test that looks for it from a neighboring table or account.
   [[sysio::action]]
   void onekey(uint32_t table_id) {
      const uint64_t self = get_self().value;
      must(put_row(table_id, self, "kk", IDX_KEY_SIZE, "v", 1));
      idx_put(table_id, self, "kk", "pp");
   }

   // That key is looked for here, in a table that does not hold it. The row or entry a lookup lands on next is the one
   // onekey stored, so only the check on the table or the code stands between it and a false hit.
   [[sysio::action]]
   void onekeymiss(uint32_t table_id, name code) {
      check(idx_find(code.value, table_id, "kk", IDX_KEY_SIZE) == -1, "kv_idx_find stops at the edge of its table");
      check(get_row(table_id, code.value, "kk", IDX_KEY_SIZE, nullptr, 0) == -1, "kv_get stops there too");
   }

   // ==========================================================================
   // Rows of every size, for the snapshot tests.
   // ==========================================================================

   // pattern() gives these their bytes, and the host test computes the same ones to compare. The chain is the test's own,
   // so each key is stored once.
   [[sysio::action]]
   void sizerow(uint32_t seed, uint32_t key_size, uint32_t value_size) {
      must(kv_set(tid_sizes, get_self().value, pattern(seed, key_size), key_size, pattern(seed, value_size), value_size,
                  KV_INSERT));
   }

   [[sysio::action]]
   void sizeentry(uint32_t seed, uint32_t sec_size, uint32_t pri_size) {
      must(kv_idx_store(tid_sizes_sec, get_self().value, pattern(seed, pri_size), pri_size, pattern(seed, sec_size),
                        sec_size));
   }

   // ==========================================================================
   // Key order.
   // ==========================================================================

   // Keys of exactly 8 bytes take the host's integer compare, in rows and in both entry fields. Stored out of order they
   // still walk in byte order, and a seek of another size takes the byte compare and lands where that order says.
   [[sysio::action]]
   void keyorder() {
      static constexpr uint32_t stored[ORDER_KEYS] = {4, 1, 5, 0, 3, 2};
      static constexpr uint32_t above_seeks        = 4; // the key both seeks land on
      static constexpr char     one_byte[]         = {'\x80'};

      const uint64_t self = get_self().value;
      for (uint32_t i : stored) {
         must(kv_set(tid_order, self, order_keys[i], ORDER_KEY_SIZE, order_keys[i], 0, KV_INSERT));
         must(kv_idx_store(tid_order_sec, self, order_keys[i], ORDER_KEY_SIZE, order_keys[i], ORDER_KEY_SIZE));
         must(kv_idx_store(tid_order_pri, self, order_keys[i], ORDER_KEY_SIZE, order_keys[0], ORDER_KEY_SIZE));
      }

      char     key[ORDER_KEY_SIZE];
      uint32_t key_size = 0, value_size = 0;
      const int32_t row = it_create(tid_order, self, nullptr, 0);
      check(it_key(row, key, sizeof(key), &key_size) == KV_OK, "rows start at the lowest key");
      same_order_key(key, key_size, 0, "rows start at the lowest key");
      for (uint32_t i = 1; i < ORDER_KEYS; ++i) {
         check(kv_it_next(row, key, sizeof(key), nullptr, 0, &key_size, &value_size) == KV_OK, "rows walk up");
         same_order_key(key, key_size, i, "rows walk up in byte order");
      }
      check(it_next(row) == KV_END, "the highest key ends the walk up");
      for (uint32_t i = ORDER_KEYS; i-- > 0;) prev_onto(row, order_keys[i], ORDER_KEY_SIZE, "rows walk down in byte order");

      check(kv_it_lower_bound(row, one_byte, sizeof(one_byte), key, sizeof(key), nullptr, 0, &key_size, &value_size) ==
            KV_OK, "a shorter seek finds a row");
      same_order_key(key, key_size, above_seeks, "a shorter seek lands on the key it starts");
      // One zero byte past the key below, so it sorts above that key and under the next.
      char nine_bytes[ORDER_KEY_SIZE + 1] = {};
      memcpy(nine_bytes, order_keys[above_seeks - 1], ORDER_KEY_SIZE);
      check(kv_it_lower_bound(row, nine_bytes, sizeof(nine_bytes), key, sizeof(key), nullptr, 0, &key_size, &value_size) ==
            KV_OK, "a longer seek finds a row");
      same_order_key(key, key_size, above_seeks, "a longer seek lands above the key it extends");
      kv_it_destroy(row);

      walk_entries(self, tid_order_sec, false);
      walk_entries(self, tid_order_pri, true);
   }

private:
   /// Walks all of an index up and then down. Entry i holds order_keys[i] as its primary key, and as its secondary key too
   /// unless `shared_sec`, when every entry holds order_keys[0] there.
   static void walk_entries(uint64_t code, uint32_t table_id, bool shared_sec) {
      char     sec[ORDER_KEY_SIZE], pri[ORDER_KEY_SIZE];
      uint32_t sec_size = 0, pri_size = 0;
      const auto on = [&](uint32_t i, const char* msg) {
         same_order_key(sec, sec_size, shared_sec ? 0 : i, msg);
         same_order_key(pri, pri_size, i, msg);
      };

      const int32_t h = idx_seek(code, table_id, nullptr, 0);
      check(kv_idx_read(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK,
            "entries start at the lowest key");
      on(0, "entries start at the lowest key");
      for (uint32_t i = 1; i < ORDER_KEYS; ++i) {
         check(kv_idx_next(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "entries walk up");
         on(i, "entries walk up in byte order");
      }
      check(idx_next(h) == KV_END, "the highest key ends the walk up");
      for (uint32_t i = ORDER_KEYS; i-- > 0;) {
         check(kv_idx_prev(h, sec, sizeof(sec), pri, sizeof(pri), &sec_size, &pri_size) == KV_OK, "entries walk down");
         on(i, "entries walk down in byte order");
      }
      kv_idx_destroy(h);
   }

   /// Changes one row and one entry of each kind under `tag`: stores new ones, rewrites what abrtseed left, removes the
   /// rest. Leaves the same state however many times it has been aborted.
   void abort_work(char tag) {
      const uint64_t self    = get_self().value;
      const char     fresh[] = {tag, 'n'};
      const char     upd[]   = {tag, 'u'};
      const char     del[]   = {tag, 'd'};
      const char     moved[] = {tag, 'm'};
      must(put_row(tid_abort, self, fresh, sizeof(fresh), "new", 3));
      must(kv_set(tid_abort, self, upd, sizeof(upd), "changed", 7, KV_UPDATE));
      must(kv_erase(tid_abort, del, sizeof(del)));
      must(kv_idx_store(tid_abort_sec, self, fresh, sizeof(fresh), fresh, sizeof(fresh)));
      must(kv_idx_update(tid_abort_sec, self, upd, sizeof(upd), upd, sizeof(upd), moved, sizeof(moved)));
      must(kv_idx_erase(tid_abort_sec, del, sizeof(del), del, sizeof(del)));
   }

   /// `size` bytes counting up from `seed`, wrapping at 256, so 256 of them hold every byte value. Each doubling is one
   /// host memcpy: a 1 MiB value in a loop of its own would take seconds under the interpreter.
   static const char* pattern(uint32_t seed, uint32_t size) {
      static constexpr uint32_t period = 256;
      char* bytes = static_cast<char*>(malloc(size ? size : 1));
      const uint32_t first = size < period ? size : period;
      for (uint32_t i = 0; i < first; ++i) bytes[i] = static_cast<char>(seed + i);
      for (uint32_t done = first; done < size;) {
         const uint32_t step = done < size - done ? done : size - done;
         memcpy(bytes + done, bytes, step);
         done += step;
      }
      return bytes;
   }

   /// Heap bytes holding `size` copies of `fill`. Each action runs once in its own memory, so nothing frees them.
   static const char* filled(char fill, uint32_t size) {
      char* bytes = static_cast<char*>(malloc(size));
      memset(bytes, fill, size);
      return bytes;
   }

   /// The status of a secondary iterator.
   static int32_t it_status_of_idx(int32_t handle) {
      uint32_t sec_size = 0, pri_size = 0;
      return kv_idx_read(handle, nullptr, 0, nullptr, 0, &sec_size, &pri_size);
   }

   /// Leaves a primary iterator open at slot 0 and sends `child` as an inline action.
   void open_handle_and_send(name child) {
      const uint64_t self   = get_self().value;
      const char     k[]    = {'K'};
      char           marker = 0;
      must(put_row(tid_inliso, self, k, sizeof(k), "v", 1));
      check(it_create(tid_inliso, self, &marker, 0) == 0, "first primary alloc must be slot 0");
      sysio::action(std::vector<sysio::permission_level>{{get_self(), "active"_n}}, get_self(), child,
                    std::vector<char>{})
         .send();
   }

   /// Opens one iterator in each pool over the first receiver's tables; both must get slot 0.
   void open_both_pools() {
      const uint64_t code   = get_first_receiver().value;
      const char     sec[]  = {'S'};
      char           marker = 0;
      check(it_create(tid_leak_pri, code, &marker, 0) == 0, "primary pool must start at slot 0");
      check(idx_find(code, tid_leak_sec, sec, sizeof(sec)) == FIRST_SECONDARY_HANDLE,
            "secondary pool must start at slot 0");
   }
};
