// Microbenchmark for Keccak-256 / SHA3-256 implementations on the consensus
// path.  Measures fc::sha3 (hand-rolled, used by the WASM sha3() host
// intrinsic) and ethash::keccak256 (used by EM signature recovery), at sizes
// spanning the realistic input range -- from 32 B (typical contract digest)
// to 33 MiB (default max wasm linear memory = 528 * 64 KiB pages).
//
// Build (RelWithDebInfo / Release; Debug numbers are not comparable):
//   ninja -C cmake-build-relwithdebinfo -j8 keccak_bench
//   ./cmake-build-relwithdebinfo/libraries/libfc/benchmark/keccak_bench

#include <fc/crypto/sha3.hpp>
#include <ethash/keccak.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::steady_clock;

namespace {

// Volatile sink prevents the optimizer from eliding the measured call.
// Reading two bytes of the digest is sufficient because Keccak's output
// depends on every input byte (the sponge absorbs the full message into
// the state before squeezing any output), so the compiler cannot reduce
// the hash computation while preserving the observable XOR into `bench_sink`.
volatile uint8_t bench_sink = 0;
inline void sink(const void* p, size_t n) {
   const uint8_t* b = static_cast<const uint8_t*>(p);
   bench_sink ^= b[0] ^ b[n - 1];
}

// Re-seeds with the same constant on every call, so buffers of different
// sizes share a common prefix. Irrelevant for throughput measurement but
// worth noting if anyone expects independent random samples across sizes.
std::vector<uint8_t> make_buffer(size_t n) {
   std::mt19937_64 rng{0xC0FFEEULL};
   std::vector<uint8_t> v(n);
   for (auto& byte : v) byte = static_cast<uint8_t>(rng());
   return v;
}

// Format a byte count in the largest power-of-1024 unit that gives a whole
// number, for readable scenario output (e.g. 34603008 -> "33 MiB").
std::string fmt_size(size_t bytes) {
   if (bytes >= 1024 * 1024 && bytes % (1024 * 1024) == 0)
      return std::to_string(bytes / (1024 * 1024)) + " MiB";
   if (bytes >= 1024 && bytes % 1024 == 0)
      return std::to_string(bytes / 1024) + " KiB";
   return std::to_string(bytes) + " B";
}

struct result {
   std::string name;
   size_t size = 0;
   double median_ms = 0;
   double min_ms = 0;
   double max_ms = 0;
};

template <typename F>
result time_it(const std::string& name, size_t size, int runs, F&& f) {
   // `runs` must be odd so `times[runs / 2]` returns the exact median rather
   // than the upper of the two middle values.
   assert(runs > 0 && runs % 2 == 1);

   std::vector<double> times;
   times.reserve(runs);
   for (int i = 0; i < 2; ++i) f();  // warmup
   for (int i = 0; i < runs; ++i) {
      auto t0 = steady_clock::now();
      f();
      auto t1 = steady_clock::now();
      times.push_back(duration_cast<microseconds>(t1 - t0).count() / 1000.0);
   }
   std::sort(times.begin(), times.end());
   return {name, size, times[runs / 2], times.front(), times.back()};
}

void print_header() {
   std::cout << std::left
             << std::setw(38) << "scenario"
             << std::right
             << std::setw(10) << "size"
             << std::setw(13) << "median(ms)"
             << std::setw(12) << "min(ms)"
             << std::setw(12) << "max(ms)"
             << std::setw(14) << "MB/s(median)"
             << "\n";
   std::cout << std::string(99, '-') << "\n";
}

void print(const result& r) {
   double mb_per_s = r.median_ms > 0
                        ? (static_cast<double>(r.size) / (1024.0 * 1024.0)) / (r.median_ms / 1000.0)
                        : 0;
   std::cout << std::left
             << std::setw(38) << r.name
             << std::right
             << std::setw(10) << fmt_size(r.size)
             << std::setw(13) << std::fixed << std::setprecision(4) << r.median_ms
             << std::setw(12) << std::fixed << std::setprecision(4) << r.min_ms
             << std::setw(12) << std::fixed << std::setprecision(4) << r.max_ms
             << std::setw(14) << std::fixed << std::setprecision(1) << mb_per_s
             << "\n";
}

}  // namespace

int main() {
   std::cout << "Keccak / SHA3 microbenchmark (warmup=2, median of runs)\n";
   std::cout << "sizes: 32 B (small) / 4 KiB / 1 MiB / 33 MiB (default max wasm linear memory)\n\n";

   print_header();

   const std::vector<std::pair<size_t, int>> scenarios = {
      {32, 1001},
      {4 * 1024, 501},
      {1024 * 1024, 51},
      {33 * 1024 * 1024, 11},
   };

   for (auto [sz, runs] : scenarios) {
      auto buf = make_buffer(sz);

      auto r1 = time_it("ethash::keccak256 (one-shot)", sz, runs, [&]() {
         auto h = ethash::keccak256(buf.data(), buf.size());
         sink(h.bytes, 32);
      });
      print(r1);

      auto r2 = time_it("fc::sha3 keccak (one-shot)", sz, runs, [&]() {
         auto h = fc::sha3::hash(reinterpret_cast<const char*>(buf.data()),
                                 static_cast<uint32_t>(buf.size()), false);
         sink(h.data(), 32);
      });
      print(r2);

      auto r3 = time_it("fc::sha3 nist (one-shot)", sz, runs, [&]() {
         auto h = fc::sha3::hash(reinterpret_cast<const char*>(buf.data()),
                                 static_cast<uint32_t>(buf.size()), true);
         sink(h.data(), 32);
      });
      print(r3);

      // Chunked at 10 KiB to mirror the WASM intrinsic's per-block checktime
      // loop (sysio::chain::config::hashing_checktime_block_size). Isolates
      // the streaming-vs-one-shot overhead for fc::sha3.
      auto r4 = time_it("fc::sha3 keccak (10 KiB chunks)", sz, runs, [&]() {
         fc::sha3::encoder enc;
         constexpr size_t bs = 10 * 1024;
         const char* d = reinterpret_cast<const char*>(buf.data());
         size_t left = buf.size();
         while (left > bs) {
            enc.write(d, bs);
            d += bs;
            left -= bs;
         }
         enc.write(d, static_cast<uint32_t>(left));
         auto h = enc.result(false);
         sink(h.data(), 32);
      });
      print(r4);

      // Final cross-impl agreement check at the scenario's input size.
      // Complements ethash_fc_sha3_cross_impl_agreement in
      // libraries/libfc/test/crypto/test_hash_functions.cpp, which covers
      // smaller corpora; the bench reaches sizes up to 33 MiB which the
      // test suite does not exercise.
      auto h_ethash = ethash::keccak256(buf.data(), buf.size());
      auto h_fcsha3 = fc::sha3::hash(reinterpret_cast<const char*>(buf.data()),
                                     static_cast<uint32_t>(buf.size()), false);
      if (std::memcmp(h_ethash.bytes, h_fcsha3.data(), 32) != 0) {
         std::cout << "  !! cross-impl MISMATCH at size " << fmt_size(sz) << "\n";
         return 1;
      }

      std::cout << "\n";
   }

   std::cout << "Cross-impl agreement (ethash vs fc::sha3 keccak, all sizes): OK\n";
   return 0;
}
