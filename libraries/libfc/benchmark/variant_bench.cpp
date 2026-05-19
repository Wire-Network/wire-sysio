// Microbenchmark for fc::variant and fc::variant_object.
//
// Measures the construction, copy, lookup, and conversion paths that the
// fc::variant performance follow-on series targets, so each subsequent
// optimization commit can report a verifiable before/after delta.  NOT a
// correctness test -- test/variant/test_variant*.cpp covers behaviour.
//
// Style mirrors libraries/chain/benchmark/abi_serializer_bench.cpp: plain
// chrono timing, no external benchmark dependency, one stand-alone
// executable that is EXCLUDE_FROM_ALL and run manually.  Each scenario
// runs a warmup phase, then `runs` rounds of `iters` ops, and reports the
// median ns/op (median dampens context-switch and thermal outliers more
// reliably than the mean for a tool intended to run on developer laptops).
//
// Build (Release is required -- Debug / RelWithDebInfo numbers are not
// comparable and should not be posted):
//
//   ninja -C cmake-build-release -j8 variant_bench
//   ./cmake-build-release/libraries/libfc/benchmark/variant_bench
//
// See BASELINES.md for the scenario catalogue and the running log.

#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using std::chrono::duration_cast;
using std::chrono::nanoseconds;
using std::chrono::steady_clock;

namespace {

struct result {
   std::string name;
   double median_ns_per_op = 0;
   double min_ns_per_op = 0;
   double max_ns_per_op = 0;
};

// Sink keeps the optimizer from eliding a measured expression whose
// observable result is otherwise unused.  doNotOptimize-style asm clobber
// would be more authoritative but is platform-specific; a volatile sink is
// portable and adequate at the granularities measured here.
volatile uint64_t g_sink = 0;

template <typename T>
inline void sink(const T& v) {
   if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
      g_sink ^= static_cast<uint64_t>(v);
   } else if constexpr (std::is_floating_point_v<T>) {
      g_sink ^= static_cast<uint64_t>(v * 1e6);
   } else {
      g_sink ^= reinterpret_cast<uintptr_t>(&v);
   }
}

template <typename Fn>
result run_bench(const std::string& name, size_t warmup, size_t iters, size_t runs, Fn&& fn) {
   for (size_t i = 0; i < warmup; ++i) fn();

   std::vector<double> per_op;
   per_op.reserve(runs);
   for (size_t r = 0; r < runs; ++r) {
      auto start = steady_clock::now();
      for (size_t i = 0; i < iters; ++i) fn();
      auto end = steady_clock::now();
      const double ns = static_cast<double>(duration_cast<nanoseconds>(end - start).count());
      per_op.push_back(ns / static_cast<double>(iters));
   }
   std::sort(per_op.begin(), per_op.end());
   return {name, per_op[per_op.size() / 2], per_op.front(), per_op.back()};
}

void print_header() {
   std::cout << std::left
             << std::setw(36) << "benchmark"
             << std::right
             << std::setw(14) << "median_ns/op"
             << std::setw(14) << "min_ns/op"
             << std::setw(14) << "max_ns/op"
             << "\n";
   std::cout << std::string(78, '-') << "\n";
}

void print_row(const result& r) {
   std::cout << std::left
             << std::setw(36) << r.name
             << std::right << std::fixed << std::setprecision(1)
             << std::setw(14) << r.median_ns_per_op
             << std::setw(14) << r.min_ns_per_op
             << std::setw(14) << r.max_ns_per_op
             << "\n";
}

// Build a representative ABI-decoded row: 50 fields, mix of int / string /
// bool / nested object.  This is the shape of an outenvelopes row at scale
// and the per-row cost driver in get_table_rows.
fc::mutable_variant_object make_50key_row() {
   fc::mutable_variant_object mvo;
   for (int i = 0; i < 30; ++i) {
      mvo("k_int_" + std::to_string(i), int64_t{i} * 1000);
   }
   for (int i = 0; i < 15; ++i) {
      mvo("k_str_" + std::to_string(i), std::string("value_") + std::to_string(i));
   }
   mvo("k_bool", true);
   mvo("k_double", 3.14159265);
   fc::mutable_variant_object nested;
   nested("inner_a", 1)("inner_b", "two")("inner_c", false);
   mvo("k_nested", fc::variant_object(nested));
   mvo("k_array", fc::variants{fc::variant(int64_t{1}), fc::variant(int64_t{2}), fc::variant(int64_t{3})});
   return mvo;
}

// Same shape as a JSON string -- exercises json::from_string and the build
// path inside variant/variant_object.
std::string make_50key_json() {
   std::ostringstream out;
   out << "{";
   bool first = true;
   for (int i = 0; i < 30; ++i) {
      if (!first) out << ",";
      first = false;
      out << "\"k_int_" << i << "\":" << (i * 1000);
   }
   for (int i = 0; i < 15; ++i) {
      out << ",\"k_str_" << i << "\":\"value_" << i << "\"";
   }
   out << R"(,"k_bool":true,"k_double":3.14159265,"k_nested":{"inner_a":1,"inner_b":"two","inner_c":false},"k_array":[1,2,3]})";
   return out.str();
}

