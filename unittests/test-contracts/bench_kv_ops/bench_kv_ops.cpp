/**
 * Runs one multi_index operation per action, for the `kv_ops` benchmark.
 *
 * Rows go in by permuted id so key order differs from allocation order, as it does in live state. Operations visit
 * rows in a second order unrelated to both, because walking rows in the order they were stored would touch adjacent
 * memory on every step and hide the cache misses this benchmark exists to measure. Even ids hold rows and odd ids
 * stay free, so emplace lands at random tree positions instead of appending at the right edge. Each point operation
 * builds its own table object, so the row is never in the CDT cache.
 */
#include "bench_row_order.hpp"

#include <sysio/sysio.hpp>
#include <sysio/multi_index.hpp>

using namespace sysio;
using namespace bench_row_order;

class [[sysio::contract("bench_kv_ops")]] bench_kv_ops : public contract {
public:
   using contract::contract;

   struct [[sysio::table]] row {
      uint64_t    id;
      uint64_t    sec;
      uint64_t    counter;
      std::string data;

      uint64_t primary_key() const { return id; }
      uint64_t by_sec() const { return sec; }
   };

   static constexpr uint16_t rows_table_id = sysio::kv::compute_table_id("rows"_n.value);

   typedef sysio::multi_index<"rows"_n, row> plain_table;
   typedef sysio::multi_index<"irows"_n, row, indexed_by<"bysec"_n, const_mem_fun<row, uint64_t, &row::by_sec>>>
      indexed_table;

   /// Stores rows [begin, begin + count) of `total` in both tables.
   [[sysio::action]]
   void populate(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         const uint64_t id = stored_id(index, total);
         plain_table plain(get_self(), get_self().value);
         plain.emplace(get_self(), [&](auto& r) { fill(r, id); });
         indexed_table indexed(get_self(), get_self().value);
         indexed.emplace(get_self(), [&](auto& r) { fill(r, id); });
      }
   }

   /// Reads `count` rows with `get`.
   [[sysio::action]]
   void opget(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         const uint64_t id = visited_id(index, total);
         plain_table plain(get_self(), get_self().value);
         check(plain.get(id).id == id, "wrong row");
      }
   }

   /// Reads `count` rows with `find`.
   [[sysio::action]]
   void opfind(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         const uint64_t id = visited_id(index, total);
         plain_table plain(get_self(), get_self().value);
         auto itr = plain.find(id);
         check(itr != plain.end() && itr->id == id, "wrong row");
      }
   }

   /// Finds `count` rows and modifies each one.
   [[sysio::action]]
   void opfindmod(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         plain_table plain(get_self(), get_self().value);
         auto itr = plain.find(visited_id(index, total));
         check(itr != plain.end(), "row not found");
         plain.modify(itr, same_payer, [](auto& r) { ++r.counter; });
      }
   }

   /// Stores `count` rows at unused ids.
   [[sysio::action]]
   void opemplace(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         const uint64_t id = free_id(index, total);
         plain_table plain(get_self(), get_self().value);
         plain.emplace(get_self(), [&](auto& r) { fill(r, id); });
      }
   }

   /// Erases the rows opemplace stored for the same arguments.
   [[sysio::action]]
   void operase(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         plain_table plain(get_self(), get_self().value);
         auto itr = plain.find(free_id(index, total));
         check(itr != plain.end(), "row not found");
         plain.erase(itr);
      }
   }

   /// Walks `count` rows in key order from the row for `begin`, wrapping at the end of the table.
   [[sysio::action]]
   void opscan(uint32_t begin, uint32_t count, uint32_t total) {
      plain_table plain(get_self(), get_self().value);
      auto itr = plain.lower_bound(visited_id(begin, total));
      for (uint32_t seen = 0; seen < count; ++seen, ++itr) {
         if (itr == plain.end()) itr = plain.begin();
         check(itr->id % 2 == 0, "wrong row");
      }
   }

   /// Walks `count` rows in secondary key order from the row for `begin`, wrapping at the end of the index.
   [[sysio::action]]
   void opsecscan(uint32_t begin, uint32_t count, uint32_t total) {
      indexed_table indexed(get_self(), get_self().value);
      auto index = indexed.get_index<"bysec"_n>();
      auto itr = index.lower_bound(secondary_key(visited_id(begin, total)));
      for (uint32_t seen = 0; seen < count; ++seen, ++itr) {
         if (itr == index.end()) itr = index.begin();
         check(itr->id % 2 == 0, "wrong row");
      }
   }

   /// Calls `kv_it_read` `count` times on one open iterator with empty buffers. That host call reads the slot and the
   /// row's sizes and does no tree search or id lookup, so this is close to the cost of crossing into the host alone.
   /// Uses the raw intrinsics because a table iterator keeps its handle private. `begin` and `total` only make each
   /// run a distinct transaction.
   [[sysio::action]]
   void ophostcall(uint32_t begin, uint32_t count, uint32_t total) {
      (void)begin;
      (void)total;
      const int32_t handle = ::kv_it_create(rows_table_id, get_self().value, nullptr, 0, nullptr, 0);
      int32_t  statuses   = 0;
      uint32_t key_size   = 0;
      uint32_t value_size = 0;
      for (uint32_t call = 0; call < count; ++call)
         statuses |= ::kv_it_read(handle, 0, nullptr, 0, nullptr, 0, &key_size, &value_size);
      ::kv_it_destroy(handle);
      check(statuses == 0, "iterator not on a row");
   }

private:
   static void fill(row& r, uint64_t id) {
      r.id      = id;
      r.sec     = secondary_key(id);
      r.counter = 0;
      r.data    = row_payload;
   }
};
