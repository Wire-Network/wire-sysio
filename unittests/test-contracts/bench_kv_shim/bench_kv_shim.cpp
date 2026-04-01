/**
 * Benchmark contract using KV-backed multi_index emulation layer.
 * Same API as bench_legacy_db, demonstrating zero-code-change migration.
 */
#include <sysio/sysio.hpp>
// kv_multi_index provided by CDT via sysio.hpp -> multi_index.hpp

using namespace sysio;

// Use KV-backed multi_index
using sysio::kv_multi_index;

class [[sysio::contract("bench_kv_shim")]] bench_kv_shim : public contract {
public:
   using contract::contract;

   struct [[sysio::table]] row {
      uint64_t    id;
      uint64_t    value1;
      uint64_t    value2;
      std::string data;

      uint64_t primary_key() const { return id; }
   };

   // Use KV-backed multi_index (no secondary indices for this benchmark)
   typedef kv_multi_index<"rows"_n, row> rows_table;

   [[sysio::action]]
   void populate(uint32_t count) {
      rows_table tbl(get_self(), get_self().value);
      for (uint32_t i = 0; i < count; ++i) {
         tbl.emplace(get_self(), [&](auto& r) {
            r.id     = i;
            r.value1 = count - i;
            r.value2 = i * 7;
            r.data   = "benchmark_payload_data_here";
         });
      }
   }

   [[sysio::action]]
   void findall(uint32_t count) {
      rows_table tbl(get_self(), get_self().value);
      for (uint32_t i = 0; i < count; ++i) {
         auto itr = tbl.find(i);
         check(itr != tbl.end(), "row not found");
         check(itr->value2 == i * 7, "bad value");
      }
   }

   [[sysio::action]]
   void iterall() {
      rows_table tbl(get_self(), get_self().value);
      uint32_t count = 0;
      for (auto itr = tbl.begin(); itr != tbl.end(); ++itr) {
         ++count;
      }
      check(count > 0, "no rows");
   }

   [[sysio::action]]
   void updateall(uint32_t count) {
      rows_table tbl(get_self(), get_self().value);
      for (uint32_t i = 0; i < count; ++i) {
         auto itr = tbl.find(i);
         check(itr != tbl.end(), "row not found");
         tbl.modify(itr, get_self(), [&](auto& r) {
            r.value2 = i * 13;
         });
      }
   }

   [[sysio::action]]
   void eraseall(uint32_t count) {
      rows_table tbl(get_self(), get_self().value);
      for (uint32_t i = 0; i < count; ++i) {
         auto itr = tbl.find(i);
         check(itr != tbl.end(), "row not found");
         tbl.erase(itr);
      }
   }
};