// An enum-shaped string variant -- representative of the case
// `as_enum_value` hits when the ABI emitted the enum as a numeric string.
const fc::variant& enum_string_variant() {
   static const fc::variant v{std::string{"42"}};
   return v;
}

const fc::variant& enum_int_variant() {
   static const fc::variant v{int64_t{42}};
   return v;
}

const fc::variant& enum_bad_string_variant() {
   static const fc::variant v{std::string{"not_a_number"}};
   return v;
}

enum class probe_enum : int { zero = 0, fortytwo = 42 };

} // namespace

int main() {
   print_header();

   // ------------------------------------------------------------------
   // Construction.  Any change to the variant ctor or the variant_object
   // default ctor lands here first.
   // ------------------------------------------------------------------
   print_row(run_bench("ctor_null",                100000, 1000000, 10, [&] {
      fc::variant v;
      sink(v.get_type());
   }));
   print_row(run_bench("ctor_int64",               100000, 1000000, 10, [&] {
      fc::variant v{int64_t{42}};
      sink(v.get_type());
   }));
   print_row(run_bench("ctor_double",              100000, 1000000, 10, [&] {
      fc::variant v{3.14};
      sink(v.get_type());
   }));
   print_row(run_bench("ctor_short_string",         50000,  500000, 10, [&] {
      fc::variant v{"short"};
      sink(v.get_type());
   }));
   print_row(run_bench("ctor_sso_boundary_14",       50000,  500000, 10, [&] {
      // Exactly at the SSO threshold; still inline.
      fc::variant v{"fourteen_bytex"};
      sink(v.get_type());
   }));
   print_row(run_bench("ctor_just_over_sso_15",      20000,  200000, 10, [&] {
      // One byte past the threshold; falls back to heap.
      fc::variant v{"fifteen_bytes_x"};
      sink(v.get_type());
   }));
   print_row(run_bench("ctor_long_string",          20000,  100000, 10, [&] {
      // 64 chars -- well past the inline threshold; heap path.
      fc::variant v{"this is a sixty four character benchmark string ----- yyzzqqww"};
      sink(v.get_type());
   }));
   print_row(run_bench("ctor_empty_mvo",            50000,  500000, 10, [&] {
      // Phase A item 1 (lazy-allocate) targets this directly.  The
      // default ctor currently does make_shared<vector<entry>>;
      // expectation post-A1 is a meaningful drop.
      fc::mutable_variant_object mvo;
      sink(mvo.size());
   }));
   print_row(run_bench("ctor_empty_vo",             50000,  500000, 10, [&] {
      fc::variant_object vo;
      sink(vo.size());
   }));

   // ------------------------------------------------------------------
   // Copy.  Deep copy for heap-backed types is on every variant
   // assignment / pass-by-value.
   // ------------------------------------------------------------------
   {
      const fc::variant v_int{int64_t{42}};
      print_row(run_bench("copy_int64",              100000, 1000000, 10, [&] {
         fc::variant w{v_int};
         sink(w.get_type());
      }));
   }
   {
      const fc::variant v_short{"short"};
      print_row(run_bench("copy_short_string",        50000,  500000, 10, [&] {
         fc::variant w{v_short};
         sink(w.get_type());
      }));
   }
   {
      const fc::variant v_long{std::string(128, 'x')};
      print_row(run_bench("copy_long_string",         20000,  100000, 10, [&] {
         fc::variant w{v_long};
         sink(w.get_type());
      }));
   }
   {
      const fc::variant v_obj{make_50key_row()};
      print_row(run_bench("copy_object_50key",        20000,  100000, 10, [&] {
         fc::variant w{v_obj};
         sink(w.get_type());
      }));
   }

   // Same-type reassignment.  These rows watch Phase B item 5: when the
   // dest already holds a heap object of the matching type, op=(const&)
   // reuses that allocation instead of delete + new.
   {
      const fc::variant rhs{std::string(64, 'b')};
      fc::variant lhs{std::string(64, 'a')};
      print_row(run_bench("assign_long_string_to_long",  10000,  200000, 10, [&] {
         lhs = rhs;
         sink(lhs.get_type());
      }));
   }
   {
      const fc::variant rhs{make_50key_row()};
      fc::variant lhs{make_50key_row()};
      print_row(run_bench("assign_object_to_object",      10000,  100000, 10, [&] {
         lhs = rhs;
         sink(lhs.get_type());
      }));
   }
   {
      const fc::variant rhs{fc::variants{fc::variant(1), fc::variant(2), fc::variant(3),
                                          fc::variant(4), fc::variant(5)}};
      fc::variant lhs{fc::variants{fc::variant(0), fc::variant(0)}};
      print_row(run_bench("assign_array_to_array",        20000,  200000, 10, [&] {
         lhs = rhs;
         sink(lhs.get_type());
      }));
   }

   // ------------------------------------------------------------------
   // Lookup on variant_object.  O(N) linear scan today; Phase B item 4
   // adds an optional hash side-table.  Both sizes captured so we can
   // tell if a hash index helps small objects (it should not -- watch
   // for regression).
   // ------------------------------------------------------------------
   {
      fc::mutable_variant_object mvo;
      mvo("a", 1)("b", 2)("c", 3)("d", 4);
      const fc::variant_object vo{mvo};
      print_row(run_bench("find_hit_4key",           100000, 1000000, 10, [&] {
         auto it = vo.find("c");
         sink(it != vo.end());
      }));
      print_row(run_bench("find_miss_4key",          100000, 1000000, 10, [&] {
         auto it = vo.find("z");
         sink(it != vo.end());
      }));
   }
   {
      const fc::variant_object vo{make_50key_row()};
      print_row(run_bench("find_hit_50key_first",     50000,  500000, 10, [&] {
         auto it = vo.find("k_int_0");
         sink(it != vo.end());
      }));
      print_row(run_bench("find_hit_50key_last",      50000,  500000, 10, [&] {
         auto it = vo.find("k_array");
         sink(it != vo.end());
      }));
      print_row(run_bench("find_miss_50key",          50000,  500000, 10, [&] {
         auto it = vo.find("not_there");
         sink(it != vo.end());
      }));
      // Phase A item 2 (find_or) collapses contains+op[] into one scan.
      // This row captures the current double-scan cost.
      print_row(run_bench("contains_then_op_50key",   50000,  500000, 10, [&] {
         if (vo.contains("k_int_15")) {
            sink(vo["k_int_15"].as_int64());
         }
      }));
      // The find_or replacement: single scan, no throw on miss.
      const fc::variant default_v{int64_t{0}};
      print_row(run_bench("find_or_50key_hit",        50000,  500000, 10, [&] {
         sink(vo.find_or("k_int_15", default_v).as_int64());
      }));
      print_row(run_bench("find_or_50key_miss",       50000,  500000, 10, [&] {
         sink(vo.find_or("not_there", default_v).as_int64());
      }));
   }

   // ------------------------------------------------------------------
   // Conversions.  as_enum_value is the Phase A item 3 watch: stoll +
   // catch(...) gets replaced by from_chars (non-throwing, faster).
   // ------------------------------------------------------------------
   {
      const fc::variant& v_int = enum_int_variant();
      print_row(run_bench("as_enum_int",             100000, 1000000, 10, [&] {
         sink(v_int.as_enum_value<probe_enum>());
      }));
      const fc::variant& v_str = enum_string_variant();
      print_row(run_bench("as_enum_string_valid",     50000,  500000, 10, [&] {
         sink(v_str.as_enum_value<probe_enum>());
      }));
      // Bad-string is the throw-on-miss path.  Today this goes through
      // catch(...) inside as_enum_value and then throws runtime_error.
      // Iteration count is small to keep the run-time bounded.
      const fc::variant& v_bad = enum_bad_string_variant();
      print_row(run_bench("as_enum_string_invalid",     500,    5000, 10, [&] {
         try { sink(v_bad.as_enum_value<probe_enum>()); }
         catch (...) { /* expected */ }
      }));
   }
   {
      const fc::variant v_int{int64_t{1234567890}};
      print_row(run_bench("as_string_int64",          50000,  500000, 10, [&] {
         sink(v_int.as_string().size());
      }));
      const fc::variant v_str{std::string{"-1234567890"}};
      print_row(run_bench("as_int64_string",          50000,  500000, 10, [&] {
         sink(v_int.as_int64());
         (void)v_str;
      }));
   }

   // ------------------------------------------------------------------
   // Workload-shaped scenarios -- closer to /v1/chain/get_table_rows
   // and the OPP cron plugin scan loops.
   // ------------------------------------------------------------------
   {
      const std::string json = make_50key_json();
      print_row(run_bench("json_parse_50key",          5000,   25000, 10, [&] {
         auto v = fc::json::from_string(json);
         sink(v.get_object().size());
      }));

      const fc::variant parsed = fc::json::from_string(json);
      print_row(run_bench("json_to_string_50key",      5000,   25000, 10, [&] {
         auto s = fc::json::to_string(parsed, fc::time_point::maximum());
         sink(s.size());
      }));

      // walk: emulate from_variant<Row>() pulling each field by name.
      // Hits the find() linear scan once per field and pays the as_*
      // cost.  This is the pattern that motivates Phase B item 4 most.
      print_row(run_bench("walk_50key_by_name",        5000,   25000, 10, [&] {
         const auto& obj = parsed.get_object();
         int64_t acc = 0;
         for (int i = 0; i < 30; ++i) {
            acc ^= obj["k_int_" + std::to_string(i)].as_int64();
         }
         sink(acc);
      }));
   }

   return 0;
}
