#include <kv_bench_chain.hpp>
#include <test_contracts.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <iostream>
#include <limits>
#include <span>

// Times each kv::table or multi_index operation at large row counts, so the cost of cache misses shows. Use a Release
// build.
//
//    benchmark/benchmark -f kv_table -r 20 --kv-rows 1000000 10000000
//    benchmark/benchmark -f kv_ops -r 20 --kv-rows 1000000 10000000

using namespace sysio::chain::literals;
using sysio::testing::test_contracts;

namespace sysio::benchmark {
namespace {

/// bench_kv_ops. bench_kv_table has the same actions with the same arguments, except `action_host_call`.
namespace ops_contract {
   constexpr auto account          = "benchkvops"_n;
   constexpr auto action_populate  = "populate"_n;
   constexpr auto action_get       = "opget"_n;
   constexpr auto action_find      = "opfind"_n;
   constexpr auto action_findmod   = "opfindmod"_n;
   constexpr auto action_emplace   = "opemplace"_n;
   constexpr auto action_erase     = "operase"_n;
   constexpr auto action_scan      = "opscan"_n;
   constexpr auto action_sec_scan  = "opsecscan"_n;
   constexpr auto action_host_call = "ophostcall"_n;
   namespace field {
      constexpr auto begin = "begin";
      constexpr auto count = "count";
      constexpr auto total = "total";
   }
}

/// Actions only bench_kv_table has.
namespace table_contract {
   constexpr auto account                = "benchkvtable"_n;
   constexpr auto action_populate_scoped = "popscoped"_n;
   constexpr auto action_modify_key      = "opmodkey"_n;
   constexpr auto action_erase_key       = "operasekey"_n;
   constexpr auto action_scoped_scan     = "opscopescan"_n;
   constexpr auto action_global_get      = "opglobalget"_n;
}

constexpr uint64_t ns_per_us           = 1'000;
constexpr uint32_t populate_batch_rows = 5'000;     ///< rows stored by one populate action
constexpr uint32_t batches_per_block   = 10;
constexpr uint32_t ops_per_run         = 10'000;    ///< operations in one measured action
constexpr uint32_t host_calls_per_run  = 1'000'000; ///< high enough that action setup does not show in the result
constexpr uint32_t warmup_ops          = 100;       ///< enough to get every action compiled before it is measured
constexpr uint64_t state_bytes_per_row = 1024;      ///< covers one row in each table plus its index entry

/// Actions of `measure_shared`. A row a warm-up emplace stores is free again before the measured runs begin.
constexpr std::array shared_actions{ops_contract::action_get,     ops_contract::action_find,
                                    ops_contract::action_findmod, ops_contract::action_emplace,
                                    ops_contract::action_erase,   ops_contract::action_scan,
                                    ops_contract::action_sec_scan};

/** Per-operation time over the runs of one measured action, in nanoseconds. */
struct op_stats {
   uint64_t total = 0;
   uint64_t min   = std::numeric_limits<uint64_t>::max();
   uint64_t max   = 0;

   /// Records one run. The result truncates to a whole nanosecond per operation.
   void add(int64_t elapsed_us, uint32_t ops = ops_per_run) {
      const uint64_t ns_per_op = elapsed_us * ns_per_us / ops;
      total += ns_per_op;
      min = std::min(min, ns_per_op);
      max = std::max(max, ns_per_op);
   }
};

/// Arguments of one bench_kv_ops or bench_kv_table action: `count` operations from `begin`, on a table of `rows` rows.
fc::mutable_variant_object op_args(uint32_t begin, uint32_t count, uint32_t rows) {
   namespace field = ops_contract::field;
   return fc::mutable_variant_object()(field::begin, begin)(field::count, count)(field::total, rows);
}

/**
 * A fresh chain holding `rows` rows of bench_kv_ops or bench_kv_table, and the ways of timing actions on it. Each run
 * continues from where the last run of any action stopped.
 */
class ops_chain {
public:
   ops_chain(chain::account_name account, const std::vector<uint8_t>& wasm, const std::string& abi, uint32_t rows,
             chain::action_name populate_action = ops_contract::action_populate)
      : _bench(account, wasm, abi, kv_bench_chain::default_state_size + rows * state_bytes_per_row)
      , _rows(rows) {
      // Fewer rows would wrap inside one run, and emplace would hit a key it stored earlier in that run.
      FC_ASSERT(rows >= ops_per_run, "needs at least {} rows, got {}", ops_per_run, rows);
      populate(populate_action);
   }

   /// Runs each action once so it is compiled before it is measured.
   void warm_up(std::span<const chain::action_name> actions) {
      for (auto action : actions)
         _bench.run(action, op_args(0, warmup_ops, _rows));
      _bench.produce_block();
   }

   /// Times an action that changes no row, with `ops` operations per run.
   void read(std::string_view name, chain::action_name action, uint32_t ops = ops_per_run) {
      op_stats stats;
      for (uint32_t run = 0; run < get_num_runs(); ++run, _begin += ops)
         stats.add(_bench.run(action, op_args(_begin, ops, _rows)), ops);
      _bench.produce_block();
      report(name, stats);
   }

