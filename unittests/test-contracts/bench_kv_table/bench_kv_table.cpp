/**
 * Runs one kv::table operation per action, for the `kv_table` benchmark.
 *
 * Stores and visits the same rows in the same orders as bench_kv_ops, so the two compare operation by operation. The
 * tables are declared and called the way the system contracts do it: a key struct, a row that keeps its id, and
 * modify and erase by key as well as through an iterator. kv::table has no row cache. Each point operation still
 * builds its own table object, to keep the shape of bench_kv_ops.
 */
#include "../bench_kv_ops/bench_row_order.hpp"

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/multi_index.hpp> // sysio::const_mem_fun (secondary-index key extractor)

using namespace sysio;
using namespace bench_row_order;

class [[sysio::contract("bench_kv_table")]] bench_kv_table : public contract {
public:
   using contract::contract;

   struct row_key {
      uint64_t id;

      SYSLIB_SERIALIZE(row_key, (id))
   };

   struct [[sysio::table("rows")]] row {
      uint64_t    id;
      uint64_t    sec;
      uint64_t    counter;
      std::string data;

      uint64_t by_sec() const { return sec; }

      SYSLIB_SERIALIZE(row, (id)(sec)(counter)(data))
   };

   /// Trivially copyable, so kv::global reads it with a plain copy and no decoding.
   struct [[sysio::table("settings")]] settings {
      uint64_t total;

      SYSLIB_SERIALIZE(settings, (total))
   };

   using plain_table     = kv::table<"rows"_n, row_key, row>;
   using indexed_table   = kv::table<"irows"_n, row_key, row,
      kv::index<"bysec"_n, const_mem_fun<row, uint64_t, &row::by_sec>>>;
   using scoped_table    = kv::scoped_table<"srows"_n, row_key, row>;
   using settings_global = kv::global<"settings"_n, settings>;

   /// Stores rows [begin, begin + count) of `total` in the plain and indexed tables, and sets the global.
   [[sysio::action]]
   void populate(uint32_t begin, uint32_t count, uint32_t total) {
      settings_global(get_self()).set(settings{total}, get_self());
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         const uint64_t id = stored_id(index, total);
         plain_table plain(get_self());
         plain.emplace(get_self(), row_key{id}, make_row(id));
         indexed_table indexed(get_self());
         indexed.emplace(get_self(), row_key{id}, make_row(id));
      }
   }

   /// Same as `populate` with the scoped table in place of the plain one, for a chain that only measures opscopescan.
   [[sysio::action]]
   void popscoped(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         const uint64_t id = stored_id(index, total);
         scoped_table scoped(get_self(), get_self().value);
         scoped.emplace(get_self(), row_key{id}, make_row(id));
         indexed_table indexed(get_self());
         indexed.emplace(get_self(), row_key{id}, make_row(id));
      }
   }

   /// Reads `count` rows with `get`.
   [[sysio::action]]
   void opget(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         const uint64_t id = visited_id(index, total);
         plain_table plain(get_self());
         check(plain.get(row_key{id}).id == id, "wrong row");
      }
   }

   /// Reads `count` rows with `find`.
   [[sysio::action]]
   void opfind(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         const uint64_t id = visited_id(index, total);
         plain_table plain(get_self());
         auto itr = plain.find(row_key{id});
         check(itr != plain.end() && itr->id == id, "wrong row");
      }
   }

   /// Finds `count` rows and modifies each one through its iterator.
   [[sysio::action]]
   void opfindmod(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         plain_table plain(get_self());
         auto itr = plain.find(row_key{visited_id(index, total)});
         check(itr != plain.end(), "row not found");
         row updated = *itr;
         ++updated.counter;
         plain.modify(same_payer, itr, updated);
      }
   }

   /// Modifies `count` rows by key, which is how the system contracts modify rows.
   [[sysio::action]]
   void opmodkey(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         plain_table plain(get_self());
         plain.modify(same_payer, row_key{visited_id(index, total)}, [](auto& r) { ++r.counter; });
      }
   }

   /// Stores `count` rows at unused ids.
   [[sysio::action]]
   void opemplace(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         const uint64_t id = free_id(index, total);
         plain_table plain(get_self());
         plain.emplace(get_self(), row_key{id}, make_row(id));
      }
   }

   /// Erases through an iterator the rows opemplace stored for the same arguments.
   [[sysio::action]]
   void operase(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         plain_table plain(get_self());
         auto itr = plain.find(row_key{free_id(index, total)});
         check(itr != plain.end(), "row not found");
         plain.erase(std::move(itr));
      }
   }

   /// Erases by key the rows opemplace stored for the same arguments.
   [[sysio::action]]
   void operasekey(uint32_t begin, uint32_t count, uint32_t total) {
      for (uint64_t index = begin; index < uint64_t(begin) + count; ++index) {
         plain_table plain(get_self());
         plain.erase(row_key{free_id(index, total)});
      }
   }

   /// Walks `count` rows in key order from the row for `begin`, wrapping at the end of the table.
   [[sysio::action]]
   void opscan(uint32_t begin, uint32_t count, uint32_t total) {
      plain_table plain(get_self());
      auto itr = plain.lower_bound(row_key{visited_id(begin, total)});
      for (uint32_t seen = 0; seen < count; ++seen, ++itr) {
         if (itr == plain.end()) itr = plain.begin();
         check(itr->id % 2 == 0, "wrong row");
      }
   }

   /// Walks `count` rows in secondary key order from the row for `begin`, wrapping at the end of the index.
   [[sysio::action]]
   void opsecscan(uint32_t begin, uint32_t count, uint32_t total) {
      indexed_table indexed(get_self());
      auto index = indexed.get_index<"bysec"_n>();
      auto itr = index.lower_bound(secondary_key(visited_id(begin, total)));
      for (uint32_t seen = 0; seen < count; ++seen, ++itr) {
         if (itr == index.end()) itr = index.begin();
         check(itr->id % 2 == 0, "wrong row");
      }
   }

   /// Walks `count` rows of the one scope in key order from the row for `begin`, wrapping at the end of the scope.
   [[sysio::action]]
   void opscopescan(uint32_t begin, uint32_t count, uint32_t total) {
      scoped_table scoped(get_self(), get_self().value);
      auto itr = scoped.lower_bound(row_key{visited_id(begin, total)});
      for (uint32_t seen = 0; seen < count; ++seen, ++itr) {
         if (itr == scoped.end()) itr = scoped.begin();
         check(itr->id % 2 == 0, "wrong row");
      }
   }

   /// Reads the global `count` times. `begin` only makes each run a distinct transaction.
   [[sysio::action]]
   void opglobalget(uint32_t begin, uint32_t count, uint32_t total) {
      (void)begin;
      for (uint32_t read = 0; read < count; ++read) {
         settings_global global(get_self());
         check(global.get().total == total, "wrong global");
      }
   }

private:
   static row make_row(uint64_t id) { return row{id, secondary_key(id), 0, row_payload}; }
};
