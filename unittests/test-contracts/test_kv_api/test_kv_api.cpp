// Comprehensive KV intrinsic test contract.
// Tests KV host functions through WASM.
// Replaces legacy test_api_db / test_api_multi_index for KV coverage.
// Intrinsic declarations are provided by the CDT headers (kv_multi_index.hpp,
// kv_raw_table.hpp, kv_table.hpp).

#include <sysio/sysio.hpp>
#include <sysio/kv_multi_index.hpp>
// kv_table.hpp (old scoped table) and kv_raw_table.hpp (raw_table class) dropped.
// Utility types (ser_buf, be_key_stream) available via kv_multi_index.hpp → kv_raw_table.hpp.
#include <cstring>

// Arbitrary table_id values for raw KV tests.
// table_id is an unrestricted uint32_t namespace tag.
static constexpr uint32_t test_table_id     = 42;
static constexpr uint32_t test_sec_table_id = 100;

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

// kv_set wrapper using test_table_id and self as payer
static int64_t kv_put(uint64_t self, const void* key, uint32_t key_size,
                      const void* value, uint32_t value_size) {
   return kv_set(test_table_id, self, key, key_size, value, value_size);
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
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
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
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
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

      kv_erase(test_table_id, key, sizeof(key));

      char buf[64];
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
      check(sz == -1, "erase: kv_get should return -1");
   }

   // ─── 4. testkvexist: contains check ────────────────────────────────────
   [[sysio::action]]
   void testkvexist() {
      uint64_t self = get_self().value;
      const char key[] = {0x04, 0x00, 0x01};
      const char val[] = "exists";
      kv_put(self, key, sizeof(key), val, sizeof(val));
      check(kv_contains(test_table_id, self, key, sizeof(key)) == 1, "contains: should be 1");

      kv_erase(test_table_id, key, sizeof(key));
      check(kv_contains(test_table_id, self, key, sizeof(key)) == 0, "contains: should be 0 after erase");
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
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 10, "setbytes: size should be 10");
      buf[3] = 'X'; buf[4] = 'Y'; buf[5] = 'Z';
      kv_put(self, key, sizeof(key), buf, 10);

      char buf2[64] = {};
      kv_get(test_table_id, self, key, sizeof(key), buf2, sizeof(buf2));
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
      uint32_t handle = kv_it_create(test_table_id, self, prefix, 1);
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
      uint32_t handle = kv_it_create(test_table_id, self, prefix, 1);

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
      uint32_t handle = kv_it_create(test_table_id, self, prefix, 1);

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
      uint32_t handle = kv_it_create(test_table_id, self, prefix, 1);

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
      uint32_t handle = kv_it_create(test_table_id, self, prefix, 1);
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
      uint32_t handle = kv_it_create(test_table_id, self, prefix, 1);
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
      uint32_t handle = kv_it_create(test_table_id, self, prefix, 1);

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

      const char sec[] = "alice";
      const char pri[] = {0x0D, 0x00, 0x01};
      kv_idx_store(0, test_sec_table_id, pri, 3, sec, 5);

      int32_t handle = kv_idx_find_secondary(self, test_sec_table_id, sec, 5);
      check(handle >= 0, "idx_store: find should succeed");
      // Check we can read the primary key back
      char pk_buf[16];
      uint32_t pk_sz = 0;
      kv_idx_primary_key((uint32_t)handle, 0, pk_buf, sizeof(pk_buf), &pk_sz);
      check(pk_sz == 3, "idx_store: pri key size");
      check(memcmp(pk_buf, pri, 3) == 0, "idx_store: pri key mismatch");
      kv_idx_destroy((uint32_t)handle);
   }

   // ─── 14. testidxremov: store then remove ─────────────────────────────────
   [[sysio::action]]
   void testidxremov() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 101;

      const char sec[] = "bob";
      const char pri[] = {0x0E, 0x00, 0x01};
      kv_idx_store(0, sec_tid, pri, 3, sec, 3);

      // Verify it exists
      int32_t h = kv_idx_find_secondary(self, sec_tid, sec, 3);
      check(h >= 0, "idx_remove: find should succeed before remove");
      int32_t st = kv_idx_next((uint32_t)h); // check handle is valid by calling next
      kv_idx_destroy((uint32_t)h);

      // Remove
      kv_idx_remove(sec_tid, pri, 3, sec, 3);

      // lower_bound should not find it: returns -1 (no entry in range)
      int32_t h2 = kv_idx_lower_bound(self, sec_tid, sec, 3);
      check(h2 < 0, "idx_remove: entry should be gone");
   }

   // ─── 15. testidxupdat: update secondary key ─────────────────────────────
   [[sysio::action]]
   void testidxupdat() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 102;

      const char old_sec[] = "charlie";
      const char new_sec[] = "david";
      const char pri[] = {0x0F, 0x00, 0x01};

      kv_idx_store(0, sec_tid, pri, 3, old_sec, 7);
      kv_idx_update(0, sec_tid, pri, 3, old_sec, 7, new_sec, 5);

      // Find by new key
      int32_t h = kv_idx_find_secondary(self, sec_tid, new_sec, 5);
      check(h >= 0, "idx_update: find should succeed");
      char pk_buf[16];
      uint32_t pk_sz = 0;
      kv_idx_primary_key((uint32_t)h, 0, pk_buf, sizeof(pk_buf), &pk_sz);
      check(pk_sz == 3, "idx_update: pri key size");
      check(memcmp(pk_buf, pri, 3) == 0, "idx_update: pri key mismatch");
      kv_idx_destroy((uint32_t)h);
   }

   // ─── 16. testidxfind: find by secondary key ─────────────────────────────
   [[sysio::action]]
   void testidxfind() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 103;

      // Store 3 entries
      const char secs[][8] = {"alpha", "beta", "gamma"};
      for (int i = 0; i < 3; ++i) {
         char pri[3] = {0x10, 0x00, (char)(i + 1)};
         kv_idx_store(0, sec_tid, pri, 3, secs[i], (uint32_t)strlen(secs[i]));
      }

      // Find "beta"
      int32_t h = kv_idx_find_secondary(self, sec_tid, "beta", 4);
      check(h >= 0, "idx_find: should find beta");
      char sk_buf[16];
      uint32_t sk_sz = 0;
      kv_idx_key((uint32_t)h, 0, sk_buf, sizeof(sk_buf), &sk_sz);
      check(sk_sz == 4, "idx_find: sec key size");
      check(memcmp(sk_buf, "beta", 4) == 0, "idx_find: sec key mismatch");
      kv_idx_destroy((uint32_t)h);
   }

   // ─── 17. testidxlbnd: lower_bound on secondary index ────────────────────
   [[sysio::action]]
   void testidxlbnd() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 104;

      // Store entries with numeric secondary keys: "10", "20", "30"
      for (int i = 1; i <= 3; ++i) {
         char sec[3];
         sec[0] = '0' + (char)i;
         sec[1] = '0';
         sec[2] = '\0';
         char pri[3] = {0x11, 0x00, (char)i};
         kv_idx_store(0, sec_tid, pri, 3, sec, 2);
      }

      // lower_bound("15") should land on "20"
      int32_t h = kv_idx_lower_bound(self, sec_tid, "15", 2);
      check(h >= 0, "idx_lbound: should find entry");
      char sk_buf[16];
      uint32_t sk_sz = 0;
      kv_idx_key((uint32_t)h, 0, sk_buf, sizeof(sk_buf), &sk_sz);
      check(sk_sz == 2, "idx_lbound: sec key size");
      check(sk_buf[0] == '2' && sk_buf[1] == '0', "idx_lbound: should land on '20'");
      kv_idx_destroy((uint32_t)h);
   }

   // ─── 18. testidxnext: forward iterate secondary index ────────────────────
   [[sysio::action]]
   void testidxnext() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 105;

      const char secs[][4] = {"aaa", "bbb", "ccc"};
      for (int i = 0; i < 3; ++i) {
         char pri[3] = {0x12, 0x00, (char)(i + 1)};
         kv_idx_store(0, sec_tid, pri, 3, secs[i], 3);
      }

      int32_t h = kv_idx_lower_bound(self, sec_tid, "aaa", 3);
      check(h >= 0, "idx_next: lower_bound should find entry");
      // We should be on "aaa", advance to "bbb"
      int32_t st = kv_idx_next((uint32_t)h);
      check(st == 0, "idx_next: should return 0 (ok)");

      char sk_buf[16];
      uint32_t sk_sz = 0;
      kv_idx_key((uint32_t)h, 0, sk_buf, sizeof(sk_buf), &sk_sz);
      check(sk_sz == 3, "idx_next: key size");
      check(memcmp(sk_buf, "bbb", 3) == 0, "idx_next: should be on bbb");
      kv_idx_destroy((uint32_t)h);
   }

   // ─── 19. testidxprev: backward iterate secondary index ───────────────────
   [[sysio::action]]
   void testidxprev() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 106;

      const char secs[][4] = {"xxx", "yyy", "zzz"};
      for (int i = 0; i < 3; ++i) {
         char pri[3] = {0x13, 0x00, (char)(i + 1)};
         kv_idx_store(0, sec_tid, pri, 3, secs[i], 3);
      }

      // Find "zzz", then prev to "yyy"
      int32_t h = kv_idx_find_secondary(self, sec_tid, "zzz", 3);
      check(h >= 0, "idx_prev: find should succeed");
      int32_t st = kv_idx_prev((uint32_t)h);
      check(st == 0, "idx_prev: should return 0 (ok)");

      char sk_buf[16];
      uint32_t sk_sz = 0;
      kv_idx_key((uint32_t)h, 0, sk_buf, sizeof(sk_buf), &sk_sz);
      check(sk_sz == 3, "idx_prev: key size");
      check(memcmp(sk_buf, "yyy", 3) == 0, "idx_prev: should be on yyy");
      kv_idx_destroy((uint32_t)h);
   }

   // ─── 20. testidxkey: read secondary key from iterator ────────────────────
   [[sysio::action]]
   void testidxkey() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 107;

      const char sec[] = "seckey_data";
      const char pri[] = {0x14, 0x00, 0x01};
      kv_idx_store(0, sec_tid, pri, 3, sec, 11);

      int32_t h = kv_idx_find_secondary(self, sec_tid, sec, 11);
      check(h >= 0, "idx_key: find should succeed");
      char dest[32];
      uint32_t actual = 0;
      int32_t st = kv_idx_key((uint32_t)h, 0, dest, sizeof(dest), &actual);
      check(st == 0, "idx_key: status");
      check(actual == 11, "idx_key: size");
      check(memcmp(dest, sec, 11) == 0, "idx_key: data mismatch");
      kv_idx_destroy((uint32_t)h);
   }

   // ─── 21. testidxprik: read primary key from secondary iterator ───────────
   [[sysio::action]]
   void testidxprik() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 108;

      const char sec[] = "lookup";
      char pri[9];
      pri[0] = 0x15;
      encode_u64(12345, pri + 1);
      kv_idx_store(0, sec_tid, pri, 9, sec, 6);

      int32_t h = kv_idx_find_secondary(self, sec_tid, sec, 6);
      check(h >= 0, "idx_prikey: find should succeed");
      char pk_buf[16];
      uint32_t pk_sz = 0;
      int32_t st = kv_idx_primary_key((uint32_t)h, 0, pk_buf, sizeof(pk_buf), &pk_sz);
      check(st == 0, "idx_prikey: status");
      check(pk_sz == 9, "idx_prikey: size");
      check(memcmp(pk_buf, pri, 9) == 0, "idx_prikey: data mismatch");
      kv_idx_destroy((uint32_t)h);
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
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
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
      kv_set(test_table_id, self, key, sizeof(key), nullptr, 0);

      char buf[16];
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
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
         int32_t sz = kv_get(test_table_id, self, key, 1, buf, sizeof(buf));
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
         int32_t sz = kv_get(test_table_id, self, key, 8, buf, sizeof(buf));
         check(sz == (int32_t)sizeof(val), "multikey: 8B size");
         check(memcmp(buf, val, sizeof(val)) == 0, "multikey: 8B data");
      }

      // 24-byte key (standard format size)
      {
         char key[24];
         memset(key, 0x1A, 24);
         const char val[] = "twentyfour_byte_key";
         kv_put(self, key, 24, val, sizeof(val));
         char buf[32];
         int32_t sz = kv_get(test_table_id, self, key, 24, buf, sizeof(buf));
         check(sz == (int32_t)sizeof(val), "multikey: 24B size");
         check(memcmp(buf, val, sizeof(val)) == 0, "multikey: 24B data");
      }

      // 100-byte key (large key)
      {
         char key[100];
         memset(key, 0x1B, 100);
         const char val[] = "hundred_byte_key";
         kv_put(self, key, 100, val, sizeof(val));
         char buf[32];
         int32_t sz = kv_get(test_table_id, self, key, 100, buf, sizeof(buf));
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
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), read_buf, sizeof(read_buf));
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
      uint32_t h = kv_it_create(test_table_id, self, prefix, 1);
      kv_it_destroy(h);

      // After destroy, the handle slot is freed.
      // Create a new iterator — should succeed (reuses the freed slot or another).
      uint32_t h2 = kv_it_create(test_table_id, self, prefix, 1);
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
         uint32_t h = kv_it_create(test_table_id, self, prefix, 1);
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
      uint32_t h = kv_it_create(test_table_id, self, prefix, 1);
      kv_it_lower_bound(h, key1, sizeof(key1));
      check(kv_it_status(h) == 0, "iterasedinv: on key1");

      // Erase key1 while iterator points to it
      kv_erase(test_table_id, key1, sizeof(key1));

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

   // ─── 29. tstitexhaust: allocate 1024 iterators (max pool) ─────────────────
   [[sysio::action]]
   void tstitexhaust() {
      uint64_t self = get_self().value;
      const char prefix[] = {0x23};

      // Allocate all 1024 slots
      for (int i = 0; i < 1024; ++i) {
         kv_it_create(test_table_id, self, prefix, 1);
      }
      // Note: iterators destroyed automatically at end of transaction
   }

   // ─── 30. tstitexhfail: try to allocate 1025th iterator (should abort) ────
   [[sysio::action]]
   void tstitexhfail() {
      uint64_t self = get_self().value;
      const char prefix[] = {0x24};

      // Allocate all 1024 slots
      for (int i = 0; i < 1024; ++i) {
         kv_it_create(test_table_id, self, prefix, 1);
      }
      // 1025th should fail — this line triggers the exception
      kv_it_create(test_table_id, self, prefix, 1);
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
      uint32_t h = kv_it_create(test_table_id, self, prefix, 1);

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
      uint32_t h = kv_it_create(test_table_id, self, prefix, 1);
      // Should be at end immediately since no rows match
      check(kv_it_status(h) == 1, "itempttbl: status should be 1 (end)");
      kv_it_destroy(h);
   }

   // ─── 33. tstwriteperm: kv_set with a different table_id and payer field ──
   [[sysio::action]]
   void tstwriteperm() {
      uint64_t self = get_self().value;
      static constexpr uint32_t alt_table_id = 43;
      // Use a different table_id to verify table_id isolation
      char key[24];
      memset(key, 0, 24);
      key[0] = 0x28;
      const char val[] = "alt_table_test";
      int64_t delta = kv_set(alt_table_id, self, key, 24, val, sizeof(val));
      check(delta > 0, "writeperm: delta should be positive for new row");

      char buf[64];
      int32_t sz = kv_get(alt_table_id, self, key, 24, buf, sizeof(buf));
      check(sz == (int32_t)sizeof(val), "writeperm: size mismatch");
      check(memcmp(buf, val, sizeof(val)) == 0, "writeperm: data mismatch");
   }

   // ─── 34. testpayer: kv_set with explicit self payer, verify delta ────────
   [[sysio::action]]
   void testpayer() {
      uint64_t self = get_self().value;
      // Payer = self via kv_put helper
      const char k1[] = {0x29, 0x00, 0x01};
      int64_t d1 = kv_put(self, k1, sizeof(k1), "p1", 2);
      check(d1 > 0, "payer: create delta should be positive");

      char buf[16];
      int32_t sz = kv_get(test_table_id, self, k1, sizeof(k1), buf, sizeof(buf));
      check(sz == 2, "payer: self payer read size");

      // Payer = self via explicit kv_set
      const char k2[] = {0x29, 0x00, 0x02};
      int64_t d2 = kv_set(test_table_id, self, k2, sizeof(k2), "p2val", 5);
      check(d2 > 0, "payer: explicit self payer delta should be positive");

      sz = kv_get(test_table_id, self, k2, sizeof(k2), buf, sizeof(buf));
      check(sz == 5, "payer: explicit payer read size");

      // Update with smaller value — delta should be negative
      int64_t d3 = kv_set(test_table_id, self, k2, sizeof(k2), "x", 1);
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
      int32_t sz = kv_get(test_table_id, self, key, 256, buf, sizeof(buf));
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
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
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
      int32_t actual_sz = kv_get(test_table_id, self, key, sizeof(key), buf, 4);
      // actual_sz should be the full value size
      check(actual_sz == (int32_t)sizeof(val), "partread: should return full size");
      // Buffer should contain first 4 bytes
      check(memcmp(buf, val, 4) == 0, "partread: partial data mismatch");

      // Read with zero-size buffer — should just return size
      int32_t sz_only = kv_get(test_table_id, self, key, sizeof(key), nullptr, 0);
      check(sz_only == (int32_t)sizeof(val), "partread: zero-buf should return size");
   }

   // ─── 39. testzeroval: kv_set with value_size=0, kv_get returns 0 ─────────
   [[sysio::action]]
   void testzeroval() {
      uint64_t self = get_self().value;
      const char key[] = {0x2E, 0x00, 0x01};
      kv_set(test_table_id, self, key, sizeof(key), nullptr, 0);

      char buf[16];
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 0, "zeroval: should return 0 for empty value");
      check(kv_contains(test_table_id, self, key, sizeof(key)) == 1, "zeroval: should still exist");
   }

   // ─── 40. tstvalreplce: overwrite with different value sizes ──────────────
   [[sysio::action]]
   void tstvalreplce() {
      uint64_t self = get_self().value;
      const char key[] = {0x2F, 0x00, 0x01};

      // Start small
      kv_put(self, key, sizeof(key), "ab", 2);
      char buf[64];
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 2, "valreplace: initial size");

      // Grow
      const char bigger[] = "abcdefghijklmnop";
      kv_put(self, key, sizeof(key), bigger, 16);
      sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 16, "valreplace: grown size");
      check(memcmp(buf, bigger, 16) == 0, "valreplace: grown data");

      // Shrink
      kv_put(self, key, sizeof(key), "x", 1);
      sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
      check(sz == 1, "valreplace: shrunk size");
      check(buf[0] == 'x', "valreplace: shrunk data");
   }

   // ─── 41. tsttableid: kv_set with different table_ids isolates data ──────
   [[sysio::action]]
   void tstkeyfrmt() {
      uint64_t self = get_self().value;

      // table_id 42 (test_table_id)
      const char k0[] = {0x31, 0x00, 0x01};
      kv_set(test_table_id, self, k0, sizeof(k0), "tid42", 5);
      char buf[16];
      int32_t sz = kv_get(test_table_id, self, k0, sizeof(k0), buf, sizeof(buf));
      check(sz == 5, "tableid: tid42 size");
      check(memcmp(buf, "tid42", 5) == 0, "tableid: tid42 data");

      // table_id 99 (different namespace)
      static constexpr uint32_t other_table_id = 99;
      kv_set(other_table_id, self, k0, sizeof(k0), "tid99", 5);
      sz = kv_get(other_table_id, self, k0, sizeof(k0), buf, sizeof(buf));
      check(sz == 5, "tableid: tid99 size");
      check(memcmp(buf, "tid99", 5) == 0, "tableid: tid99 data");

      // Verify isolation: table_id 42 still has its own value
      sz = kv_get(test_table_id, self, k0, sizeof(k0), buf, sizeof(buf));
      check(sz == 5, "tableid: tid42 still 5");
      check(memcmp(buf, "tid42", 5) == 0, "tableid: tid42 data unchanged");
   }

   // ─── 43. testidxmulti: multiple secondary indices (separate table_ids) ───
   [[sysio::action]]
   void testidxmulti() {
      uint64_t self = get_self().value;
      // Each secondary index gets its own table_id
      static constexpr uint32_t sec_tid_name = 200;
      static constexpr uint32_t sec_tid_age  = 201;
      static constexpr uint32_t sec_tid_loc  = 202;
      const char pri[] = {0x32, 0x00, 0x01};

      // Store 3 different secondary keys on 3 different table_ids
      kv_idx_store(0, sec_tid_name, pri, 3, "name_alice", 10);
      kv_idx_store(0, sec_tid_age,  pri, 3, "age_30", 6);
      kv_idx_store(0, sec_tid_loc,  pri, 3, "loc_nyc", 7);

      // Find on each index independently
      int32_t h0 = kv_idx_find_secondary(self, sec_tid_name, "name_alice", 10);
      check(h0 >= 0, "idxmulti: idx0 find should succeed");
      char pk[16]; uint32_t pk_sz = 0;
      kv_idx_primary_key((uint32_t)h0, 0, pk, sizeof(pk), &pk_sz);
      check(pk_sz == 3, "idxmulti: idx0 prikey size");
      check(memcmp(pk, pri, 3) == 0, "idxmulti: idx0 prikey");
      kv_idx_destroy((uint32_t)h0);

      int32_t h1 = kv_idx_find_secondary(self, sec_tid_age, "age_30", 6);
      check(h1 >= 0, "idxmulti: idx1 find should succeed");
      kv_idx_primary_key((uint32_t)h1, 0, pk, sizeof(pk), &pk_sz);
      check(pk_sz == 3, "idxmulti: idx1 prikey size");
      kv_idx_destroy((uint32_t)h1);

      int32_t h2 = kv_idx_find_secondary(self, sec_tid_loc, "loc_nyc", 7);
      check(h2 >= 0, "idxmulti: idx2 find should succeed");
      kv_idx_primary_key((uint32_t)h2, 0, pk, sizeof(pk), &pk_sz);
      check(pk_sz == 3, "idxmulti: idx2 prikey size");
      kv_idx_destroy((uint32_t)h2);
   }

   // ─── 44. testidxdupsk: multiple rows with same secondary key ──────────────
   [[sysio::action]]
   void testidxdupsk() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 109;

      // 3 rows with same secondary key "shared"
      for (int i = 0; i < 3; ++i) {
         char pri[3] = {0x33, 0x00, (char)(i + 1)};
         kv_idx_store(0, sec_tid, pri, 3, "shared", 6);
      }

      // lower_bound on "shared", iterate, count all
      int32_t h = kv_idx_lower_bound(self, sec_tid, "shared", 6);
      check(h >= 0, "idxdupsk: lower_bound should find entry");
      uint32_t count = 0;
      while (true) {
         char sk[16]; uint32_t sk_sz = 0;
         int32_t st = kv_idx_key((uint32_t)h, 0, sk, sizeof(sk), &sk_sz);
         if (st != 0 || sk_sz != 6 || memcmp(sk, "shared", 6) != 0)
            break;
         ++count;
         if (kv_idx_next((uint32_t)h) != 0)
            break;
      }
      check(count == 3, "idxdupsk: should find 3 rows with same sec key");
      kv_idx_destroy((uint32_t)h);
   }

   // ─── 45. testidxrange: lower_bound + iterate through range ────────────────
   [[sysio::action]]
   void testidxrange() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 110;

      // Store 5 entries with sec keys "a","b","c","d","e"
      for (int i = 0; i < 5; ++i) {
         char sec[1] = {(char)('a' + i)};
         char pri[3] = {0x34, 0x00, (char)(i + 1)};
         kv_idx_store(0, sec_tid, pri, 3, sec, 1);
      }

      // Range [b, d]: lower_bound("b"), iterate while sec_key <= "d"
      int32_t h = kv_idx_lower_bound(self, sec_tid, "b", 1);
      check(h >= 0, "idxrange: lower_bound should find entry");
      uint32_t count = 0;
      while (true) {
         char sk[4]; uint32_t sk_sz = 0;
         int32_t st = kv_idx_key((uint32_t)h, 0, sk, sizeof(sk), &sk_sz);
         if (st != 0) break;
         if (sk_sz == 1 && sk[0] > 'd') break;
         ++count;
         if (kv_idx_next((uint32_t)h) != 0) break;
      }
      check(count == 3, "idxrange: should find b,c,d = 3 entries");
      kv_idx_destroy((uint32_t)h);
   }

   // ─── 46. testidxempty: query secondary index with no entries ──────────────
   [[sysio::action]]
   void testidxempty() {
      uint64_t self = get_self().value;
      static constexpr uint32_t sec_tid = 111;

      // lower_bound on a table with no entries — returns -1 (not found)
      int32_t h = kv_idx_lower_bound(self, sec_tid, "anything", 8);
      check(h < 0, "idxempty: should return -1 for empty table");
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
      uint32_t h1 = kv_it_create(test_table_id, self, prefix, 1);
      uint32_t h2 = kv_it_create(test_table_id, self, prefix, 1);

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
      uint32_t h = kv_it_create(test_table_id, self, prefix, 1);
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
      uint32_t ha = kv_it_create(test_table_id, self, pa, 1);
      char seek_a[] = {0x37, 0x00};
      kv_it_lower_bound(ha, seek_a, 2);
      uint32_t count_a = 0;
      while (kv_it_status(ha) == 0) { ++count_a; kv_it_next(ha); }
      check(count_a == 2, "multitbl: table A should have 2 rows");
      kv_it_destroy(ha);

      // Iterate table B — should see 1 row
      const char pb[] = {0x38};
      uint32_t hb = kv_it_create(test_table_id, self, pb, 1);
      char seek_b[] = {0x38, 0x00};
      kv_it_lower_bound(hb, seek_b, 2);
      uint32_t count_b = 0;
      while (kv_it_status(hb) == 0) { ++count_b; kv_it_next(hb); }
      check(count_b == 1, "multitbl: table B should have 1 row");
      kv_it_destroy(hb);
   }

   // ─── 50. testscoped: different table_ids provide scope isolation ─────────
   [[sysio::action]]
   void testscoped() {
      uint64_t self = get_self().value;
      static constexpr uint32_t tid_scope_a = 44;
      static constexpr uint32_t tid_scope_b = 45;

      // Same key written to two different table_ids
      char key[9];
      key[0] = 0x39;
      encode_u64(100, key + 1);

      kv_set(tid_scope_a, self, key, 9, "scopeA", 6);
      kv_set(tid_scope_b, self, key, 9, "scopeB", 6);

      // Read each and verify isolation
      char buf[16];
      int32_t sz = kv_get(tid_scope_a, self, key, 9, buf, sizeof(buf));
      check(sz == 6, "scoped: A size");
      check(memcmp(buf, "scopeA", 6) == 0, "scoped: A data");

      sz = kv_get(tid_scope_b, self, key, 9, buf, sizeof(buf));
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
      int32_t sz = kv_get(test_table_id, self, key, sizeof(key), buf, sizeof(buf));
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
      uint32_t h = kv_it_create(test_table_id, self, prefix, 1);

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

   // (kv::raw_table and old kv::table tests removed — types dropped in table_id migration)

   // ════════════════════════════════════════════════════════════════════════════
   // Payer validation tests
   // ════════════════════════════════════════════════════════════════════════════

   // ─── tstpayerself: kv_set with payer=0 (self) always works ─────────────────
   [[sysio::action]]
   void tstpayerself() {
      char key[] = "payerself";
      char val[] = "data";
      // payer=0 means receiver pays — should always work
      kv_set(test_table_id, 0, key, sizeof(key), val, sizeof(val));
      check(kv_contains(test_table_id, get_self().value, key, sizeof(key)) != 0, "tstpayerself: key should exist");
   }

   // ─── tstpayeroth: kv_set with non-zero payer — fails at transaction level
   // if payer has not authorized the action (unauthorized_ram_usage_increase)
   [[sysio::action]]
   void tstpayeroth() {
      char key[] = "payerother";
      char val[] = "data";
      kv_set(test_table_id, "alice"_n.value, key, sizeof(key), val, sizeof(val));
   }

   // ─── tstemptykey: empty key (size=0) must be rejected ──────────────────────
   [[sysio::action]]
   void tstemptykey() {
      char val[] = "data";
      // This should throw — empty keys are not allowed
      kv_set(test_table_id, 0, nullptr, 0, val, sizeof(val));
   }

   // ─── tstbadfmt: table_id > UINT16_MAX is rejected ───────────────────────
   // The chain validates table_id fits in uint16_t. Values > 65535 abort.
   // This action is expected to fail with kv_key_too_large.
   [[sysio::action]]
   void tstbadfmt() {
      char key[] = "badfmt";
      char val[] = "data";
      // 65536 > UINT16_MAX → should be rejected by checked_table_id
      kv_set(65536, 0, key, sizeof(key), val, sizeof(val));
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
      kv_set(test_table_id, 0, key, sizeof(key), val1, sizeof(val1));
      uint32_t h = kv_it_create(test_table_id, get_self().value, prefix, sizeof(prefix));
      int32_t st = kv_it_lower_bound(h, key, sizeof(key));
      check(st == 0, "tstreraseins: should find key");

      // Erase and reinsert with different value
      kv_erase(test_table_id, key, sizeof(key));
      kv_set(test_table_id, 0, key, sizeof(key), val2, sizeof(val2));

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

      kv_set(test_table_id, 0, key, sizeof(key), val, sizeof(val));

      uint32_t h = kv_it_create(test_table_id, get_self().value, prefix, sizeof(prefix));
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

      kv_set(test_table_id, 0, key1, sizeof(key1), val, sizeof(val));
      kv_set(test_table_id, 0, key2, sizeof(key2), val, sizeof(val));

      // Position iterator at key2
      uint32_t h = kv_it_create(test_table_id, get_self().value, prefix, sizeof(prefix));
      kv_it_lower_bound(h, key2, sizeof(key2));
      check(kv_it_status(h) == 0, "tstpreveras: should be at key2");

      // Erase key2
      kv_erase(test_table_id, key2, sizeof(key2));

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
      kv_set(test_table_id, 0, key, sizeof(key), val, sizeof(val));
   }

   // ─── tstkeyoffbnd: kv_it_key with offset >= key_size returns 0 bytes ──────
   [[sysio::action]]
   void tstkeyoffbnd() {
      char prefix[] = {(char)0xF1};
      char key[] = {(char)0xF1, 0x01, 0x02};
      char val[] = "data";
      kv_set(test_table_id, 0, key, sizeof(key), val, sizeof(val));

      uint32_t h = kv_it_create(test_table_id, get_self().value, prefix, sizeof(prefix));
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
      kv_idx_store(0, test_sec_table_id, pri_key, sizeof(pri_key), sec_key, sizeof(sec_key));
   }

   // ─── tstnotifyram: write in notification context bills receiver's RAM ─────
   [[sysio::action]]
   void tstnotifyram() {
      // When receiving a notification (receiver != act.account),
      // kv_set writes to receiver's KV namespace and bills receiver
      char key[] = "notifykey";
      char val[] = "notifyval";
      kv_set(test_table_id, 0, key, sizeof(key), val, sizeof(val));
   }

   // ─── tstsendnotif: send notification to trigger tstnotifyram on another account
   [[sysio::action]]
   void tstsendnotif() {
      // This sends a notification to 'kvnotify' account which also has this contract
      require_recipient("kvnotify"_n);
   }

   // ════════════════════════════════════════════════════════════════════════════
   // Secondary index edge-case tests
   // ════════════════════════════════════════════════════════════════════════════

   // ─── tstsecclone: copy secondary iterator with duplicate secondary keys ──
   // When multiple rows share the same secondary key, a copied iterator must
   // preserve the exact position (matching primary key).
   [[sysio::action]]
   void tstsecclone() {
      sec_table t(get_self(), "secclone"_n.value);
      // Three rows with the SAME secondary key (age=42)
      t.emplace(get_self(), [](sec_row& r) { r.pk = 10; r.age = 42; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 20; r.age = 42; });
      t.emplace(get_self(), [](sec_row& r) { r.pk = 30; r.age = 42; });

      auto idx = t.get_index<"byage"_n>();
      auto it = idx.begin();
      check(it != idx.end() && it->pk == 10, "tstsecclone: first should be pk 10");

      // Advance to second entry (pk=20)
      ++it;
      check(it->pk == 20, "tstsecclone: second should be pk 20");

      // Copy the iterator — must land on pk=20, not pk=10
      auto it_copy = it;
      check(it_copy->pk == 20, "tstsecclone: copy must preserve position at pk 20");

      // Advance the copy — should go to pk=30
      ++it_copy;
      check(it_copy->pk == 30, "tstsecclone: copy++ should be pk 30");

      // Original should still be at pk=20
      check(it->pk == 20, "tstsecclone: original should still be pk 20");
   }

   // ─── tstsecrbig: reverse iterate secondary index with uint128_t keys ─────
   // operator-- from end must use a max_sec buffer large enough for the
   // secondary key type. uint128_t keys are 16 bytes, not 8.
   struct [[sysio::table]] sec_big_row {
      uint64_t pk;
      uint128_t score;
      uint64_t primary_key() const { return pk; }
      uint128_t by_score() const { return score; }
      SYSLIB_SERIALIZE(sec_big_row, (pk)(score))
   };
   using sec_big_table = sysio::multi_index<"secbigtbl"_n, sec_big_row,
      sysio::indexed_by<"byscore"_n, sysio::const_mem_fun<sec_big_row, uint128_t, &sec_big_row::by_score>>
   >;

   [[sysio::action]]
   void tstsecrbig() {
      sec_big_table t(get_self(), "secrbig"_n.value);
      // Use values > 2^64 to ensure uint128_t encoding matters
      uint128_t lo  = uint128_t(1) << 100;
      uint128_t mid = uint128_t(1) << 110;
      uint128_t hi  = uint128_t(1) << 120;
      t.emplace(get_self(), [&](sec_big_row& r) { r.pk = 1; r.score = lo; });
      t.emplace(get_self(), [&](sec_big_row& r) { r.pk = 2; r.score = mid; });
      t.emplace(get_self(), [&](sec_big_row& r) { r.pk = 3; r.score = hi; });

      auto idx = t.get_index<"byscore"_n>();

      // rbegin should be the highest score (hi, pk=3)
      auto rit = idx.rbegin();
      check(rit != idx.rend(), "tstsecrbig: rbegin should not be rend");
      check(rit->pk == 3, "tstsecrbig: rbegin should be pk 3 (highest score)");
      ++rit;
      check(rit->pk == 2, "tstsecrbig: second should be pk 2");
      ++rit;
      check(rit->pk == 1, "tstsecrbig: third should be pk 1 (lowest score)");
      ++rit;
      check(rit == idx.rend(), "tstsecrbig: should be rend after 3");
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
      kv_set(test_table_id, 0, key, 4, val.data(), val_size);
   }

   // Update an existing row with a new value size
   [[sysio::action]]
   void ramupdate(uint32_t key_id, uint32_t val_size) {
      char key[4];
      memcpy(key, &key_id, 4);
      std::vector<char> val(val_size, 'Y');
      kv_set(test_table_id, 0, key, 4, val.data(), val_size);
   }

   // Erase a row
   [[sysio::action]]
   void ramerase(uint32_t key_id) {
      char key[4];
      memcpy(key, &key_id, 4);
      kv_erase(test_table_id, key, 4);
   }

   // ─── tstidxpayer: kv_set + kv_idx_store with explicit payer ──────────────
   // Verifies that the payer parameter flows through to both primary and
   // secondary index RAM billing.
   [[sysio::action]]
   void tstidxpayer(sysio::name payer) {
      // Primary row: 4-byte key, 10-byte value
      char pk[] = {0x50, 0x41, 0x59, 0x01};  // "PAY\x01"
      char val[] = "idxpayerv1";
      kv_set(test_table_id, payer.value, pk, sizeof(pk), val, sizeof(val));

      // Secondary index: 8-byte sec key, 4-byte pri key
      static constexpr uint32_t sec_tid = 112;
      char sec_key[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42};
      kv_idx_store(payer.value, sec_tid,
                   pk, sizeof(pk), sec_key, sizeof(sec_key));
   }

   // ═══════════════════════════════════════════════════════════════════════════
   // Cross-scope secondary index isolation tests
   // Verify that secondary index iteration in one scope does not leak entries
   // from another scope using the same table and overlapping primary keys.
   // ═══════════════════════════════════════════════════════════════════════════

   // ─── tstxscope: cross-scope isolation — iterate one scope, verify no leak ──
   [[sysio::action]]
   void tstxscope() {
      sec_table ta(get_self(), "xscope.a"_n.value);
      sec_table tb(get_self(), "xscope.b"_n.value);

      // Same PKs, different ages in each scope
      ta.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 100; });
      ta.emplace(get_self(), [](sec_row& r) { r.pk = 2; r.age = 200; });

      tb.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 300; });
      tb.emplace(get_self(), [](sec_row& r) { r.pk = 2; r.age = 400; });

      // Iterate scope A by secondary — should see exactly 100, 200
      auto idxA = ta.get_index<"byage"_n>();
      auto it = idxA.begin();
      check(it != idxA.end(),   "tstxscope: A begin valid");
      check(it->age == 100,     "tstxscope: A first age 100");
      ++it;
      check(it->age == 200,     "tstxscope: A second age 200");
      ++it;
      check(it == idxA.end(),   "tstxscope: A end after 2");

      // Iterate scope B — should see exactly 300, 400
      auto idxB = tb.get_index<"byage"_n>();
      auto itb = idxB.begin();
      check(itb != idxB.end(),  "tstxscope: B begin valid");
      check(itb->age == 300,    "tstxscope: B first age 300");
      ++itb;
      check(itb->age == 400,    "tstxscope: B second age 400");
      ++itb;
      check(itb == idxB.end(),  "tstxscope: B end after 2");
   }

   // ─── tstxfind: cross-scope find — find in A must not return B's entry ──────
   [[sysio::action]]
   void tstxfind() {
      sec_table ta(get_self(), "xfind.a"_n.value);
      sec_table tb(get_self(), "xfind.b"_n.value);

      ta.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 50; });
      tb.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 99; });

      auto idxA = ta.get_index<"byage"_n>();
      // find(99) in scope A should miss (only scope B has age=99)
      auto it = idxA.find(99);
      check(it == idxA.end(), "tstxfind: find(99) in scope A should be end");

      // find(50) in scope A should hit
      auto it2 = idxA.find(50);
      check(it2 != idxA.end(), "tstxfind: find(50) in scope A should exist");
      check(it2->pk == 1,      "tstxfind: found pk should be 1");
   }

   // ─── tstxerase: erase from scope A must not affect scope B ─────────────────
   [[sysio::action]]
   void tstxerase() {
      sec_table ta(get_self(), "xerase.a"_n.value);
      sec_table tb(get_self(), "xerase.b"_n.value);

      ta.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 77; });
      tb.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 77; });

      // Erase from scope A
      auto idxA = ta.get_index<"byage"_n>();
      auto it = idxA.find(77);
      check(it != idxA.end(), "tstxerase: A find(77) should exist");
      idxA.erase(it);

      // Scope A should be empty
      check(idxA.begin() == idxA.end(), "tstxerase: A should be empty");

      // Scope B should still have the entry
      auto idxB = tb.get_index<"byage"_n>();
      auto itb = idxB.find(77);
      check(itb != idxB.end(), "tstxerase: B find(77) should survive");
      check(itb->pk == 1,      "tstxerase: B pk should be 1");
   }

   // ─── tstxdbl: cross-scope isolation with double secondary ──────────────────
   struct [[sysio::table]] dbl_row {
      uint64_t pk;
      double   val;
      uint64_t primary_key() const { return pk; }
      double   by_val() const { return val; }
      SYSLIB_SERIALIZE(dbl_row, (pk)(val))
   };
   using dbl_table = sysio::multi_index<"dbltbl"_n, dbl_row,
      sysio::indexed_by<"byval"_n, sysio::const_mem_fun<dbl_row, double, &dbl_row::by_val>>
   >;

   [[sysio::action]]
   void tstxdbl() {
      dbl_table ta(get_self(), "xdbl.a"_n.value);
      dbl_table tb(get_self(), "xdbl.b"_n.value);

      ta.emplace(get_self(), [](dbl_row& r) { r.pk = 1; r.val = 1.5; });
      ta.emplace(get_self(), [](dbl_row& r) { r.pk = 2; r.val = 3.14; });
      tb.emplace(get_self(), [](dbl_row& r) { r.pk = 1; r.val = 2.71; });
      tb.emplace(get_self(), [](dbl_row& r) { r.pk = 2; r.val = -0.5; });

      // Iterate scope A by secondary — should see 1.5, 3.14 (sorted)
      auto idxA = ta.get_index<"byval"_n>();
      auto it = idxA.begin();
      check(it != idxA.end(),   "tstxdbl: A begin valid");
      check(it->val == 1.5,     "tstxdbl: A first 1.5");
      ++it;
      check(it->val == 3.14,    "tstxdbl: A second 3.14");
      ++it;
      check(it == idxA.end(),   "tstxdbl: A end after 2");

      // Iterate scope B — should see -0.5, 2.71 (sorted)
      auto idxB = tb.get_index<"byval"_n>();
      auto itb = idxB.begin();
      check(itb != idxB.end(),  "tstxdbl: B begin valid");
      check(itb->val == -0.5,   "tstxdbl: B first -0.5");
      ++itb;
      check(itb->val == 2.71,   "tstxdbl: B second 2.71");
      ++itb;
      check(itb == idxB.end(),  "tstxdbl: B end after 2");

      // Reverse iterate scope A — should see 3.14, 1.5
      auto rit = idxA.rbegin();
      check(rit != idxA.rend(), "tstxdbl: A rbegin valid");
      check(rit->val == 3.14,   "tstxdbl: A rbegin 3.14");
      ++rit;
      check(rit->val == 1.5,    "tstxdbl: A rnext 1.5");
      ++rit;
      check(rit == idxA.rend(), "tstxdbl: A rend after 2");
   }

   // ─── tstxzero: cross-scope isolation with scope=0 (minimum prefix) ────────
   [[sysio::action]]
   void tstxzero() {
      // scope 0 encodes as [0x00 x 8] prefix — lexicographic minimum
      sec_table t0(get_self(), 0);
      sec_table t1(get_self(), 1);

      t0.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 10; });
      t1.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 20; });

      auto idx0 = t0.get_index<"byage"_n>();
      auto it = idx0.begin();
      check(it != idx0.end(), "tstxzero: scope 0 begin valid");
      check(it->age == 10,    "tstxzero: scope 0 age 10");
      ++it;
      check(it == idx0.end(), "tstxzero: scope 0 only 1 entry");
   }

   // ─── tstxrev: reverse iteration must not leak across scopes ───────────────
   [[sysio::action]]
   void tstxrev() {
      sec_table ta(get_self(), "xrev.a"_n.value);
      sec_table tb(get_self(), "xrev.b"_n.value);

      ta.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 10; });
      ta.emplace(get_self(), [](sec_row& r) { r.pk = 2; r.age = 20; });
      tb.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 30; });

      // Reverse iterate scope A — should see 20, 10 only
      auto idxA = ta.get_index<"byage"_n>();
      auto rit = idxA.rbegin();
      check(rit != idxA.rend(), "tstxrev: A rbegin valid");
      check(rit->age == 20,     "tstxrev: A rbegin age 20");
      ++rit;
      check(rit->age == 10,     "tstxrev: A rnext age 10");
      ++rit;
      check(rit == idxA.rend(), "tstxrev: A rend after 2");
   }

   // ─── tstxubound: upper_bound in scope A stops at scope boundary ────────────
   [[sysio::action]]
   void tstxubound() {
      sec_table ta(get_self(), "xubound.a"_n.value);
      sec_table tb(get_self(), "xubound.b"_n.value);

      ta.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 10; });
      ta.emplace(get_self(), [](sec_row& r) { r.pk = 2; r.age = 20; });
      tb.emplace(get_self(), [](sec_row& r) { r.pk = 1; r.age = 15; });

      auto idxA = ta.get_index<"byage"_n>();
      // upper_bound(10) in scope A should land on age=20, not on B's age=15
      auto it = idxA.upper_bound(10);
      check(it != idxA.end(), "tstxubound: upper_bound(10) should exist");
      check(it->age == 20,    "tstxubound: should be age 20, not 15 from scope B");
      ++it;
      check(it == idxA.end(), "tstxubound: end after upper_bound advance");
   }
};
