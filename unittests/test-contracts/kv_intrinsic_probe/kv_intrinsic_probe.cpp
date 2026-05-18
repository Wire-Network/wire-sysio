// Adversarial probe of the raw kv_* host intrinsics.
// Uses only extern "C" decls to bypass CDT's kv_multi_index / kv::table
// wrappers, so we can pass inputs CDT would never emit: zero sizes,
// oversize spans, stale/forged iterator handles, cross-pool handles,
// table_ids above the uint16 namespace, etc.
//
// Each action is self-contained and uses a distinct table_id so shared-state
// accumulation across actions is safe under a shared tester.

#include <sysio/sysio.hpp>
#include <cstring>
#include <cstdint>
#include <limits>

// Raw kv_* host intrinsic declarations. Listed here instead of via <sysio/kv.h>
// so every adversarial test is explicit about the ABI it is exercising.
extern "C" {
__attribute__((sysio_wasm_import))
int64_t  kv_set(uint32_t table_id, uint64_t payer, const void* key, uint32_t key_size,
                const void* value, uint32_t value_size);
__attribute__((sysio_wasm_import))
int32_t  kv_get(uint32_t table_id, uint64_t code, const void* key, uint32_t key_size,
                void* value, uint32_t value_size);
__attribute__((sysio_wasm_import))
int64_t  kv_erase(uint32_t table_id, const void* key, uint32_t key_size);
__attribute__((sysio_wasm_import))
int32_t  kv_contains(uint32_t table_id, uint64_t code, const void* key, uint32_t key_size);

__attribute__((sysio_wasm_import))
uint32_t kv_it_create(uint32_t table_id, uint64_t code, const void* prefix, uint32_t prefix_size);
__attribute__((sysio_wasm_import))
void     kv_it_destroy(uint32_t handle);
__attribute__((sysio_wasm_import))
int32_t  kv_it_status(uint32_t handle);
__attribute__((sysio_wasm_import))
int32_t  kv_it_next(uint32_t handle);
__attribute__((sysio_wasm_import))
int32_t  kv_it_prev(uint32_t handle);
__attribute__((sysio_wasm_import))
int32_t  kv_it_lower_bound(uint32_t handle, const void* key, uint32_t key_size);
__attribute__((sysio_wasm_import))
int32_t  kv_it_key(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size,
                   uint32_t* actual_size);
__attribute__((sysio_wasm_import))
int32_t  kv_it_value(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size,
                     uint32_t* actual_size);

__attribute__((sysio_wasm_import))
void     kv_idx_store(uint64_t payer, uint32_t table_id,
                      const void* pri_key, uint32_t pri_key_size,
                      const void* sec_key, uint32_t sec_key_size);
__attribute__((sysio_wasm_import))
void     kv_idx_remove(uint32_t table_id,
                       const void* pri_key, uint32_t pri_key_size,
                       const void* sec_key, uint32_t sec_key_size);
__attribute__((sysio_wasm_import))
void     kv_idx_update(uint64_t payer, uint32_t table_id,
                       const void* pri_key, uint32_t pri_key_size,
                       const void* old_sec_key, uint32_t old_sec_key_size,
                       const void* new_sec_key, uint32_t new_sec_key_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_find_secondary(uint64_t code, uint32_t table_id,
                               const void* sec_key, uint32_t sec_key_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_lower_bound(uint64_t code, uint32_t table_id,
                            const void* sec_key, uint32_t sec_key_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_next(uint32_t handle);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_prev(uint32_t handle);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_key(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size,
                    uint32_t* actual_size);
__attribute__((sysio_wasm_import))
int32_t  kv_idx_primary_key(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size,
                            uint32_t* actual_size);
__attribute__((sysio_wasm_import))
void     kv_idx_destroy(uint32_t handle);
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

// kv_it_stat values (from chain/kv_context.hpp).
static constexpr int32_t IT_OK     = 0;
static constexpr int32_t IT_END    = 1;
static constexpr int32_t IT_ERASED = 2;

// From chain/config.hpp. Defaults unless chain admin overrides.
static constexpr uint32_t MAX_KEY_SIZE        = 256;
static constexpr uint32_t MAX_VALUE_SIZE      = 256 * 1024;
static constexpr uint32_t MAX_SEC_KEY_SIZE    = 256;
static constexpr uint32_t MAX_PREFIX_LIMIT    = 1024;

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

      int64_t billed = kv_set(tid_zval, self, key_a, sizeof(key_a), &marker, 0);
      check(billed >= 0, "kv_set(value_size=0) must succeed (returns RAM billable)");

      int32_t sz = kv_get(tid_zval, self, key_a, sizeof(key_a), buf, sizeof(buf));
      check(sz == 0, "kv_get on empty-value row must return 0 (found, empty)");
      check(kv_contains(tid_zval, self, key_a, sizeof(key_a)) == 1,
            "kv_contains must return 1 for empty-value row");

      // dest_size=0 path: return actual size without touching dest.
      sz = kv_get(tid_zval, self, key_a, sizeof(key_a), &marker, 0);
      check(sz == 0, "kv_get(dest_size=0) on empty-value row must return 0");

      uint32_t h = kv_it_create(tid_zval, self, &marker, 0);
      check(kv_it_status(h) == IT_OK, "iterator must be positioned at the row");
      uint32_t actual = 99;
      int32_t  st     = kv_it_value(h, 0, buf, sizeof(buf), &actual);
      check(st == IT_OK && actual == 0, "kv_it_value on empty value must report actual_size=0");
      kv_it_destroy(h);

      // Grow then shrink back to 0.
      const char val[] = "XYZ";
      kv_set(tid_zval, self, key_a, sizeof(key_a), val, sizeof(val));
      kv_set(tid_zval, self, key_a, sizeof(key_a), &marker, 0);
      sz = kv_get(tid_zval, self, key_a, sizeof(key_a), buf, sizeof(buf));
      check(sz == 0, "shrink back to empty failed");
   }

   // Zero-length SECONDARY key is legal.
   [[sysio::action]]
   void zseckey() {
      const uint64_t self   = get_self().value;
      char           marker = 0;
      char           buf[16] = {};

      // Primary anchors.
      const char pkey_p[] = {'P'};
      const char pkey_q[] = {'Q'};
      kv_set(tid_zseck_pri, self, pkey_p, sizeof(pkey_p), "p", 1);
      kv_set(tid_zseck_pri, self, pkey_q, sizeof(pkey_q), "q", 1);

      // Two secondary entries with empty sec_key, different pri_key bytes.
      kv_idx_store(self, tid_zseck_sec, pkey_p, sizeof(pkey_p), &marker, 0);
      kv_idx_store(self, tid_zseck_sec, pkey_q, sizeof(pkey_q), &marker, 0);

      int32_t h = kv_idx_find_secondary(self, tid_zseck_sec, &marker, 0);
      check(h >= 0, "kv_idx_find_secondary(empty) must return a valid handle");

      uint32_t actual = 99;
      check(kv_idx_key(h, 0, buf, sizeof(buf), &actual) == IT_OK && actual == 0,
            "kv_idx_key on empty sec_key must report actual_size=0");

      actual = 99;
      check(kv_idx_primary_key(h, 0, buf, sizeof(buf), &actual) == IT_OK &&
            actual == sizeof(pkey_p),
            "kv_idx_primary_key must return stored pri_key bytes");
      check(buf[0] == 'P' || buf[0] == 'Q', "primary_key must be one of the anchors");
      char first = buf[0];

      check(kv_idx_next(h) == IT_OK, "next must land on the duplicate-sec entry");
      actual = 99;
      kv_idx_primary_key(h, 0, buf, sizeof(buf), &actual);
      check(actual == sizeof(pkey_p) && buf[0] != first,
            "second duplicate-sec entry must resolve to the other primary");
      check(kv_idx_next(h) == IT_END, "next past last empty-sec entry must report end");
      kv_idx_destroy(h);

      // Update empty -> non-empty and back.
      const char nonempty[] = {0x10, 0x20};
      kv_idx_update(self, tid_zseck_sec, pkey_p, sizeof(pkey_p), &marker, 0, nonempty, sizeof(nonempty));
      h = kv_idx_find_secondary(self, tid_zseck_sec, nonempty, sizeof(nonempty));
      check(h >= 0, "post-update find_secondary(nonempty) must succeed");
      kv_idx_destroy(h);
      kv_idx_update(self, tid_zseck_sec, pkey_p, sizeof(pkey_p), nonempty, sizeof(nonempty), &marker, 0);

      // Cleanup.
      kv_idx_remove(tid_zseck_sec, pkey_p, sizeof(pkey_p), &marker, 0);
      kv_idx_remove(tid_zseck_sec, pkey_q, sizeof(pkey_q), &marker, 0);
      kv_erase(tid_zseck_pri, pkey_p, sizeof(pkey_p));
      kv_erase(tid_zseck_pri, pkey_q, sizeof(pkey_q));
   }

   // kv_contains agrees with kv_get for present / absent rows.
   [[sysio::action]]
   void contcns() {
      const uint64_t self = get_self().value;
      const char k1[] = {'K', '1'};
      const char k2[] = {'K', '2'};

      // Row present: contains==1, get>=0.
      kv_set(tid_contcns, self, k1, sizeof(k1), "v", 1);
      check(kv_contains(tid_contcns, self, k1, sizeof(k1)) == 1, "contains: present row");
      char tmp[4];
      check(kv_get(tid_contcns, self, k1, sizeof(k1), tmp, sizeof(tmp)) == 1,
            "get: present row returns size");

      // Row absent: contains==0, get==-1.
      check(kv_contains(tid_contcns, self, k2, sizeof(k2)) == 0, "contains: absent row");
      check(kv_get(tid_contcns, self, k2, sizeof(k2), tmp, sizeof(tmp)) == -1,
            "get: absent row returns -1");

      // Wrong code: contains==0, get==-1 even when the key exists in receiver's table.
      const uint64_t foreign_code = name{"sysio"_n}.value;
      check(kv_contains(tid_contcns, foreign_code, k1, sizeof(k1)) == 0,
            "contains: cross-code must not find receiver's row");
      check(kv_get(tid_contcns, foreign_code, k1, sizeof(k1), tmp, sizeof(tmp)) == -1,
            "get: cross-code must not find receiver's row");
   }

   // An iterator whose row is erased mid-txn transitions to iterator_erased (2).
   // Subsequent reads must not OOB-access the erased bytes.
   [[sysio::action]]
   void itinvl() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      kv_set(tid_itinvl, self, k, sizeof(k), "v", 1);

      char     marker = 0;
      char     buf[8] = {};
      uint32_t h = kv_it_create(tid_itinvl, self, &marker, 0);
      check(kv_it_status(h) == IT_OK, "iterator must start at the row");

      kv_erase(tid_itinvl, k, sizeof(k));

      // status itself can be iterator_ok until observation forces re-seek, but
      // any data-read through the slot must observe the erase.
      uint32_t actual = 99;
      int32_t  st     = kv_it_key(h, 0, buf, sizeof(buf), &actual);
      check(st == IT_ERASED || st == IT_END,
            "kv_it_key after erase must report erased or end status");
      check(actual == 0, "kv_it_key after erase must not report a byte count");
      kv_it_destroy(h);
   }

   // Iterator in end state: key/value/prev all behave deterministically.
   [[sysio::action]]
   void endstat() {
      const uint64_t self = get_self().value;
      char marker = 0;

      // Empty table -> iterator starts in end state.
      uint32_t h = kv_it_create(tid_endstat, self, &marker, 0);
      check(kv_it_status(h) == IT_END, "iter on empty (code,table) must be end");

      // All data reads in end state: return end, actual_size=0.
      char     buf[4];
      uint32_t actual = 99;
      check(kv_it_key(h, 0, buf, sizeof(buf), &actual) == IT_END, "key in end state");
      check(actual == 0, "key actual_size must be 0 in end state");
      actual = 99;
      check(kv_it_value(h, 0, buf, sizeof(buf), &actual) == IT_END, "value in end state");
      check(actual == 0, "value actual_size must be 0 in end state");

      // next in end stays in end.
      check(kv_it_next(h) == IT_END, "next in end must stay end");
      kv_it_destroy(h);
   }

   // kv_it_key/value offset handling: offset == actual_size is the legit
   // "read zero from the tail" boundary; offset > actual_size must be safe.
   [[sysio::action]]
   void offsa() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      const char v[] = "abcdef"; // 7 bytes incl nul
      kv_set(tid_offsa, self, k, sizeof(k), v, sizeof(v));

      char     marker = 0;
      char     buf[8] = {};
      uint32_t h      = kv_it_create(tid_offsa, self, &marker, 0);
      check(kv_it_status(h) == IT_OK, "iter must find the row");

      // offset = actual_size: legal, reads 0 bytes, status stays ok, actual_size reported.
      uint32_t actual = 99;
      int32_t  st     = kv_it_value(h, sizeof(v), buf, sizeof(buf), &actual);
      check(st == IT_OK, "offset=actual_size must keep status ok");
      check(actual == sizeof(v), "actual_size must be reported even at tail offset");

      // offset > actual_size: must not OOB-read.
      actual = 99;
      st     = kv_it_value(h, sizeof(v) + 100, buf, sizeof(buf), &actual);
      check(st == IT_OK, "offset>actual_size must not change status");
      check(actual == sizeof(v), "actual_size must be reported even for over-read");

      // dest_size=0: return size, no write.
      actual = 99;
      st     = kv_it_value(h, 0, &marker, 0, &actual);
      check(st == IT_OK && actual == sizeof(v),
            "dest_size=0 must return actual_size without writing");

      kv_it_destroy(h);
      kv_erase(tid_offsa, k, sizeof(k));
   }

   // ==========================================================================
   // B. Rejection probes (each action calls the problematic intrinsic ONCE and
   //    expects the host to throw, aborting the action).
   // ==========================================================================

   // kv_set with empty key -> kv_key_too_large "KV key must not be empty"
   [[sysio::action]]
   void zkeyset() {
      const uint64_t self = get_self().value;
      char marker = 0;
      kv_set(tid_zkey, self, &marker, 0, "v", 1);
   }

   // kv_erase with empty key -> kv_key_too_large
   [[sysio::action]]
   void zkeyerz() {
      char marker = 0;
      kv_erase(tid_zkey, &marker, 0);
   }

   // kv_set with oversize key -> kv_key_too_large
   [[sysio::action]]
   void bigkey() {
      const uint64_t self = get_self().value;
      static char huge[MAX_KEY_SIZE + 1] = {}; // 257 zero bytes
      kv_set(tid_big, self, huge, sizeof(huge), "v", 1);
   }

   // kv_set with oversize value -> kv_value_too_large
   [[sysio::action]]
   void bigval() {
      const uint64_t self = get_self().value;
      const char k[] = {'B'};
      static char huge[MAX_VALUE_SIZE + 1] = {}; // 256 KiB + 1
      kv_set(tid_big, self, k, sizeof(k), huge, sizeof(huge));
   }

   // kv_idx_store with oversize sec_key -> kv_secondary_key_too_large
   [[sysio::action]]
   void bigsec() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      kv_set(tid_bigsec_pri, self, pk, sizeof(pk), "v", 1);
      static char huge[MAX_SEC_KEY_SIZE + 1] = {}; // 257 bytes
      kv_idx_store(self, tid_bigsec_sec, pk, sizeof(pk), huge, sizeof(huge));
   }

   // kv_idx_store with oversize pri_key -> kv_key_too_large.
   // Distinct SYS_ASSERT site from bigsec (pri_key check vs sec_key check).
   [[sysio::action]]
   void bigpri() {
      const uint64_t self = get_self().value;
      static char huge[MAX_KEY_SIZE + 1] = {}; // 257 bytes
      kv_idx_store(self, tid_bigpri, huge, sizeof(huge), "s", 1);
   }

   // kv_it_create with prefix over the absolute 1024 B limit -> kv_key_too_large
   [[sysio::action]]
   void bigpfx() {
      const uint64_t self = get_self().value;
      static char huge[MAX_PREFIX_LIMIT + 1] = {}; // 1025 bytes
      kv_it_create(tid_big, self, huge, sizeof(huge));
   }

   // table_id > uint16 max -> kv_key_too_large via checked_table_id
   [[sysio::action]]
   void tidmax() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      kv_set(static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1u,
             self, k, sizeof(k), "v", 1);
   }

   // kv_it_status called with reserved-bit-set handle -> kv_invalid_iterator
   [[sysio::action]]
   void badh() {
      kv_it_status(0xDEADBEEFu); // bits outside slot-mask|tag-bit are set
   }

   // Primary kv_it_next called on a secondary handle -> kv_invalid_iterator
   [[sysio::action]]
   void crshp() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      kv_set(tid_crossh_pri, self, pk, sizeof(pk), "v", 1);
      kv_idx_store(self, tid_crossh_sec, pk, sizeof(pk), "s", 1);

      int32_t sh = kv_idx_find_secondary(self, tid_crossh_sec, "s", 1);
      check(sh >= 0, "secondary handle required to probe cross-pool rejection");
      // Cleanup will not run -- kv_it_next must throw here.
      kv_it_next(static_cast<uint32_t>(sh));
   }

   // Secondary kv_idx_next called on a primary handle -> kv_invalid_iterator
   [[sysio::action]]
   void crshs() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      kv_set(tid_crossh_pri, self, k, sizeof(k), "v", 1);
      char marker = 0;
      uint32_t ph = kv_it_create(tid_crossh_pri, self, &marker, 0);
      // kv_idx_next expects a secondary handle -> must reject.
      kv_idx_next(ph);
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
      kv_idx_update(self, tid_idxupbig, dummy_pri, sizeof(dummy_pri),
                    "a", 1, huge, sizeof(huge));
   }

   // kv_it_status on a handle whose slot was released -> kv_invalid_iterator.
   // kv_handle_check_reserved_zero passes (handle has clean bits); the slot
   // pool's in_use check catches it.
   [[sysio::action]]
   void uaf() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      kv_set(tid_uaf, self, k, sizeof(k), "v", 1);
      char marker = 0;
      uint32_t h = kv_it_create(tid_uaf, self, &marker, 0);
      kv_it_destroy(h);
      // Use after destroy: the handle is numerically valid but its slot is not in use.
      kv_it_status(h);
   }

   // kv_it_destroy called twice on the same handle -> kv_invalid_iterator on the
   // second call (slot already released).
   [[sysio::action]]
   void dbldest() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      kv_set(tid_dbldest, self, k, sizeof(k), "v", 1);
      char marker = 0;
      uint32_t h = kv_it_create(tid_dbldest, self, &marker, 0);
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
      int64_t id = kv_set(tid_alias, self, buf, 4, buf + 2, 5);
      check(id >= 0, "kv_set with aliased key/value must succeed");

      char out[8] = {};
      int32_t sz = kv_get(tid_alias, self, buf, 4, out, sizeof(out));
      check(sz == 5, "stored value size must equal original span length");
      // Original key bytes survive whatever copy order the host chose.
      check(out[0] == 'c' && out[1] == 'd' && out[2] == 'e' && out[3] == 'f' && out[4] == 'g',
            "aliased value bytes must round-trip unchanged");
      kv_erase(tid_alias, buf, 4);
   }

   // Exhausts the 1024-slot primary iterator pool: the 1025th kv_it_create
   // must throw kv_iterator_limit_exceeded. Uses a single stored row so every
   // create positions successfully and reserves a slot (iterators in end state
   // still consume a slot).
   [[sysio::action]]
   void itlimit() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      kv_set(tid_payzero /* reuse unrelated tid */, self, k, sizeof(k), "v", 1);
      char marker = 0;
      // Allocate all 1024 slots.
      for (uint32_t i = 0; i < 1024; ++i) {
         uint32_t h = kv_it_create(tid_payzero, self, &marker, 0);
         (void)h;
      }
      // 1025th must trap.
      kv_it_create(tid_payzero, self, &marker, 0);
   }

   // ==========================================================================
   // D. Additional rejection probes (distinct SYS_ASSERT sites).
   // ==========================================================================

   // kv_erase of a key that has never been stored -> kv_key_not_found
   // (kv_erase's itr-not-end guard, distinct from the empty-key guard zkeyerz hits).
   [[sysio::action]]
   void erasmis() {
      const char k[] = {'M'};
      kv_erase(tid_erasmis, k, sizeof(k));
   }

   // kv_idx_remove of a non-existent entry -> kv_key_not_found
   // (kv_idx_remove's itr-not-end guard). (sec_key, pri_key) pair never stored.
   [[sysio::action]]
   void idxrmmis() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      kv_set(tid_idxrm_pri, self, pk, sizeof(pk), "v", 1);
      // Remove an entry that was never stored.
      kv_idx_remove(tid_idxrm_sec, pk, sizeof(pk), "s", 1);
   }

   // kv_idx_update of a non-existent entry -> kv_key_not_found
   // (kv_idx_update's itr-not-end guard).
   [[sysio::action]]
   void idxupmis() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      kv_set(tid_idxup_pri, self, pk, sizeof(pk), "v", 1);
      kv_idx_update(self, tid_idxup_sec, pk, sizeof(pk), "old", 3, "new", 3);
   }

   // ==========================================================================
   // E. Additional accepted-behavior probes.
   // ==========================================================================

   // kv_idx_find_secondary miss returns -1 and allocates NO slot. Spamming
   // misses must not exhaust the iterator pool.
   [[sysio::action]]
   void findmis() {
      const uint64_t self = get_self().value;
      // 2x the slot pool so a leak would blow up (1024 slots; try 2048).
      for (uint32_t i = 0; i < 2048; ++i) {
         int32_t h = kv_idx_find_secondary(self, tid_findmis, "nope", 4);
         check(h == -1, "find_secondary miss must return -1");
      }
   }

   // kv_idx_lower_bound past-end-of-table with non-empty table returns a valid
   // end-state handle so the caller can kv_idx_prev back into the last entry.
   [[sysio::action]]
   void lbpast() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      kv_set(tid_lbpast_pri, self, pk, sizeof(pk), "v", 1);
      kv_idx_store(self, tid_lbpast_sec, pk, sizeof(pk), "m", 1);

      // Seek past the only entry.
      int32_t h = kv_idx_lower_bound(self, tid_lbpast_sec, "z", 1);
      check(h >= 0, "lower_bound past-end in non-empty table must return a valid handle");
      // kv_it_status rejects secondary handles; use kv_idx_next to observe end
      // state (next from end stays at end).
      check(kv_idx_next(static_cast<uint32_t>(h)) == IT_END,
            "handle must be in end state (next from end must stay end)");

      // prev from end into the last entry.
      check(kv_idx_prev(static_cast<uint32_t>(h)) == IT_OK,
            "prev from end must land on last entry");
      char     buf[4] = {};
      uint32_t actual = 99;
      kv_idx_key(static_cast<uint32_t>(h), 0, buf, sizeof(buf), &actual);
      check(actual == 1 && buf[0] == 'm',
            "last entry sec_key must be the one inserted");
      kv_idx_destroy(static_cast<uint32_t>(h));

      // Cleanup.
      kv_idx_remove(tid_lbpast_sec, pk, sizeof(pk), "m", 1);
      kv_erase(tid_lbpast_pri, pk, sizeof(pk));
   }

   // kv_idx_lower_bound on an empty (code, table_id) returns -1; no slot allocated.
   [[sysio::action]]
   void lbempty() {
      const uint64_t self = get_self().value;
      // 2x slot pool to prove no leak on every call.
      for (uint32_t i = 0; i < 2048; ++i) {
         int32_t h = kv_idx_lower_bound(self, tid_lbempty, "x", 1);
         check(h == -1, "lower_bound on empty (code, table_id) must return -1");
      }
   }

   // kv_idx_primary_key on a secondary iterator after the referenced primary
   // row is erased mid-txn: master stores pri_key bytes inline on the secondary
   // entry, so the lookup returns IT_OK with the original pri_key bytes -- the
   // host does NOT auto-invalidate dangling references. Auto-cascade would be a
   // protocol change. Pinning the no-cascade behavior so a future refactor
   // that swaps inline pri_key bytes for a chainbase-id lookup would fail here.
   [[sysio::action]]
   void prmera() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      kv_set(tid_prmera_pri, self, pk, sizeof(pk), "v", 1);
      kv_idx_store(self, tid_prmera_sec, pk, sizeof(pk), "s", 1);

      int32_t h = kv_idx_find_secondary(self, tid_prmera_sec, "s", 1);
      check(h >= 0, "find_secondary must return a valid handle");

      // Erase primary, leaving the secondary entry's pri_key bytes intact.
      kv_erase(tid_prmera_pri, pk, sizeof(pk));

      char     buf[4] = {};
      uint32_t actual = 99;
      int32_t  st     = kv_idx_primary_key(static_cast<uint32_t>(h), 0, buf, sizeof(buf), &actual);
      check(st == IT_OK,
            "primary_key after primary erase returns IT_OK (pri_key bytes are stored inline)");
      check(actual == sizeof(pk) && buf[0] == 'P',
            "primary_key returns the original stored pri_key bytes");

      kv_idx_destroy(static_cast<uint32_t>(h));
      kv_idx_remove(tid_prmera_sec, pk, sizeof(pk), "s", 1);
   }

   // kv_it_prev called when iterator is positioned at begin transitions to
   // iterator_end. Symmetric to kv_it_next at end covered in endstat.
   [[sysio::action]]
   void itprvbgn() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      kv_set(tid_itprvbg, self, k, sizeof(k), "v", 1);
      char     marker = 0;
      uint32_t h      = kv_it_create(tid_itprvbg, self, &marker, 0);
      check(kv_it_status(h) == IT_OK, "iter must start at the row");
      // Only one row under this (code, table_id); prev goes to end.
      check(kv_it_prev(h) == IT_END, "prev at begin must transition to end");
      kv_it_destroy(h);
      kv_erase(tid_itprvbg, k, sizeof(k));
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
   uint32_t make_sec_handle_() {
      const uint64_t self = get_self().value;
      const char pk[] = {'X'};
      kv_set(tid_cpool, self, pk, sizeof(pk), "v", 1);
      kv_idx_store(self, tid_cpool_sec, pk, sizeof(pk), "s", 1);
      int32_t h = kv_idx_find_secondary(self, tid_cpool_sec, "s", 1);
      check(h >= 0, "precondition: secondary handle must allocate");
      return static_cast<uint32_t>(h);
   }

   // Helper: returns a live primary handle.
   uint32_t make_pri_handle_() {
      const uint64_t self = get_self().value;
      const char k[] = {'Y'};
      kv_set(tid_cpool, self, k, sizeof(k), "v", 1);
      char marker = 0;
      return kv_it_create(tid_cpool, self, &marker, 0);
   }

   [[sysio::action]] void crshdst()  { kv_it_destroy(make_sec_handle_()); }
   [[sysio::action]] void crshprv()  { kv_it_prev(make_sec_handle_()); }
   [[sysio::action]] void crshlb()   { kv_it_lower_bound(make_sec_handle_(), "k", 1); }
   [[sysio::action]] void crshk()    {
      char b[4]; uint32_t a = 0;
      kv_it_key(make_sec_handle_(), 0, b, sizeof(b), &a);
   }
   // kv_it_value on a secondary handle: master rejects via validate_primary_handle.
   // Distinct SYS_ASSERT from kv_it_next/key/prev/destroy/lower_bound which all
   // route through the same primary-handle check; this one and crshst pin the
   // remaining two sites.
   [[sysio::action]] void crshv()    {
      char b[4]; uint32_t a = 0;
      kv_it_value(make_sec_handle_(), 0, b, sizeof(b), &a);
   }
   [[sysio::action]] void crshst()   { kv_it_status(make_sec_handle_()); }

   [[sysio::action]] void crsiprv()  { kv_idx_prev(make_pri_handle_()); }
   [[sysio::action]] void crsik()    {
      char b[4]; uint32_t a = 0;
      kv_idx_key(make_pri_handle_(), 0, b, sizeof(b), &a);
   }
   [[sysio::action]] void crsipk()   {
      char b[4]; uint32_t a = 0;
      kv_idx_primary_key(make_pri_handle_(), 0, b, sizeof(b), &a);
   }
   [[sysio::action]] void crsidst()  { kv_idx_destroy(make_pri_handle_()); }

   // ==========================================================================
   // H. Resource and invariant probes a malicious contract could abuse.
   // ==========================================================================

   // Secondary iterator pool exhaustion: 1024 successful kv_idx_find_secondary
   // calls each reserve a slot. The 1025th must throw kv_iterator_limit_exceeded.
   // Symmetric to itlimit but on the independent secondary pool.
   [[sysio::action]]
   void secilim() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      kv_set(tid_secilim_p, self, pk, sizeof(pk), "v", 1);
      kv_idx_store(self, tid_secilim_s, pk, sizeof(pk), "s", 1);
      // 1024 slots. Never destroying so the pool fills up.
      for (uint32_t i = 0; i < 1024; ++i) {
         int32_t h = kv_idx_find_secondary(self, tid_secilim_s, "s", 1);
         (void)h;
      }
      // 1025th must trap.
      kv_idx_find_secondary(self, tid_secilim_s, "s", 1);
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
      int64_t id = kv_set(tid_maxok, self, max_key, MAX_KEY_SIZE, max_val, MAX_VALUE_SIZE);
      check(id >= 0, "kv_set with max-size key and value must succeed");
      check(kv_contains(tid_maxok, self, max_key, MAX_KEY_SIZE) == 1, "max-size row must be contained");
      // Spot-check round-trip at two offsets that would catch truncation.
      static char sink[MAX_VALUE_SIZE] = {};
      int32_t sz = kv_get(tid_maxok, self, max_key, MAX_KEY_SIZE, sink, MAX_VALUE_SIZE);
      check(sz == static_cast<int32_t>(MAX_VALUE_SIZE), "kv_get must report full max size");
      check(sink[0] == 'H' && sink[MAX_VALUE_SIZE-1] == 'T', "kv_get must deliver full bytes");
      kv_erase(tid_maxok, max_key, MAX_KEY_SIZE);
   }

   // Dangling-secondary invariant: the host does NOT auto-remove secondary
   // entries when the referenced primary row is erased. A contract that erases
   // a primary without first removing its secondaries leaves state that can
   // still be found, updated, and read. Master stores pri_key bytes inline on
   // the secondary entry; kv_idx_primary_key returns those bytes even after
   // the primary is gone. Pinning this behavior because "auto-cascade" would
   // be a protocol change. A malicious contract can't crash the chain with it
   // but can bloat its own RAM bill with orphan secondaries indefinitely --
   // worth documenting.
   [[sysio::action]]
   void danglng() {
      const uint64_t self = get_self().value;
      const char pk[] = {'P'};
      kv_set(tid_dangl_pri, self, pk, sizeof(pk), "v", 1);
      kv_idx_store(self, tid_dangl_sec, pk, sizeof(pk), "s", 1);
      kv_erase(tid_dangl_pri, pk, sizeof(pk));

      // Dangling secondary still findable.
      int32_t h = kv_idx_find_secondary(self, tid_dangl_sec, "s", 1);
      check(h >= 0, "dangling secondary entry must still be findable");

      // primary_key returns the stored pri_key bytes; host does not check
      // whether the referenced primary row still exists.
      char     buf[4] = {};
      uint32_t actual = 99;
      int32_t  st     = kv_idx_primary_key(static_cast<uint32_t>(h), 0, buf, sizeof(buf), &actual);
      check(st == IT_OK,
            "primary_key on dangling secondary returns IT_OK (no auto-invalidation)");
      check(actual == sizeof(pk) && buf[0] == 'P',
            "primary_key returns the originally stored pri_key bytes");
      kv_idx_destroy(static_cast<uint32_t>(h));

      // Mutation still succeeds: the (receiver, tid, old_sec, pri_key) composite
      // still resolves to a live kv_index_object even though the referenced
      // primary is gone.
      kv_idx_update(self, tid_dangl_sec, pk, sizeof(pk), "s", 1, "s2", 2);
      h = kv_idx_find_secondary(self, tid_dangl_sec, "s2", 2);
      check(h >= 0, "post-update dangling entry must still be findable at new sec_key");
      kv_idx_destroy(static_cast<uint32_t>(h));

      // Cleanup (required, else RAM leaks).
      kv_idx_remove(tid_dangl_sec, pk, sizeof(pk), "s2", 2);
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
      kv_set(tid_prixtab_a, self, pk_a, sizeof(pk_a), "va", 2);

      // Secondary entry in tid_prixtab_s referencing pri_key bytes that live
      // in tid_prixtab_a.
      kv_idx_store(self, tid_prixtab_s, pk_a, sizeof(pk_a), "s", 1);

      int32_t h = kv_idx_find_secondary(self, tid_prixtab_s, "s", 1);
      check(h >= 0, "cross-table pri_key reference must succeed");

      // primary_key returns the stored pri_key bytes verbatim.
      char     buf[4] = {};
      uint32_t actual = 99;
      int32_t  st     = kv_idx_primary_key(static_cast<uint32_t>(h), 0, buf, sizeof(buf), &actual);
      check(st == IT_OK && actual == sizeof(pk_a) && buf[0] == 'A',
            "cross-table primary_key must return the stored pri_key bytes");
      kv_idx_destroy(static_cast<uint32_t>(h));

      // Cleanup.
      kv_idx_remove(tid_prixtab_s, pk_a, sizeof(pk_a), "s", 1);
      kv_erase(tid_prixtab_a, pk_a, sizeof(pk_a));
   }

   // Inline actions get a fresh apply_context with independent iterator pools.
   // A handle allocated by the parent action is meaningless to the inline
   // child. Pinning this isolation so a future "optimization" that shares
   // pools across the action stack would fail here. The parent sends the
   // inline, then returns; the inline calls kv_it_status(0) -- it has no
   // allocated slots of its own, so the slot-pool in_use check throws.
   [[sysio::action]]
   void inlparnt() {
      const uint64_t self = get_self().value;
      const char k[] = {'K'};
      kv_set(tid_inliso, self, k, sizeof(k), "v", 1);
      char marker = 0;
      uint32_t h = kv_it_create(tid_inliso, self, &marker, 0);
      check(h == 0, "precondition: first primary alloc must be slot 0");

      // Queue the inline child. It runs in its own apply_context after this
      // action returns, where handle 0 is not in use -> kv_it_status throws.
      sysio::action(
         std::vector<sysio::permission_level>{{get_self(), "active"_n}},
         get_self(),
         "inlchild"_n,
         std::vector<char>{}
      ).send();

      // Clean up in the parent context so the failure comes from the child.
      kv_it_destroy(h);
      kv_erase(tid_inliso, k, sizeof(k));
   }

   [[sysio::action]]
   void inlchild() {
      // Fresh apply_context: no iterator slots allocated yet. kv_it_status on
      // slot 0 must throw kv_invalid_iterator via the pool's in_use check.
      kv_it_status(0);
   }

   // kv_set with payer=0 bills the receiver.
   [[sysio::action]]
   void payzero() {
      // Intentionally pass 0 for payer to exercise the default-to-receiver path.
      const char k[] = {'K'};
      int64_t id = kv_set(tid_payzero, /*payer=*/0, k, sizeof(k), "v", 1);
      check(id >= 0, "kv_set(payer=0) must succeed and bill the receiver");
      kv_erase(tid_payzero, k, sizeof(k));
   }
};