   /// Times an action that changes rows in place.
   void write(std::string_view name, chain::action_name action) {
      op_stats stats;
      for (uint32_t run = 0; run < get_num_runs(); ++run, _begin += ops_per_run) {
         stats.add(_bench.run(action, op_args(_begin, ops_per_run, _rows)));
         _bench.produce_block();
      }
      report(name, stats);
   }

   /// Times an action that stores rows and one that removes them. Each remove run undoes the store run before it, so
   /// the row count stays constant.
   void store_remove(std::string_view store_name, chain::action_name store, std::string_view remove_name,
                     chain::action_name remove) {
      op_stats store_stats, remove_stats;
      for (uint32_t run = 0; run < get_num_runs(); ++run, _begin += ops_per_run) {
         store_stats.add(_bench.run(store, op_args(_begin, ops_per_run, _rows)));
         remove_stats.add(_bench.run(remove, op_args(_begin, ops_per_run, _rows)));
         _bench.produce_block();
      }
      report(store_name, store_stats);
      report(remove_name, remove_stats);
   }

private:
   /// Stores the rows in batches and prints how long that took and how much state memory they use. Each id stores two
   /// rows and one index entry, so the per-row figure covers all three.
   void populate(chain::action_name action) {
      const uint64_t used_before = _bench.state_bytes_used();
      const auto start = std::chrono::steady_clock::now();
      uint32_t batch = 0;
      for (uint32_t begin = 0; begin < _rows; begin += populate_batch_rows) {
         _bench.run(action, op_args(begin, std::min(populate_batch_rows, _rows - begin), _rows));
         if (++batch % batches_per_block == 0)
            _bench.produce_block();
      }
      _bench.produce_block();
      const std::chrono::duration<double> seconds = std::chrono::steady_clock::now() - start;
      std::cout << std::format("populated {} rows in {:.1f} s", _rows, seconds.count()) << std::endl;
      const uint64_t used = _bench.state_bytes_used() - used_before;
      std::cout << std::format("state memory: {} bytes, {:.1f} per id", used, static_cast<double>(used) / _rows)
                << std::endl;
   }

   void report(std::string_view name, const op_stats& stats) {
      print_results(std::format("{} @ {} rows", name, _rows), get_num_runs(), stats.total, stats.min, stats.max);
   }

   kv_bench_chain _bench;
   uint32_t       _rows;
   uint32_t       _begin = 0;
};

/// Times the operations both contracts have, in one order so each sees the same history on both.
void measure_shared(ops_chain& chain) {
   chain.read("get", ops_contract::action_get);
   chain.read("find", ops_contract::action_find);
   chain.write("find + modify", ops_contract::action_findmod);
   chain.store_remove("emplace", ops_contract::action_emplace, "find + erase", ops_contract::action_erase);
   chain.read("primary scan, per row", ops_contract::action_scan);
   chain.read("secondary scan, per row", ops_contract::action_sec_scan);
}

/// Prints the settings every result of the kv_ops and kv_table features was measured under.
void print_settings() {
   std::cout << "wasm runtime: " << magic_enum::enum_name(get_wasm_runtime()) << ", " << ops_per_run
             << " operations per run, nanoseconds per operation" << std::endl;
}

} // namespace

void kv_ops_benchmarking() {
   print_settings();
   for (uint32_t rows : get_kv_rows()) {
      ops_chain chain(ops_contract::account, test_contracts::bench_kv_ops_wasm(), test_contracts::bench_kv_ops_abi(),
                      rows);
      chain.warm_up(shared_actions);
      chain.warm_up(std::array{ops_contract::action_host_call});

      measure_shared(chain);
      chain.read("host call (kv_it_read)", ops_contract::action_host_call, host_calls_per_run);
   }
}

void kv_table_benchmarking() {
   print_settings();
   const auto wasm = test_contracts::bench_kv_table_wasm();
   const auto abi  = test_contracts::bench_kv_table_abi();
   for (uint32_t rows : get_kv_rows()) {
      {
         ops_chain chain(table_contract::account, wasm, abi, rows);
         chain.warm_up(shared_actions);
         chain.warm_up(std::array{table_contract::action_modify_key, ops_contract::action_emplace,
                                  table_contract::action_erase_key, table_contract::action_global_get});

         measure_shared(chain);
         chain.write("modify by key", table_contract::action_modify_key);
         chain.store_remove("emplace, second cycle", ops_contract::action_emplace, "erase by key",
                            table_contract::action_erase_key);
         chain.read("global get", table_contract::action_global_get);
      }
      // All primary rows of all tables share one tree, so a third table on the chain above would deepen the tree under
      // every operation and spoil the comparison with kv_ops. This chain holds the same number of rows instead.
      ops_chain scoped(table_contract::account, wasm, abi, rows, table_contract::action_populate_scoped);
      scoped.warm_up(std::array{table_contract::action_scoped_scan});
      scoped.read("scoped primary scan, per row", table_contract::action_scoped_scan);
   }
}

} // namespace sysio::benchmark
