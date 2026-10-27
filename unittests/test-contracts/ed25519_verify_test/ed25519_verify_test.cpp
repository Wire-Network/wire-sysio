#include "ed25519_verify_test.hpp"
#include "test_config.hpp"

#include <array>
#include <cstring>
#include <limits>

extern "C" {
/// One signature, public key, and raw message; returns 0 on success, -1 on verification failure.
__attribute__((sysio_wasm_import))
int32_t ed25519_verify(const char* sig, uint32_t sig_len, const char* pub, uint32_t pub_len,
                       const char* msg, uint32_t msg_len);

/// Store a marker before the operation that must abort the transaction.
__attribute__((sysio_wasm_import)) int64_t kv_set(uint32_t table_id, uint64_t payer, const void* key, uint32_t key_size,
                                                  const void* value, uint32_t value_size);

/// Inspect the marker after rollback without changing state.
__attribute__((sysio_wasm_import)) int32_t kv_get(uint32_t table_id, uint64_t code, const void* key, uint32_t key_size,
                                                  void* value, uint32_t value_size);
}

namespace {
constexpr int32_t success = 0;
constexpr int32_t failure = -1;
constexpr uint32_t signature_buffer = 0;
constexpr uint32_t key_buffer = 1;
constexpr uint32_t message_buffer = 2;
constexpr uint32_t buffer_count = 3;
constexpr uint32_t signature_size = 64;
constexpr uint32_t public_key_size = 32;

/// Forward separate buffers without changing the signed message.
int32_t verify_one(const ed25519_input& entry) {
   return ed25519_verify(entry.signature.data(), entry.signature.size(), entry.public_key.data(),
                         entry.public_key.size(), entry.message.data(), entry.message.size());
}
}

void ed25519_verify_test::verify(const ed25519_input& entry, int32_t expected, bool unaligned) {
   if (!unaligned) {
      sysio::check(verify_one(entry) == expected, "unexpected Ed25519 verification result");
      return;
   }
   std::array<bytes, buffer_count> storage{entry.signature, entry.public_key, entry.message};
   for (auto& buffer : storage)
      buffer.insert(buffer.begin(), 0);
   sysio::check(ed25519_verify(storage[signature_buffer].data() + 1, entry.signature.size(),
                               storage[key_buffer].data() + 1, entry.public_key.size(),
                               storage[message_buffer].data() + 1, entry.message.size()) == expected,
                "unexpected unaligned Ed25519 result");
}

void ed25519_verify_test::batch(const std::vector<ed25519_input>& entries, int32_t expected) {
   int32_t actual = entries.empty() ? failure : success;
   for (const auto& entry : entries) {
      actual = verify_one(entry);
      if (actual != success)
         break;
   }
   sysio::check(actual == expected, "unexpected Ed25519 batch result");
}

void ed25519_verify_test::boundary(const ed25519_input& entry, uint32_t buffer, bool overrun) {
   std::array<const char*, buffer_count> data{entry.signature.data(), entry.public_key.data(), entry.message.data()};
   std::array<uint32_t, buffer_count> sizes{uint32_t(entry.signature.size()), uint32_t(entry.public_key.size()),
                                          uint32_t(entry.message.size())};
   sysio::check(buffer < buffer_count && sizes[buffer] < ed25519_test::wasm_page_size, "invalid boundary probe");
   const auto old_pages = __builtin_wasm_memory_grow(0, 1);
   sysio::check(old_pages != std::numeric_limits<size_t>::max(), "cannot grow probe memory");
   const uintptr_t end = (old_pages + 1) * ed25519_test::wasm_page_size;
   auto* target = reinterpret_cast<char*>(end - sizes[buffer]);
   if (sizes[buffer])
      std::memcpy(target, data[buffer], sizes[buffer]);
   data[buffer] = target;
   sizes[buffer] += overrun ? 1 : 0;
   sysio::check(ed25519_verify(data[signature_buffer], sizes[signature_buffer], data[key_buffer], sizes[key_buffer],
                               data[message_buffer], sizes[message_buffer]) == success, "boundary input must verify");
}

void ed25519_verify_test::badptr(uint32_t buffer) {
   std::array<char, signature_size> storage{};
   std::array<const char*, buffer_count> data{storage.data(), storage.data(), storage.data()};
   std::array<uint32_t, buffer_count> sizes{signature_size, public_key_size, 1};
   sysio::check(buffer < buffer_count, "invalid buffer selector");
   data[buffer] = reinterpret_cast<const char*>(std::numeric_limits<uint32_t>::max());
   sizes[buffer] = std::numeric_limits<uint32_t>::max();
   ed25519_verify(data[signature_buffer], sizes[signature_buffer], data[key_buffer], sizes[key_buffer],
                  data[message_buffer], sizes[message_buffer]);
   sysio::check(false, "invalid pointer unexpectedly returned");
}

void ed25519_verify_test::stress(const ed25519_input& entry) {
   using namespace ed25519_test;
   kv_set(marker_table, get_self().value, &marker_key, sizeof(marker_key), &marker_value, sizeof(marker_value));
   sysio::print(ready_message);
   for (uint32_t i = 0; i < stress_count; ++i)
      sysio::check(verify_one(entry) == success, "stress signature must verify");
   sysio::check(false, "stress loop unexpectedly completed");
}

void ed25519_verify_test::clean() {
   using namespace ed25519_test;
   char value = 0;
   sysio::check(kv_get(marker_table, get_self().value, &marker_key, sizeof(marker_key), &value, sizeof(value)) == -1,
                "timed-out action left a marker");
}
