// Comprehensive KV intrinsic test contract.
// Tests ALL 24 KV host functions through WASM.
// Replaces legacy test_api_db / test_api_multi_index for KV coverage.

#include <sysio/sysio.hpp>
#include <sysio/kv_multi_index.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_mapping.hpp>
#include <cstring>

// ── Hand-written KV intrinsic declarations (WASM imports) ──────────────────────
extern "C" {
   // Primary KV operations (5)
   __attribute__((sysio_wasm_import))
   int64_t kv_set(uint32_t key_format, uint64_t payer, const void* key, uint32_t key_size,
                  const void* value, uint32_t value_size);

   __attribute__((sysio_wasm_import))
   int32_t kv_get(uint64_t code, const void* key, uint32_t key_size,
                  void* value, uint32_t value_size);

   __attribute__((sysio_wasm_import))
   int64_t kv_erase(const void* key, uint32_t key_size);

   __attribute__((sysio_wasm_import))
   int32_t kv_contains(uint64_t code, const void* key, uint32_t key_size);

   // Primary iterators (8)
   __attribute__((sysio_wasm_import))
   uint32_t kv_it_create(uint64_t code, const void* prefix, uint32_t prefix_size);

   __attribute__((sysio_wasm_import))
   void kv_it_destroy(uint32_t handle);

   __attribute__((sysio_wasm_import))
   int32_t kv_it_status(uint32_t handle);

   __attribute__((sysio_wasm_import))
   int32_t kv_it_next(uint32_t handle);

   __attribute__((sysio_wasm_import))
   int32_t kv_it_prev(uint32_t handle);

   __attribute__((sysio_wasm_import))
   int32_t kv_it_lower_bound(uint32_t handle, const void* key, uint32_t key_size);

   __attribute__((sysio_wasm_import))
   int32_t kv_it_key(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size,
                     uint32_t* actual_size);

   __attribute__((sysio_wasm_import))
   int32_t kv_it_value(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size,
                       uint32_t* actual_size);

   // Secondary index operations (11)
   __attribute__((sysio_wasm_import))
   void kv_idx_store(uint64_t table, uint32_t index_id,
                     const void* sec_key, uint32_t sec_key_size,
                     const void* pri_key, uint32_t pri_key_size);

   __attribute__((sysio_wasm_import))
   void kv_idx_remove(uint64_t table, uint32_t index_id,
                      const void* sec_key, uint32_t sec_key_size,
                      const void* pri_key, uint32_t pri_key_size);

   __attribute__((sysio_wasm_import))
   void kv_idx_update(uint64_t table, uint32_t index_id,
                      const void* old_sec_key, uint32_t old_sec_key_size,
                      const void* new_sec_key, uint32_t new_sec_key_size,
                      const void* pri_key, uint32_t pri_key_size);

   __attribute__((sysio_wasm_import))
   uint32_t kv_idx_find_secondary(uint64_t code, uint64_t table, uint32_t index_id,
                                  const void* sec_key, uint32_t sec_key_size);

   __attribute__((sysio_wasm_import))
   uint32_t kv_idx_lower_bound(uint64_t code, uint64_t table, uint32_t index_id,
                               const void* sec_key, uint32_t sec_key_size);

   __attribute__((sysio_wasm_import))
   int32_t kv_idx_next(uint32_t handle);

   __attribute__((sysio_wasm_import))
   int32_t kv_idx_prev(uint32_t handle);

   __attribute__((sysio_wasm_import))
   int32_t kv_idx_key(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size,
                      uint32_t* actual_size);

   __attribute__((sysio_wasm_import))
   int32_t kv_idx_primary_key(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size,
                              uint32_t* actual_size);

   __attribute__((sysio_wasm_import))
   void kv_idx_destroy(uint32_t handle);
}

using namespace sysio;

// ── Helpers ────────────────────────────────────────────────────────────────────

// Encode uint64_t as 8 bytes big-endian (ensures lexicographic == numeric order)
static void encode_u64(uint64_t v, char* buf) {
   for (int i = 7; i >= 0; --i) {
      buf[i] = static_cast<char>(v & 0xFF);
      v >>= 8;
   }
}

// Build a prefixed key: [prefix_byte | payload...]
static void make_prefixed_key(uint8_t prefix, const void* payload, uint32_t payload_sz,
                              char* out, uint32_t* out_sz) {
   out[0] = static_cast<char>(prefix);
   if (payload_sz > 0)
      memcpy(out + 1, payload, payload_sz);
   *out_sz = 1 + payload_sz;
}

// kv_set wrapper using key_format=0 (raw) and self as payer
static int64_t kv_put(uint64_t self, const void* key, uint32_t key_size,
                      const void* value, uint32_t value_size) {
   return kv_set(0 /*raw*/, self, key, key_size, value, value_size);
}

// ── Contract ───────────────────────────────────────────────────────────────────

class [[sysio::contract("test_kv_api")]] test_kv_api : public contract {
public:
   using contract::contract;

