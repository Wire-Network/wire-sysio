#include "ed25519_verify_test.hpp"

#include "test_config.hpp"

#include <cstring>
#include <limits>
#include <memory>

extern "C" {
/// Packed records: [u32 LE message size][signature:64][public key:32][raw message].
__attribute__((sysio_wasm_import)) bool ed25519_verify(const char* entries, uint32_t entries_len, uint32_t count);

/// Store a marker before the operation that must abort the transaction.
__attribute__((sysio_wasm_import)) int64_t kv_set(uint32_t table_id, uint64_t payer, const void* key, uint32_t key_size,
                                                  const void* value, uint32_t value_size);

/// Inspect the marker after rollback without changing state.
__attribute__((sysio_wasm_import)) int32_t kv_get(uint32_t table_id, uint64_t code, const void* key, uint32_t key_size,
                                                  void* value, uint32_t value_size);
}

void ed25519_verify_test::verify(const bytes& entries, uint32_t count, bool expected, bool unaligned) {
   bytes storage(entries.size() + 1);
   const char* data = entries.data();
   if (unaligned) {
      std::memcpy(storage.data() + 1, data, entries.size());
      data = storage.data() + 1;
   }
   sysio::check(ed25519_verify(data, entries.size(), count) == expected, "unexpected Ed25519 verification result");
}

void ed25519_verify_test::boundary(const bytes& entry, bool overrun) {
   sysio::check(!entry.empty() && entry.size() < ed25519_test::wasm_page_size, "invalid boundary probe record");
   const auto old_pages = __builtin_wasm_memory_grow(0, 1);
   sysio::check(old_pages != std::numeric_limits<size_t>::max(), "cannot grow probe memory");
   const uintptr_t end = (old_pages + 1) * ed25519_test::wasm_page_size;
   auto* data = reinterpret_cast<char*>(end - entry.size());
   std::memcpy(data, entry.data(), entry.size());
   const bool valid = ed25519_verify(data, entry.size() + (overrun ? 1 : 0), 1);
   sysio::check(valid, "boundary record must verify");
}

void ed25519_verify_test::badptr() {
   ed25519_verify(reinterpret_cast<const char*>(std::numeric_limits<uint32_t>::max()), ed25519_test::empty_record_size,
                  1);
   sysio::check(false, "invalid pointer unexpectedly returned");
}

void ed25519_verify_test::stress(const bytes& empty_record) {
   using namespace ed25519_test;
   sysio::check(empty_record.size() == empty_record_size, "stress requires a signed empty message");
   const size_t entries_size = empty_record.size() * stress_count;
   // Every byte is filled below; avoid an interpreter-side zero-initialization loop.
   std::unique_ptr<char[]> entries(new char[entries_size]);
   static_assert((stress_count & (stress_count - 1)) == 0);
   std::memcpy(entries.get(), empty_record.data(), empty_record.size());
   // Doubling avoids spending the transaction budget in a contract-side preparation loop.
   for (size_t filled = empty_record.size(); filled < entries_size; filled *= 2)
      std::memcpy(entries.get() + filled, entries.get(), filled);
   kv_set(marker_table, get_self().value, &marker_key, sizeof(marker_key), &marker_value, sizeof(marker_value));
   sysio::print(ready_message);
   ed25519_verify(entries.get(), entries_size, stress_count);
   sysio::check(false, "stress batch unexpectedly completed");
}

void ed25519_verify_test::clean() {
   using namespace ed25519_test;
   char value = 0;
   sysio::check(kv_get(marker_table, get_self().value, &marker_key, sizeof(marker_key), &value, sizeof(value)) == -1,
                "timed-out action left a marker");
}
