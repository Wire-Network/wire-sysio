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

volatile uint8_t bench_sink = 0;
inline void sink(const void* p, size_t n) {
   const uint8_t* b = static_cast<const uint8_t*>(p);
   bench_sink ^= static_cast<uint8_t>(b[0] ^ b[n - 1]);
}

std::vector<uint8_t> make_buffer(size_t n) {
   std::mt19937_64 rng{0xC0FFEEULL};
   std::vector<uint8_t> v(n);
   for (auto& byte : v) byte = static_cast<uint8_t>(rng());
   return v;
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
             << std::setw(13) << "size(bytes)"
             << std::setw(13) << "median(ms)"
             << std::setw(12) << "min(ms)"
             << std::setw(12) << "max(ms)"
             << std::setw(14) << "MB/s(median)"
             << "\n";
   std::cout << std::string(102, '-') << "\n";
}

void print(const result& r) {
   double mb_per_s = r.median_ms > 0
                        ? (static_cast<double>(r.size) / (1024.0 * 1024.0)) / (r.median_ms / 1000.0)
                        : 0;
   std::cout << std::left
             << std::setw(38) << r.name
             << std::right
             << std::setw(13) << r.size
             << std::setw(13) << std::fixed << std::setprecision(4) << r.median_ms
             << std::setw(12) << std::fixed << std::setprecision(4) << r.min_ms
             << std::setw(12) << std::fixed << std::setprecision(4) << r.max_ms
             << std::setw(14) << std::fixed << std::setprecision(1) << mb_per_s
             << "\n";
}

}  // namespace

int main() {
   std::cout << "Keccak / SHA3 microbenchmark (warmup=2, median of runs)\n";
   std::cout << "sizes: 32 B (typical digest input) / 4 KiB / 1 MiB / 33 MiB (max wasm memory)\n\n";

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

      // Chunked at 10 KiB (the WASM intrinsic's hashing_checktime_block_size)
      // -- isolates the streaming-vs-one-shot overhead for fc::sha3.
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

      std::cout << "\n";
   }

   // Correctness smoke: ethash vs fc::sha3 keccak must agree byte-for-byte at
   // the largest size, otherwise consensus parity between the two impls is
   // already broken.
   auto buf = make_buffer(33 * 1024 * 1024);
   auto h_ethash = ethash::keccak256(buf.data(), buf.size());
   auto h_fcsha3 = fc::sha3::hash(reinterpret_cast<const char*>(buf.data()),
                                  static_cast<uint32_t>(buf.size()), false);
   bool agree = std::memcmp(h_ethash.bytes, h_fcsha3.data(), 32) == 0;
   std::cout << "Cross-impl agreement (ethash vs fc::sha3 keccak, 33 MiB): "
             << (agree ? "OK" : "MISMATCH") << "\n";

   return agree ? 0 : 1;
}