   // ─── 1. testkvstord: kv_set + kv_get round-trip ────────────────────────
   [[sysio::action]]
   void testkvstord() {
      uint64_t self = get_self().value;
      const char key[] = {0x01, 0x00, 0x01};
      const char val[] = "hello_kv";
      kv_put(self, key, sizeof(key), val, sizeof(val));

      char buf[64] = {};
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == (int32_t)sizeof(val), "kv_get size mismatch");
      check(memcmp(buf, val, sizeof(val)) == 0, "kv_get data mismatch");
   }

   // ─── 2. testkvupdate: create then overwrite ─────────────────────────────
   [[sysio::action]]
   void testkvupdate() {
      uint64_t self = get_self().value;
      const char key[] = {0x02, 0x00, 0x01};
      const char v1[] = "original";
      const char v2[] = "updated_value";
      kv_put(self, key, sizeof(key), v1, sizeof(v1));
      kv_put(self, key, sizeof(key), v2, sizeof(v2));

      char buf[64] = {};
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == (int32_t)sizeof(v2), "update: size mismatch");
      check(memcmp(buf, v2, sizeof(v2)) == 0, "update: data mismatch");
   }

   // ─── 3. testkverase: set, erase, get returns -1 ─────────────────────────
   [[sysio::action]]
   void testkverase() {
      uint64_t self = get_self().value;
      const char key[] = {0x03, 0x00, 0x01};
      const char val[] = "to_erase";
      kv_put(self, key, sizeof(key), val, sizeof(val));

      kv_erase(key, sizeof(key));

      char buf[64];
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == -1, "erase: kv_get should return -1");
   }

   // ─── 4. testkvexist: contains check ────────────────────────────────────
   [[sysio::action]]
   void testkvexist() {
      uint64_t self = get_self().value;
      const char key[] = {0x04, 0x00, 0x01};
      const char val[] = "exists";
      kv_put(self, key, sizeof(key), val, sizeof(val));
      check(kv_contains(self, key, sizeof(key)) == 1, "contains: should be 1");

      kv_erase(key, sizeof(key));
      check(kv_contains(self, key, sizeof(key)) == 0, "contains: should be 0 after erase");
   }

   // ─── 5. testkvsetbt: partial write at offset ───────────────────────────
   [[sysio::action]]
   void testkvsetbt() {
      uint64_t self = get_self().value;
      const char key[] = {0x05, 0x00, 0x01};
      const char base_val[] = "AAAAAAAAAA"; // 10 A's + null
      kv_put(self, key, sizeof(key), base_val, 10);

      // Overwrite bytes 3..5 with "XYZ" using read-modify-write
      char buf[64] = {};
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 10, "setbytes: size should be 10");
      buf[3] = 'X'; buf[4] = 'Y'; buf[5] = 'Z';
      kv_put(self, key, sizeof(key), buf, 10);

      char buf2[64] = {};
      kv_get(self, key, sizeof(key), buf2, sizeof(buf2));
      check(buf2[0] == 'A', "setbytes: byte 0");
      check(buf2[3] == 'X', "setbytes: byte 3");
      check(buf2[4] == 'Y', "setbytes: byte 4");
      check(buf2[5] == 'Z', "setbytes: byte 5");
      check(buf2[6] == 'A', "setbytes: byte 6");
   }

   // ─── 6. testitcreate: create iterator, check status, destroy ────────────
   [[sysio::action]]
   void testitcreate() {
      uint64_t self = get_self().value;
      // Seed one row so the iterator has something
      const char key[] = {0x06, 0x00, 0x01};
      const char val[] = "row";
      kv_put(self, key, sizeof(key), val, sizeof(val));

      const char prefix[] = {0x06};
      uint32_t handle = kv_it_create(self, prefix, 1);
      int32_t status = kv_it_status(handle);
      // After create, iterator is positioned before the first element (status = 0 ok or 2 end)
      // After lower_bound to first key, should be on a valid row
      kv_it_lower_bound(handle, key, sizeof(key));
      status = kv_it_status(handle);
      check(status == 0, "it_create: status should be 0 (ok) after lower_bound to existing key");
      kv_it_destroy(handle);
   }

   // ─── 7. testitnext: forward iteration over 5 rows ───────────────────────
   [[sysio::action]]
   void testitnext() {
      uint64_t self = get_self().value;
      // Insert 5 rows with prefix 0x07
      for (uint64_t i = 0; i < 5; ++i) {
         char key[9];
         key[0] = 0x07;
         encode_u64(i, key + 1);
         char val[8];
         encode_u64(i * 10, val);
         kv_put(self, key, 9, val, 8);
      }

      const char prefix[] = {0x07};
      uint32_t handle = kv_it_create(self, prefix, 1);

      // Seek to beginning of prefix
      char seek_key[] = {0x07, 0,0,0,0,0,0,0,0};
      kv_it_lower_bound(handle, seek_key, 9);

      uint32_t count = 0;
      uint64_t prev_id = 0;
      bool first = true;
      while (kv_it_status(handle) == 0) {
         // Read the key to verify ordering
         char kbuf[16];
         uint32_t klen = 0;
         kv_it_key(handle, 0, kbuf, sizeof(kbuf), &klen);
         check(klen == 9, "it_next: key size should be 9");

         // Decode the id from key bytes [1..8]
         uint64_t id = 0;
         for (int j = 1; j <= 8; ++j)
            id = (id << 8) | (uint8_t)kbuf[j];

         if (!first) {
            check(id > prev_id, "it_next: keys must be ascending");
         }
         prev_id = id;
         first = false;
         ++count;
         kv_it_next(handle);
      }
      check(count == 5, "it_next: should iterate 5 rows");
      kv_it_destroy(handle);
   }

   // ─── 8. testitprev: backward iteration ──────────────────────────────────
   [[sysio::action]]
   void testitprev() {
      uint64_t self = get_self().value;
      // Insert 5 rows with prefix 0x08
      for (uint64_t i = 0; i < 5; ++i) {
         char key[9];
         key[0] = 0x08;
         encode_u64(i, key + 1);
         char val[8];
         encode_u64(i, val);
         kv_put(self, key, 9, val, 8);
      }

      const char prefix[] = {0x08};
      uint32_t handle = kv_it_create(self, prefix, 1);

      // Seek past the last key in prefix: lower_bound on prefix+1
      // (prefix 0x09 is beyond all 0x08 keys)
      char past_key[] = {0x09, 0,0,0,0,0,0,0,0};
      kv_it_lower_bound(handle, past_key, 1);

      // Now prev should land on the last 0x08 key
      kv_it_prev(handle);

      uint32_t count = 0;
      uint64_t prev_id = 99;
      while (kv_it_status(handle) == 0) {
         char kbuf[16];
         uint32_t klen = 0;
         kv_it_key(handle, 0, kbuf, sizeof(kbuf), &klen);
         check(klen == 9, "it_prev: key size should be 9");

         uint64_t id = 0;
         for (int j = 1; j <= 8; ++j)
            id = (id << 8) | (uint8_t)kbuf[j];

         if (count > 0) {
            check(id < prev_id, "it_prev: keys must be descending");
         }
         prev_id = id;
         ++count;
         kv_it_prev(handle);
      }
      check(count == 5, "it_prev: should iterate 5 rows backward");
      kv_it_destroy(handle);
   }

   // ─── 9. testitlbound: lower_bound seek ──────────────────────────────────
   [[sysio::action]]
   void testitlbound() {
      uint64_t self = get_self().value;
      // Insert keys: 0x09 + {10, 20, 30, 40, 50}
      uint64_t ids[] = {10, 20, 30, 40, 50};
      for (auto id : ids) {
         char key[9];
         key[0] = 0x09;
         encode_u64(id, key + 1);
         kv_put(self, key, 9, "v", 1);
      }

      const char prefix[] = {0x09};
      uint32_t handle = kv_it_create(self, prefix, 1);

      // lower_bound on key 25 -> should land on 30
      char seek[9];
      seek[0] = 0x09;
      encode_u64(25, seek + 1);
      kv_it_lower_bound(handle, seek, 9);

      check(kv_it_status(handle) == 0, "lb: should be on valid row");

      char kbuf[16];
      uint32_t klen = 0;
      kv_it_key(handle, 0, kbuf, sizeof(kbuf), &klen);
      uint64_t found_id = 0;
      for (int j = 1; j <= 8; ++j)
         found_id = (found_id << 8) | (uint8_t)kbuf[j];
      check(found_id == 30, "lb: should land on 30");

      kv_it_destroy(handle);
   }

   // ─── 10. testitkey: verify key data from iterator ────────────────────────
   [[sysio::action]]
   void testitkey() {
      uint64_t self = get_self().value;
      char key[9];
      key[0] = 0x0A;
      encode_u64(42, key + 1);
      kv_put(self, key, 9, "val42", 5);

      const char prefix[] = {0x0A};
      uint32_t handle = kv_it_create(self, prefix, 1);
      kv_it_lower_bound(handle, key, 9);

      char dest[16];
      uint32_t actual = 0;
      int32_t status = kv_it_key(handle, 0, dest, sizeof(dest), &actual);
      check(status == 0, "it_key: status should be 0");
      check(actual == 9, "it_key: actual size should be 9");
      check(memcmp(dest, key, 9) == 0, "it_key: data mismatch");

      kv_it_destroy(handle);
   }

   // ─── 11. testitvalue: verify value data from iterator ────────────────────
   [[sysio::action]]
   void testitvalue() {
      uint64_t self = get_self().value;
      char key[9];
      key[0] = 0x0B;
      encode_u64(77, key + 1);
      const char val[] = "value_seventy_seven";
      kv_put(self, key, 9, val, sizeof(val));

      const char prefix[] = {0x0B};
      uint32_t handle = kv_it_create(self, prefix, 1);
      kv_it_lower_bound(handle, key, 9);

      char dest[64];
      uint32_t actual = 0;
      int32_t status = kv_it_value(handle, 0, dest, sizeof(dest), &actual);
      check(status == 0, "it_value: status should be 0");
      check(actual == sizeof(val), "it_value: size mismatch");
      check(memcmp(dest, val, sizeof(val)) == 0, "it_value: data mismatch");

      kv_it_destroy(handle);
   }

   // ─── 12. testitubound: upper_bound simulation ────────────────────────────
   [[sysio::action]]
   void testitubound() {
      uint64_t self = get_self().value;
      // Insert keys: 0x0C + {10, 20, 30}
      uint64_t ids[] = {10, 20, 30};
      for (auto id : ids) {
         char key[9];
         key[0] = 0x0C;
         encode_u64(id, key + 1);
         kv_put(self, key, 9, "x", 1);
      }

      const char prefix[] = {0x0C};
      uint32_t handle = kv_it_create(self, prefix, 1);

      // upper_bound(20) = lower_bound(21) -> should land on 30
      char seek[9];
      seek[0] = 0x0C;
      encode_u64(21, seek + 1);
      kv_it_lower_bound(handle, seek, 9);

      check(kv_it_status(handle) == 0, "ub: should be on valid row");

      char kbuf[16];
      uint32_t klen = 0;
      kv_it_key(handle, 0, kbuf, sizeof(kbuf), &klen);
      uint64_t found_id = 0;
      for (int j = 1; j <= 8; ++j)
         found_id = (found_id << 8) | (uint8_t)kbuf[j];
      check(found_id == 30, "ub: should land on 30");

      kv_it_destroy(handle);
   }

   // ─── 13. testidxstore: secondary index store + find ──────────────────────
   [[sysio::action]]
   void testidxstore() {
      uint64_t self = get_self().value;
      uint64_t table = "idxtbl"_n.value;

      const char sec[] = "alice";
      const char pri[] = {0x0D, 0x00, 0x01};
      kv_idx_store(table, 0, sec, 5, pri, 3);

      uint32_t handle = kv_idx_find_secondary(self, table, 0, sec, 5);
      // Check we can read the primary key back
      char pk_buf[16];
      uint32_t pk_sz = 0;
      kv_idx_primary_key(handle, 0, pk_buf, sizeof(pk_buf), &pk_sz);
      check(pk_sz == 3, "idx_store: pri key size");
      check(memcmp(pk_buf, pri, 3) == 0, "idx_store: pri key mismatch");
      kv_idx_destroy(handle);
   }

   // ─── 14. testidxremov: store then remove ─────────────────────────────────
   [[sysio::action]]
   void testidxremov() {
      uint64_t self = get_self().value;
      uint64_t table = "idxrmv"_n.value;

      const char sec[] = "bob";
      const char pri[] = {0x0E, 0x00, 0x01};
      kv_idx_store(table, 0, sec, 3, pri, 3);

      // Verify it exists
      uint32_t h = kv_idx_find_secondary(self, table, 0, sec, 3);
      int32_t st = kv_idx_next(h); // check handle is valid by calling next
      kv_idx_destroy(h);

      // Remove
      kv_idx_remove(table, 0, sec, 3, pri, 3);

      // lower_bound should not find it: status should be end-of-range
      uint32_t h2 = kv_idx_lower_bound(self, table, 0, sec, 3);
      // After removal, lower_bound on the same key should be at end (status != 0)
      // or find a different key. We check via kv_idx_key.
      char sk_buf[16];
      uint32_t sk_sz = 0;
      int32_t s2 = kv_idx_key(h2, 0, sk_buf, sizeof(sk_buf), &sk_sz);
      // If removed, the iterator should be at end, so key read returns status != 0
      check(s2 != 0 || sk_sz != 3 || memcmp(sk_buf, sec, 3) != 0,
            "idx_remove: entry should be gone");
      kv_idx_destroy(h2);
   }

   // ─── 15. testidxupdat: update secondary key ─────────────────────────────
   [[sysio::action]]
   void testidxupdat() {
      uint64_t self = get_self().value;
      uint64_t table = "idxupd"_n.value;

      const char old_sec[] = "charlie";
      const char new_sec[] = "david";
      const char pri[] = {0x0F, 0x00, 0x01};

      kv_idx_store(table, 0, old_sec, 7, pri, 3);
      kv_idx_update(table, 0, old_sec, 7, new_sec, 5, pri, 3);

      // Find by new key
      uint32_t h = kv_idx_find_secondary(self, table, 0, new_sec, 5);
      char pk_buf[16];
      uint32_t pk_sz = 0;
      kv_idx_primary_key(h, 0, pk_buf, sizeof(pk_buf), &pk_sz);
      check(pk_sz == 3, "idx_update: pri key size");
      check(memcmp(pk_buf, pri, 3) == 0, "idx_update: pri key mismatch");
      kv_idx_destroy(h);
   }

   // ─── 16. testidxfind: find by secondary key ─────────────────────────────
   [[sysio::action]]
   void testidxfind() {
      uint64_t self = get_self().value;
      uint64_t table = "idxfnd"_n.value;

      // Store 3 entries
      const char secs[][8] = {"alpha", "beta", "gamma"};
      for (int i = 0; i < 3; ++i) {
         char pri[3] = {0x10, 0x00, (char)(i + 1)};
         kv_idx_store(table, 0, secs[i], (uint32_t)strlen(secs[i]), pri, 3);
      }

      // Find "beta"
      uint32_t h = kv_idx_find_secondary(self, table, 0, "beta", 4);
      char sk_buf[16];
      uint32_t sk_sz = 0;
      kv_idx_key(h, 0, sk_buf, sizeof(sk_buf), &sk_sz);
      check(sk_sz == 4, "idx_find: sec key size");
      check(memcmp(sk_buf, "beta", 4) == 0, "idx_find: sec key mismatch");
      kv_idx_destroy(h);
   }

   // ─── 17. testidxlbnd: lower_bound on secondary index ────────────────────
   [[sysio::action]]
   void testidxlbnd() {
      uint64_t self = get_self().value;
      uint64_t table = "idxlb"_n.value;

      // Store entries with numeric secondary keys: "10", "20", "30"
      for (int i = 1; i <= 3; ++i) {
         char sec[3];
         sec[0] = '0' + (char)i;
         sec[1] = '0';
         sec[2] = '\0';
         char pri[3] = {0x11, 0x00, (char)i};
         kv_idx_store(table, 0, sec, 2, pri, 3);
      }

      // lower_bound("15") should land on "20"
      uint32_t h = kv_idx_lower_bound(self, table, 0, "15", 2);
      char sk_buf[16];
      uint32_t sk_sz = 0;
      kv_idx_key(h, 0, sk_buf, sizeof(sk_buf), &sk_sz);
      check(sk_sz == 2, "idx_lbound: sec key size");
      check(sk_buf[0] == '2' && sk_buf[1] == '0', "idx_lbound: should land on '20'");
      kv_idx_destroy(h);
   }

   // ─── 18. testidxnext: forward iterate secondary index ────────────────────
   [[sysio::action]]
   void testidxnext() {
      uint64_t self = get_self().value;
      uint64_t table = "idxnxt"_n.value;

      const char secs[][4] = {"aaa", "bbb", "ccc"};
      for (int i = 0; i < 3; ++i) {
         char pri[3] = {0x12, 0x00, (char)(i + 1)};
         kv_idx_store(table, 0, secs[i], 3, pri, 3);
      }

      uint32_t h = kv_idx_lower_bound(self, table, 0, "aaa", 3);
      // We should be on "aaa", advance to "bbb"
      int32_t st = kv_idx_next(h);
      check(st == 0, "idx_next: should return 0 (ok)");

      char sk_buf[16];
      uint32_t sk_sz = 0;
      kv_idx_key(h, 0, sk_buf, sizeof(sk_buf), &sk_sz);
      check(sk_sz == 3, "idx_next: key size");
      check(memcmp(sk_buf, "bbb", 3) == 0, "idx_next: should be on bbb");
      kv_idx_destroy(h);
   }

   // ─── 19. testidxprev: backward iterate secondary index ───────────────────
   [[sysio::action]]
   void testidxprev() {
      uint64_t self = get_self().value;
      uint64_t table = "idxprv"_n.value;

      const char secs[][4] = {"xxx", "yyy", "zzz"};
      for (int i = 0; i < 3; ++i) {
         char pri[3] = {0x13, 0x00, (char)(i + 1)};
         kv_idx_store(table, 0, secs[i], 3, pri, 3);
      }

      // Find "zzz", then prev to "yyy"
      uint32_t h = kv_idx_find_secondary(self, table, 0, "zzz", 3);
      int32_t st = kv_idx_prev(h);
      check(st == 0, "idx_prev: should return 0 (ok)");

      char sk_buf[16];
      uint32_t sk_sz = 0;
      kv_idx_key(h, 0, sk_buf, sizeof(sk_buf), &sk_sz);
      check(sk_sz == 3, "idx_prev: key size");
      check(memcmp(sk_buf, "yyy", 3) == 0, "idx_prev: should be on yyy");
      kv_idx_destroy(h);
   }

   // ─── 20. testidxkey: read secondary key from iterator ────────────────────
   [[sysio::action]]
   void testidxkey() {
      uint64_t self = get_self().value;
      uint64_t table = "idxkrd"_n.value;

      const char sec[] = "seckey_data";
      const char pri[] = {0x14, 0x00, 0x01};
      kv_idx_store(table, 0, sec, 11, pri, 3);

      uint32_t h = kv_idx_find_secondary(self, table, 0, sec, 11);
      char dest[32];
      uint32_t actual = 0;
      int32_t st = kv_idx_key(h, 0, dest, sizeof(dest), &actual);
      check(st == 0, "idx_key: status");
      check(actual == 11, "idx_key: size");
      check(memcmp(dest, sec, 11) == 0, "idx_key: data mismatch");
      kv_idx_destroy(h);
   }

   // ─── 21. testidxprik: read primary key from secondary iterator ───────────
   [[sysio::action]]
   void testidxprik() {
      uint64_t self = get_self().value;
      uint64_t table = "idxpri"_n.value;

      const char sec[] = "lookup";
      char pri[9];
      pri[0] = 0x15;
      encode_u64(12345, pri + 1);
      kv_idx_store(table, 0, sec, 6, pri, 9);

      uint32_t h = kv_idx_find_secondary(self, table, 0, sec, 6);
      char pk_buf[16];
      uint32_t pk_sz = 0;
      int32_t st = kv_idx_primary_key(h, 0, pk_buf, sizeof(pk_buf), &pk_sz);
      check(st == 0, "idx_prikey: status");
      check(pk_sz == 9, "idx_prikey: size");
      check(memcmp(pk_buf, pri, 9) == 0, "idx_prikey: data mismatch");
      kv_idx_destroy(h);
   }

   // ─── 22. testcrossrd: cross-contract read ────────────────────────────────
   [[sysio::action]]
   void testcrossrd() {
      uint64_t self = get_self().value;
      const char key[] = {0x16, 0x00, 0x01};
      const char val[] = "cross_contract";
      kv_put(self, key, sizeof(key), val, sizeof(val));

      // Read using own code (should succeed)
      char buf[64];
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == (int32_t)sizeof(val), "crossrd: self read size");
      check(memcmp(buf, val, sizeof(val)) == 0, "crossrd: self read data");

      // Note: cross-contract read with a different code would require
      // deploying to another account. We verify the self-read path here;
      // the test driver handles the cross-contract scenario.
   }

   // ─── 23. testemptyval: empty value ───────────────────────────────────────
   [[sysio::action]]
   void testemptyval() {
      uint64_t self = get_self().value;
      const char key[] = {0x17, 0x00, 0x01};
      // Set with empty value (nullptr, size 0)
      kv_set(0, self, key, sizeof(key), nullptr, 0);

      char buf[16];
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 0, "emptyval: kv_get should return 0 for empty value");
   }

   // ─── 24. testmultikey: various key sizes ─────────────────────────────────
   [[sysio::action]]
   void testmultikey() {
      uint64_t self = get_self().value;

      // 1-byte key
      {
         const char key[] = {0x18};
         const char val[] = "one_byte_key";
         kv_put(self, key, 1, val, sizeof(val));
         char buf[32];
         int32_t sz = kv_get(self, key, 1, buf, sizeof(buf));
         check(sz == (int32_t)sizeof(val), "multikey: 1B size");
         check(memcmp(buf, val, sizeof(val)) == 0, "multikey: 1B data");
      }

      // 8-byte key
      {
         char key[8];
         encode_u64(0x1900000000000001ULL, key);
         const char val[] = "eight_byte_key";
         kv_put(self, key, 8, val, sizeof(val));
         char buf[32];
         int32_t sz = kv_get(self, key, 8, buf, sizeof(buf));
         check(sz == (int32_t)sizeof(val), "multikey: 8B size");
         check(memcmp(buf, val, sizeof(val)) == 0, "multikey: 8B data");
      }

      // 24-byte key (SSO boundary)
      {
         char key[24];
         memset(key, 0x1A, 24);
         const char val[] = "twentyfour_byte_key";
         kv_put(self, key, 24, val, sizeof(val));
         char buf[32];
         int32_t sz = kv_get(self, key, 24, buf, sizeof(buf));
         check(sz == (int32_t)sizeof(val), "multikey: 24B size");
         check(memcmp(buf, val, sizeof(val)) == 0, "multikey: 24B data");
      }

      // 100-byte key (heap path)
      {
         char key[100];
         memset(key, 0x1B, 100);
         const char val[] = "hundred_byte_key";
         kv_put(self, key, 100, val, sizeof(val));
         char buf[32];
         int32_t sz = kv_get(self, key, 100, buf, sizeof(buf));
         check(sz == (int32_t)sizeof(val), "multikey: 100B size");
         check(memcmp(buf, val, sizeof(val)) == 0, "multikey: 100B data");
      }
   }

   // ─── 25. testnested: complex nested struct round-trip ────────────────────
   [[sysio::action]]
   void testnested() {
      uint64_t self = get_self().value;

      // Build a structured value: [count(4) | entry0 | entry1 | ...]
      // Each entry: [name_len(4) | name_bytes | score(8)]
      // We'll pack 3 entries manually.
      struct entry {
         const char* name;
         uint32_t name_len;
         uint64_t score;
      };
      entry entries[] = {
         {"alice",   5, 100},
         {"bob",     3, 200},
         {"charlie", 7, 300}
      };

      char val_buf[256];
      uint32_t pos = 0;

      // Write count
      uint32_t count = 3;
      memcpy(val_buf + pos, &count, 4); pos += 4;

      for (auto& e : entries) {
         memcpy(val_buf + pos, &e.name_len, 4); pos += 4;
         memcpy(val_buf + pos, e.name, e.name_len); pos += e.name_len;
         memcpy(val_buf + pos, &e.score, 8); pos += 8;
      }

      const char key[] = {0x1C, 0x00, 0x01};
      kv_put(self, key, sizeof(key), val_buf, pos);

      // Read back and verify
      char read_buf[256];
      int32_t sz = kv_get(self, key, sizeof(key), read_buf, sizeof(read_buf));
      check(sz == (int32_t)pos, "nested: total size mismatch");

      uint32_t rpos = 0;
      uint32_t rcount = 0;
      memcpy(&rcount, read_buf + rpos, 4); rpos += 4;
      check(rcount == 3, "nested: count mismatch");

      for (uint32_t i = 0; i < rcount; ++i) {
         uint32_t nlen = 0;
         memcpy(&nlen, read_buf + rpos, 4); rpos += 4;
         check(nlen == entries[i].name_len, "nested: name_len mismatch");
         check(memcmp(read_buf + rpos, entries[i].name, nlen) == 0, "nested: name mismatch");
         rpos += nlen;
         uint64_t score = 0;
         memcpy(&score, read_buf + rpos, 8); rpos += 8;
         check(score == entries[i].score, "nested: score mismatch");
      }
   }

   // ═══════════════════════════════════════════════════════════════════════════
   // Additional edge-case tests (26–51)
   // ═══════════════════════════════════════════════════════════════════════════

   // ─── 26. testitdestr: destroy iterator, verify handle invalid, create new ─
   [[sysio::action]]
   void testitdestr() {
      uint64_t self = get_self().value;
      const char key[] = {0x20, 0x00, 0x01};
      kv_put(self, key, sizeof(key), "val", 3);

      const char prefix[] = {0x20};
      uint32_t h = kv_it_create(self, prefix, 1);
      kv_it_destroy(h);

      // After destroy, the handle slot is freed.
      // Create a new iterator — should succeed (reuses the freed slot or another).
      uint32_t h2 = kv_it_create(self, prefix, 1);
      kv_it_lower_bound(h2, key, sizeof(key));
      check(kv_it_status(h2) == 0, "itdestr: new iterator should work");
      kv_it_destroy(h2);
   }

   // ─── 27. testitreuse: create/destroy cycle, verify handle reuse ───────────
   [[sysio::action]]
   void testitreuse() {
      uint64_t self = get_self().value;
      const char key[] = {0x21, 0x00, 0x01};
      kv_put(self, key, sizeof(key), "v", 1);

      const char prefix[] = {0x21};

      // Allocate and free 5 times, all should succeed
      for (int i = 0; i < 5; ++i) {
         uint32_t h = kv_it_create(self, prefix, 1);
         kv_it_lower_bound(h, key, sizeof(key));
         check(kv_it_status(h) == 0, "itreuse: iterator should be valid");
         kv_it_destroy(h);
      }
   }

   // ─── 28. tsterasedinv: erase row under iterator, verify erased status ───
   [[sysio::action]]
   void tsterasedinv() {
      uint64_t self = get_self().value;
      const char key1[] = {0x22, 0x00, 0x01};
      const char key2[] = {0x22, 0x00, 0x02};
      kv_put(self, key1, sizeof(key1), "a", 1);
      kv_put(self, key2, sizeof(key2), "b", 1);

      const char prefix[] = {0x22};
      uint32_t h = kv_it_create(self, prefix, 1);
      kv_it_lower_bound(h, key1, sizeof(key1));
      check(kv_it_status(h) == 0, "iterasedinv: on key1");

      // Erase key1 while iterator points to it
      kv_erase(key1, sizeof(key1));

      // kv_it_key should detect the erasure and return status 2 (erased)
      char kbuf[16];
      uint32_t klen = 0;
      int32_t st = kv_it_key(h, 0, kbuf, sizeof(kbuf), &klen);
      check(st == 2, "iterasedinv: should return erased status (2)");

      // kv_it_next should still advance past the erased row to key2
      kv_it_next(h);
      check(kv_it_status(h) == 0, "iterasedinv: should advance to key2");

      char kbuf2[16];
      uint32_t klen2 = 0;
      kv_it_key(h, 0, kbuf2, sizeof(kbuf2), &klen2);
      check(klen2 == sizeof(key2), "iterasedinv: key2 size");
      check(memcmp(kbuf2, key2, sizeof(key2)) == 0, "iterasedinv: key2 data");

      kv_it_destroy(h);
   }

   // ─── 29. tstitexhaust: allocate 16 iterators (max pool) ──────────────────
   //     The 17th allocation must fail — tested from the host side
   [[sysio::action]]
   void tstitexhaust() {
      uint64_t self = get_self().value;
      const char prefix[] = {0x23};

      // Allocate all 16 slots
      uint32_t handles[16];
      for (int i = 0; i < 16; ++i) {
         handles[i] = kv_it_create(self, prefix, 1);
      }

      // Clean up all
      for (int i = 0; i < 16; ++i) {
         kv_it_destroy(handles[i]);
      }
   }

   // ─── 30. tstitexhfail: try to allocate 17th iterator (should abort) ──────
   [[sysio::action]]
   void tstitexhfail() {
      uint64_t self = get_self().value;
      const char prefix[] = {0x24};

      // Allocate all 16 slots
      for (int i = 0; i < 16; ++i) {
         kv_it_create(self, prefix, 1);
      }
      // 17th should fail — this line triggers the exception
      kv_it_create(self, prefix, 1);
      // Should not reach here
      check(false, "itexhfail: should have thrown");
   }

   // ─── 31. testitprefix: iterator only sees matching prefix ─────────────────
   [[sysio::action]]
   void testitprefix() {
      uint64_t self = get_self().value;
      // Insert keys with prefix 0x25 and 0x26
      const char k1[] = {0x25, 0x00, 0x01};
      const char k2[] = {0x25, 0x00, 0x02};
      const char k3[] = {0x26, 0x00, 0x01};  // different prefix
      kv_put(self, k1, sizeof(k1), "a", 1);
      kv_put(self, k2, sizeof(k2), "b", 1);
      kv_put(self, k3, sizeof(k3), "c", 1);

      const char prefix[] = {0x25};
      uint32_t h = kv_it_create(self, prefix, 1);

      // Seek to start of prefix
      char seek[] = {0x25, 0x00, 0x00};
      kv_it_lower_bound(h, seek, sizeof(seek));

      uint32_t count = 0;
      while (kv_it_status(h) == 0) {
         // Verify key starts with 0x25
         char kbuf[16];
         uint32_t klen = 0;
         kv_it_key(h, 0, kbuf, sizeof(kbuf), &klen);
         check(kbuf[0] == 0x25, "itprefix: key should start with 0x25");
         ++count;
         kv_it_next(h);
      }
      check(count == 2, "itprefix: should see exactly 2 rows");
      kv_it_destroy(h);
   }

   // ─── 32. tstitempttbl: iterator on empty prefix, status != ok immediately ─
   [[sysio::action]]
   void tstitempttbl() {
      uint64_t self = get_self().value;
      // Use prefix 0x27 — never populated
      const char prefix[] = {0x27};
      uint32_t h = kv_it_create(self, prefix, 1);
      // Should be at end immediately since no rows match
      check(kv_it_status(h) == 1, "itempttbl: status should be 1 (end)");
      kv_it_destroy(h);
   }

   // ─── 33. tstwriteperm: kv_set with key_format=1 and payer field ──────────
   [[sysio::action]]
   void tstwriteperm() {
      uint64_t self = get_self().value;
      // Use key_format=1 (standard 24B) with a valid payer
      char key[24];
      memset(key, 0, 24);
      key[0] = 0x28;
      const char val[] = "format1_test";
      int64_t delta = kv_set(1 /*standard*/, self, key, 24, val, sizeof(val));
      check(delta > 0, "writeperm: delta should be positive for new row");

      char buf[64];
      int32_t sz = kv_get(self, key, 24, buf, sizeof(buf));
      check(sz == (int32_t)sizeof(val), "writeperm: size mismatch");
      check(memcmp(buf, val, sizeof(val)) == 0, "writeperm: data mismatch");
   }

   // ─── 34. testpayer: kv_set with explicit self payer, verify delta ────────
   [[sysio::action]]
   void testpayer() {
      uint64_t self = get_self().value;
      // Payer = self via kv_put helper (key_format=0)
      const char k1[] = {0x29, 0x00, 0x01};
      int64_t d1 = kv_put(self, k1, sizeof(k1), "p1", 2);
      check(d1 > 0, "payer: create delta should be positive");

      char buf[16];
      int32_t sz = kv_get(self, k1, sizeof(k1), buf, sizeof(buf));
      check(sz == 2, "payer: self payer read size");

      // Payer = self via explicit kv_set (key_format=0)
      const char k2[] = {0x29, 0x00, 0x02};
      int64_t d2 = kv_set(0, self, k2, sizeof(k2), "p2val", 5);
      check(d2 > 0, "payer: explicit self payer delta should be positive");

      sz = kv_get(self, k2, sizeof(k2), buf, sizeof(buf));
      check(sz == 5, "payer: explicit payer read size");

      // Update with smaller value — delta should be negative
      int64_t d3 = kv_set(0, self, k2, sizeof(k2), "x", 1);
      check(d3 < 0, "payer: shrink delta should be negative");
   }

   // ─── 35. testmaxkey: kv_set with exactly max_kv_key_size (256 bytes) ─────
   [[sysio::action]]
   void testmaxkey() {
      uint64_t self = get_self().value;
      char key[256];
      memset(key, 0x2A, 256);
      const char val[] = "max_key";
      kv_put(self, key, 256, val, sizeof(val));

      char buf[16];
      int32_t sz = kv_get(self, key, 256, buf, sizeof(buf));
      check(sz == (int32_t)sizeof(val), "maxkey: size mismatch");
   }

   // ─── 36. tstovrszkey: kv_set with 257-byte key (should fail) ────────────
   //     Tested from host side with BOOST_CHECK_THROW
   [[sysio::action]]
   void tstovrszkey() {
      uint64_t self = get_self().value;
      char key[257];
      memset(key, 0x2B, 257);
      // This should trigger kv_key_too_large assertion
      kv_put(self, key, 257, "x", 1);
      check(false, "ovrszkey: should have thrown");
   }

   // ─── 37. testmaxval: kv_set with large value (1024 bytes) ────────────────
   [[sysio::action]]
   void testmaxval() {
      uint64_t self = get_self().value;
      const char key[] = {0x2C, 0x00, 0x01};
      char val[1024];
      for (int i = 0; i < 1024; ++i)
         val[i] = (char)(i & 0xFF);
      kv_put(self, key, sizeof(key), val, 1024);

      char buf[1024];
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 1024, "maxval: size mismatch");
      check(memcmp(buf, val, 1024) == 0, "maxval: data mismatch");
   }

   // ─── 38. testpartread: kv_get with small buffer returns actual size ───────
   [[sysio::action]]
   void testpartread() {
      uint64_t self = get_self().value;
      const char key[] = {0x2D, 0x00, 0x01};
      const char val[] = "this_is_a_long_value_string";
      kv_put(self, key, sizeof(key), val, sizeof(val));

      // Read with small buffer (4 bytes)
      char buf[4] = {};
      int32_t actual_sz = kv_get(self, key, sizeof(key), buf, 4);
      // actual_sz should be the full value size
      check(actual_sz == (int32_t)sizeof(val), "partread: should return full size");
      // Buffer should contain first 4 bytes
      check(memcmp(buf, val, 4) == 0, "partread: partial data mismatch");

      // Read with zero-size buffer — should just return size
      int32_t sz_only = kv_get(self, key, sizeof(key), nullptr, 0);
      check(sz_only == (int32_t)sizeof(val), "partread: zero-buf should return size");
   }

   // ─── 39. testzeroval: kv_set with value_size=0, kv_get returns 0 ─────────
   [[sysio::action]]
   void testzeroval() {
      uint64_t self = get_self().value;
      const char key[] = {0x2E, 0x00, 0x01};
      kv_set(0, self, key, sizeof(key), nullptr, 0);

      char buf[16];
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 0, "zeroval: should return 0 for empty value");
      check(kv_contains(self, key, sizeof(key)) == 1, "zeroval: should still exist");
   }

   // ─── 40. tstvalreplce: overwrite with different value sizes ──────────────
   [[sysio::action]]
   void tstvalreplce() {
      uint64_t self = get_self().value;
      const char key[] = {0x2F, 0x00, 0x01};

      // Start small
      kv_put(self, key, sizeof(key), "ab", 2);
      char buf[64];
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 2, "valreplace: initial size");

      // Grow
      const char bigger[] = "abcdefghijklmnop";
      kv_put(self, key, sizeof(key), bigger, 16);
      sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 16, "valreplace: grown size");
      check(memcmp(buf, bigger, 16) == 0, "valreplace: grown data");

      // Shrink
      kv_put(self, key, sizeof(key), "x", 1);
      sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 1, "valreplace: shrunk size");
      check(buf[0] == 'x', "valreplace: shrunk data");
   }

   // ─── 41. tstkeyfrmt: kv_set with key_format=0 and key_format=1 ──────────
   [[sysio::action]]
   void tstkeyfrmt() {
      uint64_t self = get_self().value;

      // key_format=0 (raw)
      const char k0[] = {0x31, 0x00, 0x01};
      kv_set(0, self, k0, sizeof(k0), "raw", 3);
      char buf[16];
      int32_t sz = kv_get(self, k0, sizeof(k0), buf, sizeof(buf));
      check(sz == 3, "keyformat: raw size");
      check(memcmp(buf, "raw", 3) == 0, "keyformat: raw data");

      // key_format=1 (standard 24B)
      char k1[24];
      memset(k1, 0, 24);
      k1[0] = 0x31;
      k1[1] = 0x01;
      kv_set(1, self, k1, 24, "std", 3);
      sz = kv_get(self, k1, 24, buf, sizeof(buf));
      check(sz == 3, "keyformat: std size");
      check(memcmp(buf, "std", 3) == 0, "keyformat: std data");
   }

   // ─── 43. testidxmulti: multiple secondary indices on same table ───────────
   [[sysio::action]]
   void testidxmulti() {
      uint64_t self = get_self().value;
      uint64_t table = "idxmul"_n.value;
      const char pri[] = {0x32, 0x00, 0x01};

      // Store 3 different secondary keys on 3 different index_ids
      kv_idx_store(table, 0, "name_alice", 10, pri, 3);
      kv_idx_store(table, 1, "age_30", 6, pri, 3);
      kv_idx_store(table, 2, "loc_nyc", 7, pri, 3);

      // Find on each index independently
      uint32_t h0 = kv_idx_find_secondary(self, table, 0, "name_alice", 10);
      char pk[16]; uint32_t pk_sz = 0;
      kv_idx_primary_key(h0, 0, pk, sizeof(pk), &pk_sz);
      check(pk_sz == 3, "idxmulti: idx0 prikey size");
      check(memcmp(pk, pri, 3) == 0, "idxmulti: idx0 prikey");
      kv_idx_destroy(h0);

      uint32_t h1 = kv_idx_find_secondary(self, table, 1, "age_30", 6);
      kv_idx_primary_key(h1, 0, pk, sizeof(pk), &pk_sz);
      check(pk_sz == 3, "idxmulti: idx1 prikey size");
      kv_idx_destroy(h1);

      uint32_t h2 = kv_idx_find_secondary(self, table, 2, "loc_nyc", 7);
      kv_idx_primary_key(h2, 0, pk, sizeof(pk), &pk_sz);
      check(pk_sz == 3, "idxmulti: idx2 prikey size");
      kv_idx_destroy(h2);
   }

   // ─── 44. testidxdupsk: multiple rows with same secondary key ──────────────
   [[sysio::action]]
   void testidxdupsk() {
      uint64_t self = get_self().value;
      uint64_t table = "idxdup"_n.value;

      // 3 rows with same secondary key "shared"
      for (int i = 0; i < 3; ++i) {
         char pri[3] = {0x33, 0x00, (char)(i + 1)};
         kv_idx_store(table, 0, "shared", 6, pri, 3);
      }

      // lower_bound on "shared", iterate, count all
      uint32_t h = kv_idx_lower_bound(self, table, 0, "shared", 6);
      uint32_t count = 0;
      while (true) {
         char sk[16]; uint32_t sk_sz = 0;
         int32_t st = kv_idx_key(h, 0, sk, sizeof(sk), &sk_sz);
         if (st != 0 || sk_sz != 6 || memcmp(sk, "shared", 6) != 0)
            break;
         ++count;
         if (kv_idx_next(h) != 0)
            break;
      }
      check(count == 3, "idxdupsk: should find 3 rows with same sec key");
      kv_idx_destroy(h);
   }

   // ─── 45. testidxrange: lower_bound + iterate through range ────────────────
   [[sysio::action]]
   void testidxrange() {
      uint64_t self = get_self().value;
      uint64_t table = "idxrng"_n.value;

      // Store 5 entries with sec keys "a","b","c","d","e"
      for (int i = 0; i < 5; ++i) {
         char sec[1] = {(char)('a' + i)};
         char pri[3] = {0x34, 0x00, (char)(i + 1)};
         kv_idx_store(table, 0, sec, 1, pri, 3);
      }

      // Range [b, d]: lower_bound("b"), iterate while sec_key <= "d"
      uint32_t h = kv_idx_lower_bound(self, table, 0, "b", 1);
      uint32_t count = 0;
      while (true) {
         char sk[4]; uint32_t sk_sz = 0;
         int32_t st = kv_idx_key(h, 0, sk, sizeof(sk), &sk_sz);
         if (st != 0) break;
         if (sk_sz == 1 && sk[0] > 'd') break;
         ++count;
         if (kv_idx_next(h) != 0) break;
      }
      check(count == 3, "idxrange: should find b,c,d = 3 entries");
      kv_idx_destroy(h);
   }

   // ─── 46. testidxempty: query secondary index with no entries ──────────────
   [[sysio::action]]
   void testidxempty() {
      uint64_t self = get_self().value;
      uint64_t table = "idxemp"_n.value;

      // lower_bound on a table with no entries
      uint32_t h = kv_idx_lower_bound(self, table, 0, "anything", 8);
      char sk[16]; uint32_t sk_sz = 0;
      int32_t st = kv_idx_key(h, 0, sk, sizeof(sk), &sk_sz);
      // Should be at end — status != 0
      check(st != 0, "idxempty: should be at end");
      kv_idx_destroy(h);
   }

   // ─── 47. testmultiit: multiple iterators simultaneously on same prefix ────
   [[sysio::action]]
   void testmultiit() {
      uint64_t self = get_self().value;
      // Insert 3 rows with prefix 0x35
      for (int i = 0; i < 3; ++i) {
         char key[9];
         key[0] = 0x35;
         encode_u64(i, key + 1);
         kv_put(self, key, 9, "v", 1);
      }

      const char prefix[] = {0x35};
      // Open two iterators on same prefix
      uint32_t h1 = kv_it_create(self, prefix, 1);
      uint32_t h2 = kv_it_create(self, prefix, 1);

      char seek[] = {0x35, 0,0,0,0,0,0,0,0};
      kv_it_lower_bound(h1, seek, 9);
      kv_it_lower_bound(h2, seek, 9);

      // Advance h1 twice, h2 once
      kv_it_next(h1);
      kv_it_next(h1);
      kv_it_next(h2);

      // h1 should be on key 2, h2 should be on key 1
      char kbuf1[16], kbuf2[16];
      uint32_t klen1 = 0, klen2 = 0;
      kv_it_key(h1, 0, kbuf1, sizeof(kbuf1), &klen1);
      kv_it_key(h2, 0, kbuf2, sizeof(kbuf2), &klen2);

      uint64_t id1 = 0, id2 = 0;
      for (int j = 1; j <= 8; ++j) {
         id1 = (id1 << 8) | (uint8_t)kbuf1[j];
         id2 = (id2 << 8) | (uint8_t)kbuf2[j];
      }
      check(id1 == 2, "multiit: h1 should be on id 2");
      check(id2 == 1, "multiit: h2 should be on id 1");

      kv_it_destroy(h1);
      kv_it_destroy(h2);
   }

   // ─── 48. testitwritev: write row while iterating, verify visibility ───────
   [[sysio::action]]
   void testitwritev() {
      uint64_t self = get_self().value;
      // Insert 2 rows
      char k1[9]; k1[0] = 0x36; encode_u64(10, k1 + 1);
      char k3[9]; k3[0] = 0x36; encode_u64(30, k3 + 1);
      kv_put(self, k1, 9, "a", 1);
      kv_put(self, k3, 9, "c", 1);

      const char prefix[] = {0x36};
      uint32_t h = kv_it_create(self, prefix, 1);
      char seek[] = {0x36, 0,0,0,0,0,0,0,0};
      kv_it_lower_bound(h, seek, 9);
      check(kv_it_status(h) == 0, "itwritev: on first row");

      // Insert a new row between existing ones
      char k2[9]; k2[0] = 0x36; encode_u64(20, k2 + 1);
      kv_put(self, k2, 9, "b", 1);

      // Advance — should see the new row
      kv_it_next(h);
      check(kv_it_status(h) == 0, "itwritev: should see next row");

      char kbuf[16]; uint32_t klen = 0;
      kv_it_key(h, 0, kbuf, sizeof(kbuf), &klen);
      uint64_t id = 0;
      for (int j = 1; j <= 8; ++j)
         id = (id << 8) | (uint8_t)kbuf[j];
      check(id == 20, "itwritev: should see newly inserted row at id 20");

      kv_it_destroy(h);
   }

   // ─── 49. testmultitbl: rows in different key prefixes are isolated ────────
   [[sysio::action]]
   void testmultitbl() {
      uint64_t self = get_self().value;
      // "Table A" = prefix 0x37, "Table B" = prefix 0x38
      const char ka1[] = {0x37, 0x01};
      const char ka2[] = {0x37, 0x02};
      const char kb1[] = {0x38, 0x01};
      kv_put(self, ka1, 2, "a1", 2);
      kv_put(self, ka2, 2, "a2", 2);
      kv_put(self, kb1, 2, "b1", 2);

      // Iterate table A — should see 2 rows
      const char pa[] = {0x37};
      uint32_t ha = kv_it_create(self, pa, 1);
      char seek_a[] = {0x37, 0x00};
      kv_it_lower_bound(ha, seek_a, 2);
      uint32_t count_a = 0;
      while (kv_it_status(ha) == 0) { ++count_a; kv_it_next(ha); }
      check(count_a == 2, "multitbl: table A should have 2 rows");
      kv_it_destroy(ha);

      // Iterate table B — should see 1 row
      const char pb[] = {0x38};
      uint32_t hb = kv_it_create(self, pb, 1);
      char seek_b[] = {0x38, 0x00};
      kv_it_lower_bound(hb, seek_b, 2);
      uint32_t count_b = 0;
      while (kv_it_status(hb) == 0) { ++count_b; kv_it_next(hb); }
      check(count_b == 1, "multitbl: table B should have 1 row");
      kv_it_destroy(hb);
   }

   // ─── 50. testscoped: key_format=1 with different scopes in 24B key ────────
   [[sysio::action]]
   void testscoped() {
      uint64_t self = get_self().value;

      // 24-byte key: [prefix(1) | scope(8) | id(8) | pad(7)]
      // Scope A = 1, Scope B = 2
      char kA[24]; memset(kA, 0, 24);
      kA[0] = 0x39;
      encode_u64(1, kA + 1);  // scope
      encode_u64(100, kA + 9); // id

      char kB[24]; memset(kB, 0, 24);
      kB[0] = 0x39;
      encode_u64(2, kB + 1);  // scope
      encode_u64(100, kB + 9); // id

      kv_set(1, self, kA, 24, "scopeA", 6);
      kv_set(1, self, kB, 24, "scopeB", 6);

      // Read each and verify isolation
      char buf[16];
      int32_t sz = kv_get(self, kA, 24, buf, sizeof(buf));
      check(sz == 6, "scoped: A size");
      check(memcmp(buf, "scopeA", 6) == 0, "scoped: A data");

      sz = kv_get(self, kB, 24, buf, sizeof(buf));
      check(sz == 6, "scoped: B size");
      check(memcmp(buf, "scopeB", 6) == 0, "scoped: B data");
   }

   // ─── 51. testbinround: binary round-trip with all 256 byte values ─────────
   [[sysio::action]]
   void testbinround() {
      uint64_t self = get_self().value;
      const char key[] = {0x3A, 0x00, 0x01};

      // Build value with all 256 byte values
      char val[256];
      for (int i = 0; i < 256; ++i)
         val[i] = (char)i;
      kv_put(self, key, sizeof(key), val, 256);

      char buf[256];
      int32_t sz = kv_get(self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 256, "binround: size");
      check(memcmp(buf, val, 256) == 0, "binround: data mismatch");
   }

   // ─── 52. testlargepop: populate 100 rows, iterate, verify count & order ──
   [[sysio::action]]
   void testlargepop() {
      uint64_t self = get_self().value;

      // Insert 100 rows with prefix 0x3B
      for (uint64_t i = 0; i < 100; ++i) {
         char key[9];
         key[0] = 0x3B;
         encode_u64(i, key + 1);
         char val[8];
         encode_u64(i * 7, val);
         kv_put(self, key, 9, val, 8);
      }

      // Iterate all rows
      const char prefix[] = {0x3B};
      uint32_t h = kv_it_create(self, prefix, 1);

      char seek[] = {0x3B, 0,0,0,0,0,0,0,0};
      kv_it_lower_bound(h, seek, 9);

      uint32_t count = 0;
      uint64_t prev_id = 0;
      bool first = true;
      while (kv_it_status(h) == 0) {
         char kbuf[16];
         uint32_t klen = 0;
         kv_it_key(h, 0, kbuf, sizeof(kbuf), &klen);
         check(klen == 9, "largepop: key size");

         uint64_t id = 0;
         for (int j = 1; j <= 8; ++j)
            id = (id << 8) | (uint8_t)kbuf[j];

         if (!first) {
            check(id > prev_id, "largepop: must be ascending");
         }
         prev_id = id;
         first = false;
         ++count;
         kv_it_next(h);
      }
      check(count == 100, "largepop: should iterate 100 rows");
      kv_it_destroy(h);
   }

   // ════════════════════════════════════════════════════════════════════════════
   // kv_multi_index tests
   // ════════════════════════════════════════════════════════════════════════════

   struct [[sysio::table]] mi_row {
      uint64_t id;
      uint64_t value;
      uint64_t primary_key() const { return id; }
      SYSLIB_SERIALIZE(mi_row, (id)(value))
   };
   using mi_table = sysio::kv_multi_index<"mitable"_n, mi_row>;

   // ─── tstdecend: decrement end() iterator ─────────────────────────────────
   [[sysio::action]]
   void tstdecend() {
      mi_table t(get_self(), get_self().value);
      // Populate 3 rows
      t.emplace(get_self(), [](mi_row& r) { r.id = 1; r.value = 100; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 2; r.value = 200; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 3; r.value = 300; });

      // --cend() should give us the last element (id==3)
      auto it = t.cend();
      --it;
      check(it->primary_key() == 3, "tstdecend: --cend() should be pk 3");
      check(it->value == 300, "tstdecend: value mismatch");

      // Also test the expression form
      auto it2 = --t.cend();
      check(it2->primary_key() == 3, "tstdecend: (--cend())->pk should be 3");
   }

   // ─── tstbeginend: begin()==end() on empty table ──────────────────────────
   [[sysio::action]]
   void tstbeginend() {
      // Use a different scope so the table is empty
      mi_table t(get_self(), "empty"_n.value);
      check(t.cbegin() == t.cend(), "tstbeginend: empty table begin should equal end");
   }

   // ─── tstfwditer: forward iteration ───────────────────────────────────────
   [[sysio::action]]
   void tstfwditer() {
      mi_table t(get_self(), "fwd"_n.value);
      t.emplace(get_self(), [](mi_row& r) { r.id = 10; r.value = 1; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 20; r.value = 2; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 30; r.value = 3; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 40; r.value = 4; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 50; r.value = 5; });

      uint64_t expected[] = {10, 20, 30, 40, 50};
      uint32_t count = 0;
      for (auto it = t.cbegin(); it != t.cend(); ++it) {
         check(it->primary_key() == expected[count], "tstfwditer: pk mismatch");
         ++count;
      }
      check(count == 5, "tstfwditer: should see 5 rows");
   }

   // ─── tstreviter: reverse iteration ───────────────────────────────────────
   [[sysio::action]]
   void tstreviter() {
      mi_table t(get_self(), "rev"_n.value);
      t.emplace(get_self(), [](mi_row& r) { r.id = 10; r.value = 1; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 20; r.value = 2; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 30; r.value = 3; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 40; r.value = 4; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 50; r.value = 5; });

      uint64_t expected[] = {50, 40, 30, 20, 10};
      uint32_t count = 0;
      auto it = --t.cend();
      while (true) {
         check(it->primary_key() == expected[count], "tstreviter: pk mismatch");
         ++count;
         if (it == t.cbegin()) break;
         --it;
      }
      check(count == 5, "tstreviter: should see 5 rows");
   }

   // ─── tstfindmiss: find() returns end() for missing key ───────────────────
   [[sysio::action]]
   void tstfindmiss() {
      mi_table t(get_self(), "findmiss"_n.value);
      t.emplace(get_self(), [](mi_row& r) { r.id = 1; r.value = 100; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 2; r.value = 200; });

      auto it = t.find(99999);
      check(it == t.end(), "tstfindmiss: find(99999) should return end()");
   }

   // ─── tstenddestr: end() iterator destructor doesn't crash ────────────────
   [[sysio::action]]
   void tstenddestr() {
      {
         mi_table t(get_self(), "enddestr"_n.value);
         auto it = t.end(); // sentinel handle -1
         // it goes out of scope here — destructor must not crash
      }
      // If we get here, the destructor didn't crash
      check(true, "tstenddestr: survived end() destructor");
   }

   // ─── tstemplace: emplace + get round trip ────────────────────────────────
   [[sysio::action]]
   void tstemplace() {
      mi_table t(get_self(), "emplace"_n.value);
      t.emplace(get_self(), [](mi_row& r) { r.id = 42; r.value = 12345; });

      const auto& row = t.get(42);
      check(row.id == 42, "tstemplace: id mismatch");
      check(row.value == 12345, "tstemplace: value mismatch");
   }

   // ─── tstmodify: modify via kv_multi_index ────────────────────────────────
   [[sysio::action]]
   void tstmodify() {
      mi_table t(get_self(), "modify"_n.value);
      t.emplace(get_self(), [](mi_row& r) { r.id = 7; r.value = 100; });

      const auto& row = t.get(7);
      check(row.value == 100, "tstmodify: initial value");

      t.modify(row, get_self(), [](mi_row& r) { r.value = 999; });

      const auto& updated = t.get(7);
      check(updated.value == 999, "tstmodify: updated value");
   }

   // ─── tsterase: erase via kv_multi_index ──────────────────────────────────
   [[sysio::action]]
   void tsterase() {
      mi_table t(get_self(), "erase"_n.value);
      t.emplace(get_self(), [](mi_row& r) { r.id = 55; r.value = 777; });

      // Verify it exists
      check(t.find(55) != t.end(), "tsterase: should exist before erase");

      const auto& row = t.get(55);
      t.erase(row);

      check(t.find(55) == t.end(), "tsterase: should be end() after erase");
   }

   // ─── tstlbound: lower_bound via kv_multi_index ──────────────────────────
   [[sysio::action]]
   void tstlbound() {
      mi_table t(get_self(), "lbound"_n.value);
      t.emplace(get_self(), [](mi_row& r) { r.id = 10; r.value = 1; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 20; r.value = 2; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 30; r.value = 3; });

      // lower_bound(15) should return iterator to row 20
      auto it1 = t.lower_bound(15);
      check(it1 != t.end(), "tstlbound: lb(15) should not be end");
      check(it1->primary_key() == 20, "tstlbound: lb(15) should be pk 20");

      // lower_bound(20) should return iterator to row 20
      auto it2 = t.lower_bound(20);
      check(it2 != t.end(), "tstlbound: lb(20) should not be end");
      check(it2->primary_key() == 20, "tstlbound: lb(20) should be pk 20");

      // lower_bound(31) should return end()
      auto it3 = t.lower_bound(31);
      check(it3 == t.end(), "tstlbound: lb(31) should be end");
   }

   // ════════════════════════════════════════════════════════════════════════════
   // secondary_index_view modify/erase tests
   // ════════════════════════════════════════════════════════════════════════════

   struct [[sysio::table]] sec_row {
      uint64_t pk;
      uint64_t age;
      uint64_t primary_key() const { return pk; }
      uint64_t by_age() const { return age; }
      SYSLIB_SERIALIZE(sec_row, (pk)(age))
   };
   using sec_table = sysio::multi_index<"sectbl"_n, sec_row,
      sysio::indexed_by<"byage"_n, sysio::const_mem_fun<sec_row, uint64_t, &sec_row::by_age>>
   >;

   // ─── tstsecmod: modify row via secondary iterator ──────────────────────────
   [[sysio::action]]
   void tstsecmod() {
      sec_table t(get_self(), get_self().value);
      t.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 25; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 2; r.age = 30; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 3; r.age = 35; });

      auto idx = t.get_index<"byage"_n>();
      auto it = idx.find(30);
      check(it != idx.end(), "tstsecmod: find(30) should exist");
      check(it->pk == 2, "tstsecmod: pk should be 2");

      // Modify via secondary iterator
      idx.modify(it, get_self(), [](sec_row& r) { r.age = 99; });

      // Verify via primary lookup
      const auto& row = t.get(2);
      check(row.age == 99, "tstsecmod: age should be 99 after modify");

      // Old secondary key should be gone
      auto it2 = idx.find(30);
      check(it2 == idx.end(), "tstsecmod: old secondary key 30 should be gone");

      // New secondary key should exist
      auto it3 = idx.find(99);
      check(it3 != idx.end(), "tstsecmod: new secondary key 99 should exist");
      check(it3->pk == 2, "tstsecmod: pk at new key should be 2");
   }

   // ─── tstsecerase: erase row via secondary iterator ─────────────────────────
   [[sysio::action]]
   void tstsecerase() {
      sec_table t(get_self(), "secerase"_n.value);
      t.emplace(get_self(), [](sec_row& r) { r.pk = 10; r.age = 100; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 20; r.age = 200; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 30; r.age = 300; });

      auto idx = t.get_index<"byage"_n>();

      // Erase via secondary iterator
      auto it = idx.find(200);
      check(it != idx.end(), "tstsecerase: find(200) should exist");
      idx.erase(it);

      // Verify primary row is gone
      check(t.find(20) == t.end(), "tstsecerase: pk 20 should be gone");

      // Verify secondary key is gone
      auto it2 = idx.find(200);
      check(it2 == idx.end(), "tstsecerase: sec key 200 should be gone");

      // Other rows intact
      check(t.find(10) != t.end(), "tstsecerase: pk 10 should remain");
      check(t.find(30) != t.end(), "tstsecerase: pk 30 should remain");
   }

   // ─── tstseciter: iterate secondary index in order ──────────────────────────
   [[sysio::action]]
   void tstseciter() {
      sec_table t(get_self(), "seciter"_n.value);
      t.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 50; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 2; r.age = 20; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 3; r.age = 40; });

      auto idx = t.get_index<"byage"_n>();
      auto it = idx.begin();

      // Should iterate in secondary key order: 20, 40, 50
      check(it != idx.end(), "tstseciter: begin should be valid");
      check(it->age == 20, "tstseciter: first should be age 20");
      ++it;
      check(it->age == 40, "tstseciter: second should be age 40");
      ++it;
      check(it->age == 50, "tstseciter: third should be age 50");
      ++it;
      check(it == idx.end(), "tstseciter: should be end after 3");
   }

   // ─── tstseccoerce: find with int literal coerces to uint64_t secondary key ─
   [[sysio::action]]
   void tstseccoerce() {
      sec_table t(get_self(), "coerce"_n.value);
      t.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 42; });

      auto idx = t.get_index<"byage"_n>();

      // Find with int literal (not uint64_t) — must coerce correctly
      auto it1 = idx.find(42);
      check(it1 != idx.end(), "tstseccoerce: find(int 42) should work");
      check(it1->pk == 1, "tstseccoerce: pk should be 1");

      // Find with explicit uint64_t — same result
      auto it2 = idx.find(uint64_t(42));
      check(it2 != idx.end(), "tstseccoerce: find(uint64_t 42) should work");
      check(it2->pk == 1, "tstseccoerce: pk should be 1");

      // lower_bound with int literal
      auto it3 = idx.lower_bound(40);
      check(it3 != idx.end(), "tstseccoerce: lower_bound(int 40) should work");
      check(it3->age == 42, "tstseccoerce: lower_bound(40) should find age 42");
   }

   // ─── tstrbegin: rbegin/rend on primary table ───────────────────────────────
   [[sysio::action]]
   void tstrbegin() {
      mi_table t(get_self(), "rbegin"_n.value);
      t.emplace(get_self(), [](mi_row& r) { r.id = 10; r.value = 1; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 20; r.value = 2; });
      t.emplace(get_self(), [](mi_row& r) { r.id = 30; r.value = 3; });

      // rbegin should point to last element (pk=30)
      auto rit = t.rbegin();
      check(rit != t.rend(), "tstrbegin: rbegin should not be rend");
      check(rit->id == 30, "tstrbegin: rbegin should be pk 30");
      ++rit;
      check(rit->id == 20, "tstrbegin: second should be pk 20");
      ++rit;
      check(rit->id == 10, "tstrbegin: third should be pk 10");
      ++rit;
      check(rit == t.rend(), "tstrbegin: should be rend after 3");
   }

   // ─── tstrbempty: rbegin==rend on empty table ───────────────────────────
   [[sysio::action]]
   void tstrbempty() {
      mi_table t(get_self(), "rbempty"_n.value);
      check(t.rbegin() == t.rend(), "tstrbempty: rbegin should equal rend on empty table");
   }

   // ─── tstsecrbegin: rbegin/rend on secondary index ─────────────────────────
   [[sysio::action]]
   void tstsecrbegin() {
      sec_table t(get_self(), "secrb"_n.value);
      t.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 50; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 2; r.age = 20; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 3; r.age = 40; });

      auto idx = t.get_index<"byage"_n>();

      // rbegin should be the highest age (50)
      auto rit = idx.rbegin();
      check(rit != idx.rend(), "tstsecrbegin: rbegin should not be rend");
      check(rit->age == 50, "tstsecrbegin: rbegin should be age 50");
      ++rit;
      check(rit->age == 40, "tstsecrbegin: second should be age 40");
      ++rit;
      check(rit->age == 20, "tstsecrbegin: third should be age 20");
      ++rit;
      check(rit == idx.rend(), "tstsecrbegin: should be rend after 3");
   }

   // ─── tstsecersnxt: secondary erase returns next iterator ──────────────────
   [[sysio::action]]
   void tstsecersnxt() {
      sec_table t(get_self(), "secersnxt"_n.value);
      t.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 100; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 2; r.age = 200; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 3; r.age = 300; });

      auto idx = t.get_index<"byage"_n>();
      auto it = idx.begin();
      check(it->age == 100, "tstsecersnxt: first should be age 100");

      // Erase first, should return iterator to second (age 200)
      it = idx.erase(it);
      check(it != idx.end(), "tstsecersnxt: erase should return next");
      check(it->age == 200, "tstsecersnxt: next after erase should be age 200");

      // Erase second, should return iterator to third (age 300)
      it = idx.erase(it);
      check(it != idx.end(), "tstsecersnxt: erase should return next again");
      check(it->age == 300, "tstsecersnxt: next after erase should be age 300");

      // Erase third, should return end
      it = idx.erase(it);
      check(it == idx.end(), "tstsecersnxt: erase last should return end");
   }

   // ════════════════════════════════════════════════════════════════════════════
   // kv::mapping tests
   // ════════════════════════════════════════════════════════════════════════════

   struct map_val {
      uint64_t x;
      uint64_t y;
      SYSLIB_SERIALIZE(map_val, (x)(y))
   };

   // ─── tstmapsetget: mapping set + get round trip ────────────────────────────
   [[sysio::action]]
   void tstmapsetget() {
      sysio::kv::mapping<uint64_t, map_val> m;
      m.set(42, map_val{100, 200});

      auto val = m.get(42);
      check(val.has_value(), "tstmapsetget: get(42) should return value");
      check(val->x == 100, "tstmapsetget: x should be 100");
      check(val->y == 200, "tstmapsetget: y should be 200");
   }

   // ─── tstmapcont: mapping contains + erase ──────────────────────────────────
   [[sysio::action]]
   void tstmapcont() {
      sysio::kv::mapping<uint64_t, uint64_t> m;
      m.set(7, uint64_t(999));

      check(m.contains(7), "tstmapcont: should contain 7");
      check(!m.contains(8), "tstmapcont: should not contain 8");

      m.erase(7);
      check(!m.contains(7), "tstmapcont: should not contain 7 after erase");

      auto val = m.get(7);
      check(!val.has_value(), "tstmapcont: get(7) should be nullopt after erase");
   }

   // ─── tstmapupdate: mapping update existing key ─────────────────────────────
   [[sysio::action]]
   void tstmapupdate() {
      sysio::kv::mapping<uint64_t, map_val> m;
      m.set(1, map_val{10, 20});
      m.set(1, map_val{30, 40});

      auto val = m.get(1);
      check(val.has_value(), "tstmapupdate: should exist");
      check(val->x == 30 && val->y == 40, "tstmapupdate: should be updated value");
   }

   // ─── tstmapstrkey: mapping with name key ───────────────────────────────────
   [[sysio::action]]
   void tstmapstrkey() {
      sysio::kv::mapping<sysio::name, uint64_t> m;
      m.set("alice"_n, uint64_t(100));
      m.set("bob"_n, uint64_t(200));

      auto a = m.get("alice"_n);
      auto b = m.get("bob"_n);
      check(a.has_value() && *a == 100, "tstmapstrkey: alice should be 100");
      check(b.has_value() && *b == 200, "tstmapstrkey: bob should be 200");
   }

   // ════════════════════════════════════════════════════════════════════════════
   // kv::table + begin_all_scopes tests
   // ════════════════════════════════════════════════════════════════════════════

   struct [[sysio::table]] kvt_row {
      uint64_t pk;
      uint64_t val;
      uint64_t primary_key() const { return pk; }
      SYSLIB_SERIALIZE(kvt_row, (pk)(val))
   };
   using kvt_table = sysio::kv::table<"kvttable"_n, kvt_row>;

   // ─── tstkvtbasic: kv::table set + get + erase ─────────────────────────────
   [[sysio::action]]
   void tstkvtbasic() {
      kvt_table t(get_self(), get_self().value);

      t.set(1, kvt_row{1, 100});
      t.set(2, kvt_row{2, 200});

      check(t.contains(1), "tstkvtbasic: should contain pk 1");
      check(t.contains(2), "tstkvtbasic: should contain pk 2");

      const auto& r1 = t.get(1);
      check(r1.val == 100, "tstkvtbasic: pk 1 val should be 100");

      t.erase(1);
      check(!t.contains(1), "tstkvtbasic: pk 1 should be gone after erase");
      check(t.contains(2), "tstkvtbasic: pk 2 should remain");
   }

   // ─── tstkvtiter: kv::table begin/end iteration ────────────────────────────
   [[sysio::action]]
   void tstkvtiter() {
      kvt_table t(get_self(), "iter"_n.value);
      t.set(10, kvt_row{10, 1});
      t.set(20, kvt_row{20, 2});
      t.set(30, kvt_row{30, 3});

      int count = 0;
      uint64_t prev_pk = 0;
      for (auto it = t.begin(); it != t.end(); ++it) {
         check(it->pk > prev_pk, "tstkvtiter: should be ascending");
         prev_pk = it->pk;
         ++count;
      }
      check(count == 3, "tstkvtiter: should iterate 3 rows");
   }

   // ─── tstkvtscope: begin_all_scopes across multiple scopes ─────────────────
   [[sysio::action]]
   void tstkvtscope() {
      kvt_table t1(get_self(), "scope1"_n.value);
      kvt_table t2(get_self(), "scope2"_n.value);
      kvt_table t3(get_self(), "scope3"_n.value);

      t1.set(1, kvt_row{1, 100});
      t2.set(2, kvt_row{2, 200});
      t3.set(3, kvt_row{3, 300});

      // begin_all_scopes should see all 3 rows across all scopes
      kvt_table any_scope(get_self(), 0);
      int count = 0;
      for (auto it = any_scope.begin_all_scopes(); it != any_scope.end_all_scopes(); ++it) {
         ++count;
      }
      check(count == 3, "tstkvtscope: begin_all_scopes should find 3 rows across 3 scopes");
   }

   // ════════════════════════════════════════════════════════════════════════════
   // Payer validation tests
   // ════════════════════════════════════════════════════════════════════════════

   // ─── tstpayerself: kv_set with payer=0 (self) always works ─────────────────
   [[sysio::action]]
   void tstpayerself() {
      char key[] = "payerself";
      char val[] = "data";
      // payer=0 means receiver pays — should always work
      kv_set(0, 0, key, sizeof(key), val, sizeof(val));
      check(kv_contains(get_self().value, key, sizeof(key)) != 0, "tstpayerself: key should exist");
   }

   // ─── tstpayeroth: kv_set with non-zero payer from non-privileged — must fail
   [[sysio::action]]
   void tstpayeroth() {
      char key[] = "payerother";
      char val[] = "data";
      // payer = some other account (not self) — should fail for non-privileged
      kv_set(0, "alice"_n.value, key, sizeof(key), val, sizeof(val));
   }

   // ─── tstpayerprv: kv_set with non-zero payer from privileged — should work
   [[sysio::action]]
   void tstpayerprv() {
      char key[] = "payerpriv";
      char val[] = "data";
      // This contract must be set_privileged before calling this action
      kv_set(0, "alice"_n.value, key, sizeof(key), val, sizeof(val));
      check(kv_contains(get_self().value, key, sizeof(key)) != 0, "tstpayerprv: key should exist");
   }

   // ─── tstemptykey: empty key (size=0) must be rejected ──────────────────────
   [[sysio::action]]
   void tstemptykey() {
      char val[] = "data";
      // This should throw — empty keys are not allowed
      kv_set(0, 0, nullptr, 0, val, sizeof(val));
   }

   // ─── tstbadfmt: invalid key_format (>1) must be rejected ──────────────────
   [[sysio::action]]
   void tstbadfmt() {
      char key[] = "badfmt";
      char val[] = "data";
      kv_set(2, 0, key, sizeof(key), val, sizeof(val));
   }

   // ════════════════════════════════════════════════════════════════════════════
   // Security edge case tests (from audit recommendations)
   // ════════════════════════════════════════════════════════════════════════════

   // ─── tstreraseins: erase row under iterator, reinsert same key, iterator sees new row
   [[sysio::action]]
   void tstreraseins() {
      char prefix[] = {(char)0xEE};
      char key[] = {(char)0xEE, 0x01};
      char val1[] = "original";
      char val2[] = "replaced";

      // Store and create iterator positioned at key
      kv_set(0, 0, key, sizeof(key), val1, sizeof(val1));
      uint32_t h = kv_it_create(get_self().value, prefix, sizeof(prefix));
      int32_t st = kv_it_lower_bound(h, key, sizeof(key));
      check(st == 0, "tstreraseins: should find key");

      // Erase and reinsert with different value
      kv_erase(key, sizeof(key));
      kv_set(0, 0, key, sizeof(key), val2, sizeof(val2));

      // Read via iterator — should see new row (re-seek semantics)
      uint32_t actual = 0;
      char buf[16] = {};
      int32_t status = kv_it_value(h, 0, buf, sizeof(buf), &actual);
      // Status should be ok (0) — cursor re-seeks to key which now exists
      check(status == 0, "tstreraseins: iterator should find reinserted row");
      check(actual == sizeof(val2), "tstreraseins: value size should match new value");

      kv_it_destroy(h);
   }

   // ─── tstprevbgn: kv_it_prev from begin position should return end status
   [[sysio::action]]
   void tstprevbgn() {
      char prefix[] = {(char)0xEF};
      char key[] = {(char)0xEF, 0x01};
      char val[] = "data";

      kv_set(0, 0, key, sizeof(key), val, sizeof(val));

      uint32_t h = kv_it_create(get_self().value, prefix, sizeof(prefix));
      int32_t st = kv_it_status(h);
      check(st == 0, "tstprevbgn: should be at first element");

      // prev from first element should go to end (status 1)
      int32_t prev_st = kv_it_prev(h);
      check(prev_st == 1, "tstprevbgn: prev from begin should return end status");

      kv_it_destroy(h);
   }

   // ─── tstpreveras: kv_it_prev on erased iterator
   [[sysio::action]]
   void tstpreveras() {
      char prefix[] = {(char)0xF0};
      char key1[] = {(char)0xF0, 0x01};
      char key2[] = {(char)0xF0, 0x02};
      char val[] = "data";

      kv_set(0, 0, key1, sizeof(key1), val, sizeof(val));
      kv_set(0, 0, key2, sizeof(key2), val, sizeof(val));

      // Position iterator at key2
      uint32_t h = kv_it_create(get_self().value, prefix, sizeof(prefix));
      kv_it_lower_bound(h, key2, sizeof(key2));
      check(kv_it_status(h) == 0, "tstpreveras: should be at key2");

      // Erase key2
      kv_erase(key2, sizeof(key2));

      // kv_it_key should report erased
      uint32_t actual = 0;
      char buf[4];
      int32_t key_st = kv_it_key(h, 0, buf, sizeof(buf), &actual);
      check(key_st == 2, "tstpreveras: should report erased status");

      kv_it_destroy(h);
   }

   // ─── tstrdonly: kv_set in read-only context must fail
   // Note: this action itself doesn't enforce read-only — that's set by the
   // transaction context. The test driver must push this as a read-only trx.
   [[sysio::action]]
   void tstrdonly() {
      char key[] = "rdonly";
      char val[] = "data";
      kv_set(0, 0, key, sizeof(key), val, sizeof(val));
   }

   // ─── tstkeyoffbnd: kv_it_key with offset >= key_size returns 0 bytes ──────
   [[sysio::action]]
   void tstkeyoffbnd() {
      char prefix[] = {(char)0xF1};
      char key[] = {(char)0xF1, 0x01, 0x02};
      char val[] = "data";
      kv_set(0, 0, key, sizeof(key), val, sizeof(val));

      uint32_t h = kv_it_create(get_self().value, prefix, sizeof(prefix));
      check(kv_it_status(h) == 0, "tstkeyoffbnd: should be at key");

      // Read key with offset == key_size (3) — should copy 0 bytes
      uint32_t actual = 0;
      char buf[8] = {0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42};
      int32_t st = kv_it_key(h, 3, buf, sizeof(buf), &actual);
      check(st == 0, "tstkeyoffbnd: status should be ok");
      check(actual == 3, "tstkeyoffbnd: actual_size should be full key size");
      // buf should be untouched since offset >= key_size
      check(buf[0] == 0x42, "tstkeyoffbnd: buf should be untouched");

      // Read key with offset > key_size — same behavior
      uint32_t actual2 = 0;
      st = kv_it_key(h, 100, buf, sizeof(buf), &actual2);
      check(st == 0, "tstkeyoffbnd: status ok for large offset");
      check(actual2 == 3, "tstkeyoffbnd: actual_size still full key size");

      kv_it_destroy(h);
   }

   // ─── tstbigseckey: oversized secondary key must be rejected ───────────────
   [[sysio::action]]
   void tstbigseckey() {
      // max_kv_secondary_key_size is typically 256
      // Create a 257-byte secondary key — should fail
      char sec_key[257];
      memset(sec_key, 'A', sizeof(sec_key));
      char pri_key[] = {0x01};
      kv_idx_store("testtable"_n.value, 0, sec_key, sizeof(sec_key), pri_key, sizeof(pri_key));
   }

   // ─── tstnotifyram: write in notification context bills receiver's RAM ─────
   [[sysio::action]]
   void tstnotifyram() {
      // When receiving a notification (receiver != act.account),
      // kv_set writes to receiver's KV namespace and bills receiver
      char key[] = "notifykey";
      char val[] = "notifyval";
      kv_set(0, 0, key, sizeof(key), val, sizeof(val));
   }

   // ─── tstsendnotif: send notification to trigger tstnotifyram on another account
   [[sysio::action]]
   void tstsendnotif() {
      // This sends a notification to 'kvnotify' account which also has this contract
      require_recipient("kvnotify"_n);
   }

   // ════════════════════════════════════════════════════════════════════════════
   // RAM billing test helpers — parameterized actions for host-side verification
   // ════════════════════════════════════════════════════════════════════════════

   // Store a row with configurable key and value sizes
   [[sysio::action]]
   void ramstore(uint32_t key_id, uint32_t val_size) {
      char key[4];
      memcpy(key, &key_id, 4);
      std::vector<char> val(val_size, 'X');
      kv_set(0, 0, key, 4, val.data(), val_size);
   }

   // Update an existing row with a new value size
   [[sysio::action]]
   void ramupdate(uint32_t key_id, uint32_t val_size) {
      char key[4];
      memcpy(key, &key_id, 4);
      std::vector<char> val(val_size, 'Y');
      kv_set(0, 0, key, 4, val.data(), val_size);
   }

   // Erase a row
   [[sysio::action]]
   void ramerase(uint32_t key_id) {
      char key[4];
      memcpy(key, &key_id, 4);
      kv_erase(key, 4);
   }
};
