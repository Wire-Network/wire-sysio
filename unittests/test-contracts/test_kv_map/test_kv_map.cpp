#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>

using namespace sysio;

// Demonstrates kv::table with custom BE-encoded keys.

class [[sysio::contract("test_kv_map")]] test_kv_map : public contract {
public:
   using contract::contract;

   // Key struct — fields are BE-encoded for ordered storage
   struct my_key {
      std::string region;
      uint64_t    id;
      SYSLIB_SERIALIZE(my_key, (region)(id))
   };

   // Value struct
   struct [[sysio::table("geodata")]] my_value {
      std::string payload;
      uint64_t    amount;
      SYSLIB_SERIALIZE(my_value, (payload)(amount))
   };

   kv::table<"geodata"_n, my_key, my_value> geodata{get_self()};

   [[sysio::action]]
   void put(std::string region, uint64_t id, std::string payload, uint64_t amount) {
      geodata.set(get_self(), {region, id}, {payload, amount});
   }

   [[sysio::action]]
   void get(std::string region, uint64_t id) {
      auto val = geodata.try_get({region, id});
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
   struct signed_key {
      int64_t val;
      SYSLIB_SERIALIZE(signed_key, (val))
   };

   struct [[sysio::table("signeddata")]] signed_value {
      int64_t original_key;
      SYSLIB_SERIALIZE(signed_value, (original_key))
   };

   kv::table<"signeddata"_n, signed_key, signed_value> signed_store{get_self()};

   [[sysio::action]]
   void chkintorder() {
      // Store entries with negative, zero, and positive keys
      int64_t keys[] = {100, -50, 0, -100, 50};
      for (auto k : keys) {
         signed_store.set(get_self(), {k}, {k});
      }

      // Iteration must yield: -100, -50, 0, 50, 100
      int64_t expected[] = {-100, -50, 0, 50, 100};
      int idx = 0;
      for (auto it = signed_store.begin(); it != signed_store.end(); ++it) {
         check(idx < 5, "chkintorder: too many entries");
         check(it->original_key == expected[idx],
               "chkintorder: wrong order at position");
         ++idx;
      }
      check(idx == 5, "chkintorder: should have 5 entries");
   }
};
