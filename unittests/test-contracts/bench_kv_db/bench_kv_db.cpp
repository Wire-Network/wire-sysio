// Benchmark contract using KV intrinsics directly (hand-written extern "C" declarations).
// This avoids dependency on CDT kv.h which isn't installed yet.

#include <sysio/sysio.hpp>
#include <cstring>

// key_format 0 = raw bytes (used by all operations in this contract)
static constexpr uint32_t key_format = 0;

// Hand-written KV intrinsic declarations matching host interface.hpp signatures.
// legacy_span<T> maps to (ptr, size) pair in WASM ABI.
extern "C" {
   __attribute__((sysio_wasm_import))
   int64_t kv_set(uint32_t key_format, uint64_t payer, const void* key, uint32_t key_size, const void* value, uint32_t value_size);

   __attribute__((sysio_wasm_import))
   int32_t kv_get(uint32_t key_format, uint64_t code, const void* key, uint32_t key_size, void* value, uint32_t value_size);

   __attribute__((sysio_wasm_import))
   int64_t kv_erase(uint32_t key_format, const void* key, uint32_t key_size);

   __attribute__((sysio_wasm_import))
   int32_t kv_contains(uint32_t key_format, uint64_t code, const void* key, uint32_t key_size);

   __attribute__((sysio_wasm_import))
   uint32_t kv_it_create(uint32_t key_format, uint64_t code, const void* prefix, uint32_t prefix_size);

   __attribute__((sysio_wasm_import))
   void kv_it_destroy(uint32_t handle);

   __attribute__((sysio_wasm_import))
   int32_t kv_it_next(uint32_t handle);

   __attribute__((sysio_wasm_import))
   int32_t kv_it_key(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size, uint32_t* actual_size);

   __attribute__((sysio_wasm_import))
   int32_t kv_it_value(uint32_t handle, uint32_t offset, void* dest, uint32_t dest_size, uint32_t* actual_size);
}

using namespace sysio;

// Encode uint64_t as 8 bytes big-endian
static void encode_key(uint64_t id, char* buf) {
   for (int i = 7; i >= 0; --i) {
      buf[i] = static_cast<char>(id & 0xFF);
      id >>= 8;
   }
}

// Simple row payload matching legacy benchmark
struct row_payload {
   uint64_t    value1;
   uint64_t    value2;
   char        data[28]; // "benchmark_payload_data_here" + null

   row_payload() : value1(0), value2(0) {
      memcpy(data, "benchmark_payload_data_here\0", 28);
   }
};

class [[sysio::contract("bench_kv_db")]] bench_kv_db : public contract {
public:
   using contract::contract;

   [[sysio::action]]
   void populate(uint32_t count) {
      for (uint32_t i = 0; i < count; ++i) {
         char key[8];
         encode_key(i, key);

         row_payload payload;
         payload.value1 = count - i;
         payload.value2 = i * 7;

         kv_set(0, get_self().value, key, 8, &payload, sizeof(payload));
      }
   }

   [[sysio::action]]
   void findall(uint32_t count) {
      uint64_t code = get_self().value;
      for (uint32_t i = 0; i < count; ++i) {
         char key[8];
         encode_key(i, key);

         row_payload payload;
         int32_t sz = kv_get(key_format, code, key, 8, &payload, sizeof(payload));
         check(sz > 0, "row not found");
         check(payload.value2 == i * 7, "bad value");
      }
   }

   [[sysio::action]]
   void iterall() {
      uint64_t code = get_self().value;
      uint32_t handle = kv_it_create(key_format, code, nullptr, 0);

      uint32_t count = 0;
      int32_t status = 0; // 0 = OK
      while (status == 0) {
         ++count;
         status = kv_it_next(handle);
      }

      kv_it_destroy(handle);
      check(count > 0, "no rows");
   }

   [[sysio::action]]
   void updateall(uint32_t count) {
      uint64_t code = get_self().value;
      for (uint32_t i = 0; i < count; ++i) {
         char key[8];
         encode_key(i, key);

         // Read existing
         row_payload payload;
         kv_get(key_format, code, key, 8, &payload, sizeof(payload));

         // Modify and write back
         payload.value2 = i * 13;
         kv_set(0, get_self().value, key, 8, &payload, sizeof(payload));
      }
   }

   [[sysio::action]]
   void eraseall(uint32_t count) {
      for (uint32_t i = 0; i < count; ++i) {
         char key[8];
         encode_key(i, key);
         kv_erase(key_format, key, 8);
      }
   }
};
