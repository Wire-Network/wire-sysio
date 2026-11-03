#include <benchmark.hpp>

#include <chainbase/chainbase.hpp>
#include <fc/exception/exception.hpp>
#include <fc/filesystem.hpp>
#include <sysio/chain/kv_table_objects.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

// Times kv_object operations directly against chainbase, with no WASM or intrinsic layer.
//
//    benchmark/benchmark -f kv_chainbase

using namespace sysio::chain;

namespace sysio::benchmark {
namespace {

constexpr uint16_t bench_table_id = 1;
constexpr uint64_t bench_db_size  = 1024 * 1024 * 256; // 256 MB

// Helper: measure execution time
template<typename Func>
double measure_ns(Func&& f, int iterations) {
   auto start = std::chrono::high_resolution_clock::now();
   for (int i = 0; i < iterations; ++i) {
      f(i);
   }
   auto end = std::chrono::high_resolution_clock::now();
   return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / double(iterations);
}

// Helper: generate random bytes
static std::vector<char> random_bytes(std::mt19937& rng, size_t len) {
   std::vector<char> v(len);
   for (auto& c : v) c = static_cast<char>(rng() & 0xFF);
   return v;
}

// Helper: format ns/op
static std::string fmt_ns(double ns) {
   std::ostringstream ss;
   if (ns >= 1e6) ss << std::fixed << std::setprecision(2) << ns / 1e6 << " ms";
   else if (ns >= 1e3) ss << std::fixed << std::setprecision(1) << ns / 1e3 << " us";
   else ss << std::fixed << std::setprecision(0) << ns << " ns";
   return ss.str();
}

void chainbase_micro_benchmark() {
   fc::temp_directory dir;
   chainbase::database db(dir.path(), chainbase::database::read_write, bench_db_size);

   // Register KV indices
   db.add_index<kv_index>();
   db.add_index<kv_index_index>();

   std::mt19937 rng(42); // deterministic seed

   const std::vector<int> row_counts = {100, 1000, 10000};

   std::cout << "\n========== KV Database Micro-Benchmark ==========\n";
   std::cout << std::left << std::setw(25) << "Operation"
             << std::setw(10) << "Rows"
             << std::setw(15) << "ns/op" << "\n";
   std::cout << std::string(50, '-') << "\n";

   for (int N : row_counts) {
      auto session = db.start_undo_session(true);

      // --- INSERT benchmark ---
      std::vector<std::vector<char>> kv_keys(N);
      auto value = random_bytes(rng, 128);

      double kv_insert = measure_ns([&](int i) {
         // 8-byte big-endian key
         uint64_t k = static_cast<uint64_t>(i);
         char key_buf[8];
         for (int j = 7; j >= 0; --j) { key_buf[j] = static_cast<char>(k & 0xFF); k >>= 8; }
         kv_keys[i].assign(key_buf, key_buf + 8);

         db.create<kv_object>([&](auto& o) {
            o.code = "benchmark"_n;
            o.table_id = bench_table_id;
            o.set_key(kv_keys[i].data(), kv_keys[i].size());
            o.set_value({value.data(), value.size()});
         });
      }, N);

      std::cout << std::setw(25) << "Insert"
                << std::setw(10) << N
                << std::setw(15) << fmt_ns(kv_insert) << "\n";

      // --- POINT LOOKUP benchmark ---
      auto& kv_idx = db.get_index<kv_index, by_code_key>();

      double kv_find = measure_ns([&](int i) {
         int idx = i % N;
         auto sv = std::string_view(kv_keys[idx].data(), kv_keys[idx].size());
         auto itr = kv_idx.find(boost::make_tuple(name("benchmark"), bench_table_id, sv));
         FC_ASSERT(itr != kv_idx.end());
      }, N);

      std::cout << std::setw(25) << "Point Lookup"
                << std::setw(10) << N
                << std::setw(15) << fmt_ns(kv_find) << "\n";

      // --- SEQUENTIAL ITERATION benchmark ---
      double kv_iter = measure_ns([&](int) {
         auto itr = kv_idx.lower_bound(boost::make_tuple(name("benchmark"), bench_table_id));
         int count = 0;
         while (itr != kv_idx.end() && itr->code == name("benchmark")) {
            ++count;
            ++itr;
         }
         FC_ASSERT(count == N);
      }, 1);

      std::cout << std::setw(25) << "Full Iteration"
                << std::setw(10) << N
                << std::setw(15) << fmt_ns(kv_iter / N) << "\n";

      // --- UPDATE benchmark ---
      auto new_value = random_bytes(rng, 128);

      double kv_update = measure_ns([&](int i) {
         int idx = i % N;
         auto sv = std::string_view(kv_keys[idx].data(), kv_keys[idx].size());
         auto itr = kv_idx.find(boost::make_tuple(name("benchmark"), bench_table_id, sv));
         db.modify(*itr, [&](auto& o) {
            o.set_value({new_value.data(), new_value.size()});
         });
      }, N);

      std::cout << std::setw(25) << "Update"
                << std::setw(10) << N
                << std::setw(15) << fmt_ns(kv_update) << "\n";

      // --- ERASE benchmark ---
      double kv_erase = measure_ns([&](int i) {
         auto sv = std::string_view(kv_keys[i].data(), kv_keys[i].size());
         auto itr = kv_idx.find(boost::make_tuple(name("benchmark"), bench_table_id, sv));
         FC_ASSERT(itr != kv_idx.end());
         db.remove(*itr);
      }, N);

      std::cout << std::setw(25) << "Erase"
                << std::setw(10) << N
                << std::setw(15) << fmt_ns(kv_erase) << "\n";

      std::cout << std::string(50, '-') << "\n";

      session.undo();
   }

   std::cout << "=================================================\n\n";
}

void full_intrinsic_path_benchmark() {
   fc::temp_directory dir;
   chainbase::database db(dir.path(), chainbase::database::read_write, bench_db_size);

   db.add_index<kv_index>();

   std::mt19937 rng(42);

   const std::vector<int> row_counts = {100, 1000, 10000};

   std::cout << "\n===== KV Full Intrinsic Path Benchmark =====\n";
   std::cout << std::left << std::setw(25) << "Operation"
             << std::setw(10) << "Rows"
             << std::setw(15) << "ns/op" << "\n";
   std::cout << std::string(50, '-') << "\n";

   for (int N : row_counts) {
      auto session = db.start_undo_session(true);

      auto value = random_bytes(rng, 128);
      auto new_value = random_bytes(rng, 128);

      // --- KV STORE: direct composite key lookup + create ---
      auto& kv_idx = db.get_index<kv_index, by_code_key>();
      std::vector<std::vector<char>> kv_keys(N);

      double kv_store = measure_ns([&](int i) {
         // KV: single composite key check + create
         char key_buf[8];
         uint64_t k = static_cast<uint64_t>(i);
         for (int j = 7; j >= 0; --j) { key_buf[j] = static_cast<char>(k & 0xFF); k >>= 8; }
         kv_keys[i].assign(key_buf, key_buf + 8);
         auto sv = std::string_view(key_buf, 8);

         // Check if exists (what kv_set does)
         (void)kv_idx.find(boost::make_tuple(name("kvbench"), bench_table_id, sv));
         // Create new
         db.create<kv_object>([&](auto& o) {
            o.code = "kvbench"_n;
            o.table_id = bench_table_id;
            o.set_key(key_buf, 8);
            o.set_value({value.data(), value.size()});
         });
      }, N);

      std::cout << std::setw(25) << "Store (full path)"
                << std::setw(10) << N
                << std::setw(15) << fmt_ns(kv_store) << "\n";

      // --- FIND benchmark ---
      double kv_find = measure_ns([&](int i) {
         int idx = i % N;
         auto sv = std::string_view(kv_keys[idx].data(), kv_keys[idx].size());
         auto itr = kv_idx.find(boost::make_tuple(name("kvbench"), bench_table_id, sv));
         FC_ASSERT(itr != kv_idx.end());
      }, N);

      std::cout << std::setw(25) << "Find (full path)"
                << std::setw(10) << N
                << std::setw(15) << fmt_ns(kv_find) << "\n";

      // --- UPDATE benchmark ---
      double kv_update = measure_ns([&](int i) {
         int idx = i % N;
         auto sv = std::string_view(kv_keys[idx].data(), kv_keys[idx].size());
         auto itr = kv_idx.find(boost::make_tuple(name("kvbench"), bench_table_id, sv));
         // Compute delta on value size (simulating intrinsic path)
         int64_t old_size = static_cast<int64_t>(itr->value_view().size());
         int64_t new_size = static_cast<int64_t>(new_value.size());
         volatile int64_t delta = new_size - old_size;
         (void)delta;
         db.modify(*itr, [&](auto& o) {
            o.set_value({new_value.data(), new_value.size()});
         });
      }, N);

      std::cout << std::setw(25) << "Update (full path)"
                << std::setw(10) << N
                << std::setw(15) << fmt_ns(kv_update) << "\n";

      // --- ERASE benchmark ---
      double kv_erase = measure_ns([&](int i) {
         auto sv = std::string_view(kv_keys[i].data(), kv_keys[i].size());
         auto itr = kv_idx.find(boost::make_tuple(name("kvbench"), bench_table_id, sv));
         FC_ASSERT(itr != kv_idx.end());
         db.remove(*itr);
      }, N);

      std::cout << std::setw(25) << "Erase (full path)"
                << std::setw(10) << N
                << std::setw(15) << fmt_ns(kv_erase) << "\n";

      std::cout << std::string(50, '-') << "\n";

      session.undo();
   }

   std::cout << "=================================================\n\n";
}

} // namespace

void kv_chainbase_benchmarking() {
   chainbase_micro_benchmark();
   full_intrinsic_path_benchmark();
}

} // namespace sysio::benchmark
