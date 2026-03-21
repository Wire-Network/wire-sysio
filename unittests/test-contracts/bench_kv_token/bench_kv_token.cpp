/**
 * Optimized token contract using raw KV intrinsics with zero serialization.
 * Stores balance as raw 16 bytes (amount + symbol). Key follows SHiP-compatible
 * encoding: [table:8B][scope:8B][pk:8B] = 24 bytes.
 */
#include <sysio/sysio.hpp>
#include <cstring>

extern "C" {
   __attribute__((sysio_wasm_import))
   int64_t kv_set(uint32_t key_format, uint64_t payer, const void* key, uint32_t key_size, const void* value, uint32_t value_size);
   __attribute__((sysio_wasm_import))
   int32_t kv_get(uint64_t code, const void* key, uint32_t key_size, void* value, uint32_t value_size);
   __attribute__((sysio_wasm_import))
   int64_t kv_erase(const void* key, uint32_t key_size);
   __attribute__((sysio_wasm_import))
   int32_t kv_contains(uint64_t code, const void* key, uint32_t key_size);
}

using namespace sysio;

// Raw balance: no serialization, fixed 16 bytes
struct raw_balance {
   int64_t  amount;
   uint64_t sym_code;
};
static_assert(sizeof(raw_balance) == 16, "raw_balance must be 16 bytes");

static void encode_be64(char* buf, uint64_t v) {
   for (int i = 7; i >= 0; --i) { buf[i] = static_cast<char>(v & 0xFF); v >>= 8; }
}

// SHiP-compatible key: [table("accounts"):8B][scope(owner):8B][pk(sym_code):8B]
static constexpr uint64_t ACCOUNTS_TABLE = "accounts"_n.value;

static void make_key(char* key, uint64_t scope, uint64_t pk) {
   encode_be64(key,      ACCOUNTS_TABLE);
   encode_be64(key + 8,  scope);
   encode_be64(key + 16, pk);
}

class [[sysio::contract("bench_kv_token")]] bench_kv_token : public contract {
public:
   using contract::contract;

   [[sysio::action]]
   void setup(uint32_t num_accounts) {
      raw_balance bal{1000000, 1}; // 1M tokens, symbol code 1
      char key[24];
      for (uint32_t i = 0; i < num_accounts; ++i) {
         make_key(key, i, 1);
         kv_set(0, get_self().value, key, 24, &bal, 16);
      }
   }

   [[sysio::action]]
   void dotransfers(uint32_t count) {
      uint64_t code = get_self().value;
      char from_key[24], to_key[24];
      raw_balance from_bal, to_bal;

      for (uint32_t i = 0; i < count; ++i) {
         uint64_t from_scope = i % 100;
         uint64_t to_scope   = (i + 1) % 100;

         // sub_balance: read, subtract, write
         make_key(from_key, from_scope, 1);
         int32_t sz = kv_get(code, from_key, 24, &from_bal, 16);
         check(sz == 16, "no balance");
         check(from_bal.amount >= 1, "overdrawn");
         from_bal.amount -= 1;
         kv_set(0, get_self().value, from_key, 24, &from_bal, 16);

         // add_balance: read, add, write
         make_key(to_key, to_scope, 1);
         sz = kv_get(code, to_key, 24, &to_bal, 16);
         check(sz == 16, "no balance");
         to_bal.amount += 1;
         kv_set(0, get_self().value, to_key, 24, &to_bal, 16);
      }
   }
};
