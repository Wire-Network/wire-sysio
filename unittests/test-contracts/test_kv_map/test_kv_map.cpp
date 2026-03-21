#include <sysio/sysio.hpp>
#include <sysio/kv_raw_table.hpp>

using namespace sysio;

// Demonstrates kv::raw_table with ordered BE keys and ABI key metadata.
// SHiP emits these as contract_row_kv deltas; clients decode key (BE)
// and value (LE/ABI) using key_names/key_types from the contract ABI.

class [[sysio::contract("test_kv_map")]] test_kv_map : public contract {
public:
   using contract::contract;

   // Key struct — fields are BE-encoded for ordered storage
   struct my_key {
      std::string region;
      uint64_t    id;
      SYSLIB_SERIALIZE(my_key, (region)(id))
   };

   // Value struct with [[sysio::kv_key]] for ABI key metadata
   struct [[sysio::table("geodata"), sysio::kv_key("my_key")]] my_value {
      std::string payload;
      uint64_t    amount;
      SYSLIB_SERIALIZE(my_value, (payload)(amount))
   };

   kv::raw_table<my_key, my_value> geodata;

   [[sysio::action]]
   void put(std::string region, uint64_t id, std::string payload, uint64_t amount) {
      geodata.set({region, id}, {payload, amount});
   }

   [[sysio::action]]
   void get(std::string region, uint64_t id) {
      auto val = geodata.get({region, id});
      check(val.has_value(), "key not found");
      check(val->payload.size() > 0, "empty payload");
   }

   [[sysio::action]]
   void erase(std::string region, uint64_t id) {
      geodata.erase({region, id});
   }

   [[sysio::action]]
   void count() {
      uint32_t n = 0;
      for (auto it = geodata.begin(); it != geodata.end(); ++it) {
         ++n;
      }
      check(n > 0, "no entries");
   }

   // ─── chkintorder: verify signed int64_t keys sort correctly ──────────────
   // be_key_stream must apply sign-bit flip so negative values sort before
   // positives in lexicographic (memcmp) comparison.
   struct signed_key {
      int64_t val;
      SYSLIB_SERIALIZE(signed_key, (val))
   };

   struct [[sysio::table("signeddata"), sysio::kv_key("signed_key")]] signed_value {
      int64_t original_key;
      SYSLIB_SERIALIZE(signed_value, (original_key))
   };

   kv::raw_table<signed_key, signed_value> signed_store;

   [[sysio::action]]
   void chkintorder() {
      // Store entries with negative, zero, and positive keys
      int64_t keys[] = {100, -50, 0, -100, 50};
      for (auto k : keys) {
         signed_store.set({k}, {k});
      }

      // Iteration must yield: -100, -50, 0, 50, 100
      int64_t expected[] = {-100, -50, 0, 50, 100};
      int idx = 0;
      for (auto it = signed_store.begin(); it != signed_store.end(); ++it) {
         check(idx < 5, "chkintorder: too many entries");
         check((*it).original_key == expected[idx],
               "chkintorder: wrong order at position");
         ++idx;
      }
      check(idx == 5, "chkintorder: should have 5 entries");
   }
};
