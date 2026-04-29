#include <benchmark.hpp>

#include <sysio/chain/abi_def.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/action.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/block.hpp>
#include <sysio/chain/transaction.hpp>

#include <fc/io/json.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/reflect/json_stream.hpp>
#include <fc/variant.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

// Microbenchmark for sysio::chain::abi_serializer.
//
// Measures construction cost and steady-state decode throughput so that
// successive optimization commits can report a verifiable before/after
// delta.  NOT a correctness test -- abi_tests.cpp covers that.
//
// Run via the unified benchmark harness:
//
//   ninja -C cmake-build-release -j8 benchmark
//   ./cmake-build-release/benchmark/benchmark abi_serializer --num-runs 10
//
// Release build (`-DCMAKE_BUILD_TYPE=Release`, `-O3`) is required for
// comparable numbers; debug / RelWithDebInfo timings are dominated by
// instrumentation.  Each scenario does its own inner loop sized to keep
// per-call overhead amortised; the harness's `--num-runs` controls how
// many times the outer scenario is repeated for variance reporting.

namespace sysio::benchmark {

using namespace sysio::chain;
using bytes = sysio::benchmark::bytes;

namespace {

constexpr fc::microseconds max_serialization_time = fc::microseconds{1'000'000};

abi_serializer::yield_function_t yield_fn() {
   return abi_serializer::create_yield_function(max_serialization_time);
}

std::string slurp(const std::filesystem::path& p) {
   std::ifstream f(p);
   if (!f) {
      std::cerr << "abi_serializer benchmark: failed to open " << p << "\n";
      std::exit(1);
   }
   std::stringstream ss;
   ss << f.rdbuf();
   return ss.str();
}

} // namespace

void abi_serializer_benchmarking() {
   const std::filesystem::path token_path  = TOKEN_ABI_PATH;
   const std::filesystem::path system_path = SYSTEM_ABI_PATH;

   const auto token_abi  = fc::json::from_string(slurp(token_path)).as<abi_def>();
   const auto system_abi = fc::json::from_string(slurp(system_path)).as<abi_def>();

   // Prebuild serializers + packed payloads for the steady-state decode
   // scenarios.  Cost of these setup steps is NOT in the measured region.
   abi_serializer token_abis{abi_def(token_abi), yield_fn()};
   abi_serializer system_abis{abi_def(system_abi), yield_fn()};

   const auto transfer_var = fc::json::from_string(
      R"({"from":"alice","to":"bob","quantity":"1.0000 SYS","memo":"benchmark payment"})");
   chain::bytes transfer_bytes;
   try {
      transfer_bytes = token_abis.variant_to_binary("transfer", transfer_var, yield_fn());
   } catch (const std::exception& e) {
      std::cerr << "abi_serializer benchmark: transfer pack failed: " << e.what() << "\n";
      return;
   }

   // newaccount exercises a nested struct path (authority sub-struct + array
   // of key_weights).  An authority with one key is enough to cover the
   // nested+array code paths without bloating the fixture.
   const auto newaccount_var = fc::json::from_string(
      R"({"creator":"sysio","name":"alice","owner":)"
      R"({"threshold":1,"keys":[{"key":"SYS1111111111111111111111111111111114T1Anm","weight":1}],)"
      R"("accounts":[],"waits":[]},"active":)"
      R"({"threshold":1,"keys":[{"key":"SYS1111111111111111111111111111111114T1Anm","weight":1}],)"
      R"("accounts":[],"waits":[]}})");
   chain::bytes newaccount_bytes;
   try {
      newaccount_bytes = system_abis.variant_to_binary("newaccount", newaccount_var, yield_fn());
   } catch (const std::exception& e) {
      std::cerr << "abi_serializer benchmark: newaccount pack failed: " << e.what() << "\n";
      return;
   }

   // regproducer -- flat-struct baseline comparable to transfer but in a
   // larger ABI context.  Measures per-field dispatch amortised over the
   // whole-action cost in the big-ABI case.
   chain::bytes regproducer_bytes;
   bool regproducer_ok = false;
   try {
      const auto regproducer_var = fc::json::from_string(
         R"({"producer":"alice","producer_key":"SYS1111111111111111111111111111111114T1Anm",)"
         R"("url":"https://example.com","location":0})");
      regproducer_bytes = system_abis.variant_to_binary("regproducer", regproducer_var, yield_fn());
      regproducer_ok = true;
   } catch (const std::exception& e) {
      std::cerr << "abi_serializer benchmark warning: regproducer prepack failed (" << e.what()
                << "); skipping regproducer scenarios.\n";
   }

   // Construction scenarios.  Each construction is slow enough that one outer
   // run is meaningful; no inner loop needed.
   benchmarking("abi_serializer construct token (small ~4KB)", [&] {
      abi_serializer a{abi_def(token_abi), yield_fn()};
      (void)a;
   });
   benchmarking("abi_serializer construct system (~37KB)", [&] {
      abi_serializer a{abi_def(system_abi), yield_fn()};
      (void)a;
   });

   // Steady-state decode (variant path).  Inner loop sized to keep per-op
   // overhead well above timer resolution.
   constexpr size_t decode_iters     = 10000;
   constexpr size_t big_decode_iters = 5000;

   benchmarking("decode transfer x10000 (variant)", [&] {
      for (size_t i = 0; i < decode_iters; ++i) {
         auto v = token_abis.binary_to_variant("transfer", transfer_bytes, yield_fn());
         (void)v;
      }
   });
   benchmarking("decode newaccount x5000 (variant, nested)", [&] {
      for (size_t i = 0; i < big_decode_iters; ++i) {
         auto v = system_abis.binary_to_variant("newaccount", newaccount_bytes, yield_fn());
         (void)v;
      }
   });
   if (regproducer_ok) {
      benchmarking("decode regproducer x5000 (variant)", [&] {
         for (size_t i = 0; i < big_decode_iters; ++i) {
            auto v = system_abis.binary_to_variant("regproducer", regproducer_bytes, yield_fn());
            (void)v;
         }
      });
   }

   // Round-trip pack+unpack of transfer -- covers the pack half, not just unpack.
   benchmarking("roundtrip transfer x5000 (pack+unpack)", [&] {
      for (size_t i = 0; i < big_decode_iters; ++i) {
         auto bin = token_abis.variant_to_binary("transfer", transfer_var, yield_fn());
         auto var = token_abis.binary_to_variant("transfer", bin, yield_fn());
         (void)var;
      }
   });

   // resolve_type on a typedef ("account_name" -> "name") -- typedef chain walk.
   benchmarking("resolve_type account_name x100000", [&] {
      const std::string probe = "account_name";
      for (size_t i = 0; i < 100000; ++i) {
         auto r = system_abis.resolve_type(probe);
         (void)r;
      }
   });

   // -------------------------------------------------------------------------
   // Streaming JSON comparisons.  Each pair measures the same payload twice:
   //   - "(variant) json": binary_to_variant + fc::json::to_string
   //   - "(stream) json" : binary_to_json_stream directly into a json_writer
   //
   // The streaming path skips the per-field fc::variant tree and the post-pass
   // string serialiser, so the gap should grow with field count.
   // -------------------------------------------------------------------------

   benchmarking("transfer  x10000 (variant) json", [&] {
      for (size_t i = 0; i < decode_iters; ++i) {
         auto v = token_abis.binary_to_variant("transfer", transfer_bytes, yield_fn());
         auto s = fc::json::to_string(v, fc::time_point::maximum());
         (void)s;
      }
   });
   benchmarking("transfer  x10000 (stream)  json", [&] {
      for (size_t i = 0; i < decode_iters; ++i) {
         std::string out;
         out.reserve(256);
         fc::json_writer w(out);
         token_abis.binary_to_json_stream("transfer", transfer_bytes, w, yield_fn());
      }
   });

   benchmarking("newaccount x5000 (variant) json", [&] {
      for (size_t i = 0; i < big_decode_iters; ++i) {
         auto v = system_abis.binary_to_variant("newaccount", newaccount_bytes, yield_fn());
         auto s = fc::json::to_string(v, fc::time_point::maximum());
         (void)s;
      }
   });
   benchmarking("newaccount x5000 (stream)  json", [&] {
      for (size_t i = 0; i < big_decode_iters; ++i) {
         std::string out;
         out.reserve(512);
         fc::json_writer w(out);
         system_abis.binary_to_json_stream("newaccount", newaccount_bytes, w, yield_fn());
      }
   });

   if (regproducer_ok) {
      benchmarking("regproducer x5000 (variant) json", [&] {
         for (size_t i = 0; i < big_decode_iters; ++i) {
            auto v = system_abis.binary_to_variant("regproducer", regproducer_bytes, yield_fn());
            auto s = fc::json::to_string(v, fc::time_point::maximum());
            (void)s;
         }
      });
      benchmarking("regproducer x5000 (stream)  json", [&] {
         for (size_t i = 0; i < big_decode_iters; ++i) {
            std::string out;
            out.reserve(256);
            fc::json_writer w(out);
            system_abis.binary_to_json_stream("regproducer", regproducer_bytes, w, yield_fn());
         }
      });
   }

   // -------------------------------------------------------------------------
   // get_block-shape comparison.  Builds a synthetic signed_block holding a
   // single transaction with N transfer actions, then times two paths:
   //   - variant: abi_serializer::to_variant<signed_block> + json::to_string
   //              (mirrors chain_plugin::convert_block)
   //   - stream:  abi_serializer::to_json_stream<signed_block> straight into
   //              a json_writer (mirrors convert_block_stream)
   // The actions count surfaces the per-action ABI decode cost, which is the
   // variant path's main allocation hot spot.
   // -------------------------------------------------------------------------

   constexpr size_t actions_per_trx = 4;
   constexpr size_t block_iters     = 2000;

   transaction inner_trx;
   inner_trx.expiration       = fc::time_point_sec{ fc::time_point::now() + fc::seconds(60) };
   inner_trx.ref_block_num    = 1;
   inner_trx.ref_block_prefix = 1;
   for (size_t i = 0; i < actions_per_trx; ++i) {
      action act;
      act.account       = "sysio.token"_n;
      act.name          = "transfer"_n;
      act.authorization = { { "alice"_n, "active"_n } };
      act.data          = transfer_bytes;
      inner_trx.actions.push_back(std::move(act));
   }

   signed_transaction signed_trx{ std::move(inner_trx), {}, {} };
   packed_transaction ptrx(std::move(signed_trx), packed_transaction::compression_type::none);
   transaction_receipt rcpt(std::move(ptrx));

   signed_block block;
   block.producer  = "alice"_n;
   block.timestamp = block_timestamp_type{};
   block.transactions.push_back(std::move(rcpt));

   // Resolver: sysio.token -> the prebuilt token_abis; other accounts -> nullopt
   // (matches production chain_plugin behaviour for unknown accounts).
   auto block_resolver = [&token_abis](const name& acct) -> std::optional<abi_serializer> {
      if (acct == "sysio.token"_n) return token_abis;
      return {};
   };

   benchmarking("block (4 transfers) x2000 (variant) json", [&] {
      for (size_t i = 0; i < block_iters; ++i) {
         fc::variant pretty;
         abi_serializer::to_variant(block, pretty, block_resolver, max_serialization_time);
         auto s = fc::json::to_string(pretty, fc::time_point::maximum());
         (void)s;
      }
   });
   benchmarking("block (4 transfers) x2000 (stream)  json", [&] {
      for (size_t i = 0; i < block_iters; ++i) {
         std::string out;
         out.reserve(2048);
         fc::json_writer w(out);
         abi_serializer::to_json_stream(block, w, block_resolver, max_serialization_time);
      }
   });

   // -------------------------------------------------------------------------
   // get_block-shape comparison INCLUDING per-request resolver build.  The
   // scenarios above hold the abi_serializer constant across iterations -- that
   // measures only the walk + emit cost.  In production each /v1/chain/get_block
   // request rebuilds the resolver from the captured abi bytes (Phase 2 of
   // read_only::get_block_stream_async), so the per-request cost includes
   // abi_serializer::to_abi(bytes, abi_def) + abi_serializer ctor for every
   // unique account referenced in the block.  These scenarios capture that.
   //
   // Setup: pack the parsed token ABI to raw bytes once (matches what
   // capture_abis would produce), then in each iteration parse + construct +
   // walk + emit, simulating the full Phase 2 + Phase 3 work for one request.
   // -------------------------------------------------------------------------

   const chain::bytes token_abi_bytes = fc::raw::pack(token_abi);

   constexpr size_t block_full_iters = 500;

   benchmarking("get_block-shape x500 (variant + abi build) json", [&] {
      for (size_t i = 0; i < block_full_iters; ++i) {
         abi_def abi;
         abi_serializer::to_abi(token_abi_bytes, abi);
         abi_serializer fresh{std::move(abi), yield_fn()};
         auto resolver = [&fresh](const name& acct) -> std::optional<abi_serializer> {
            if (acct == "sysio.token"_n) return fresh;
            return {};
         };
         fc::variant pretty;
         abi_serializer::to_variant(block, pretty, resolver, max_serialization_time);
         auto s = fc::json::to_string(pretty, fc::time_point::maximum());
         (void)s;
      }
   });
   benchmarking("get_block-shape x500 (stream + abi build)  json", [&] {
      for (size_t i = 0; i < block_full_iters; ++i) {
         abi_def abi;
         abi_serializer::to_abi(token_abi_bytes, abi);
         abi_serializer fresh{std::move(abi), yield_fn()};
         auto resolver = [&fresh](const name& acct) -> std::optional<abi_serializer> {
            if (acct == "sysio.token"_n) return fresh;
            return {};
         };
         std::string out;
         out.reserve(2048);
         fc::json_writer w(out);
         abi_serializer::to_json_stream(block, w, resolver, max_serialization_time);
      }
   });
}

} // namespace sysio::benchmark
