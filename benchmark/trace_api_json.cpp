#include <benchmark.hpp>

#include <sysio/trace_api/request_handler.hpp>
#include <sysio/trace_api/trace.hpp>

#include <fc/crypto/hex.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/io/json.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/variant.hpp>

#include <random>
#include <string>
#include <vector>

// Compares the two trace_api block serialization paths:
//   1) fc::mutable_variant_object -> fc::variant -> fc::json::to_string  (legacy)
//   2) fc::json_writer streaming directly into std::string               (this PR)
//
// Each run serializes a fresh synthetic block_trace_v0 with a configurable number of
// transactions and actions-per-transaction.  The shapes and field types match what
// real producers emit, so the comparison reflects realistic HTTP workloads.

namespace sysio::benchmark {

namespace {

using namespace sysio::trace_api;

// Deterministic filler for reproducible timings across runs.  Uses a stable PRNG so
// the synthetic trace's byte payloads don't change shape between "variant" and
// "stream" timings - otherwise any compiler/cache effect would dominate the signal.
std::mt19937_64 make_rng() { return std::mt19937_64{0xDEADBEEFCAFEF00Dull}; }

chain::bytes random_bytes(std::mt19937_64& rng, size_t n) {
   chain::bytes b(n);
   std::uniform_int_distribution<int> byte_dist{0, 255};
   for (auto& c : b) c = static_cast<char>(byte_dist(rng));
   return b;
}

const chain::signature_type& fake_signature() {
   // One real K1 signature generated once at first call, reused across all synthetic
   // trxs.  We're measuring serialization cost of the signature type, not the
   // number of distinct signatures, so reuse is fine.
   static const chain::signature_type s = []{
      auto priv = fc::crypto::private_key::generate();
      return priv.sign(fc::sha256::hash(std::string{"benchmark-digest"}));
   }();
   return s;
}

block_trace_v0 make_synthetic_block_trace(size_t num_trxs, size_t actions_per_trx, size_t action_data_bytes) {
   auto rng = make_rng();

   block_trace_v0 bt;
   bt.id          = fc::sha256::hash(std::string{"block-id"});
   bt.number      = 123456;
   bt.previous_id = fc::sha256::hash(std::string{"prev-id"});
   bt.timestamp   = chain::block_timestamp_type(1'700'000'000u);
   bt.producer    = chain::name{"defproducera"};
   bt.transaction_mroot = fc::sha256::hash(std::string{"trx-mroot"});
   bt.finality_mroot    = fc::sha256::hash(std::string{"fin-mroot"});

   bt.transactions.reserve(num_trxs);
   for (size_t i = 0; i < num_trxs; ++i) {
      transaction_trace_v0 trx;
      trx.id = fc::sha256::hash(std::string{"trx-"} + std::to_string(i));
      trx.actions.reserve(actions_per_trx);
      uint64_t seq = i * actions_per_trx;
      for (size_t a = 0; a < actions_per_trx; ++a) {
         action_trace_v0 act;
         act.global_sequence = seq++;
         act.receiver        = chain::name{"sysio.token"};
         act.account         = chain::name{"sysio.token"};
         act.action          = chain::name{"transfer"};
         act.authorization.push_back({chain::name{"alice"}, chain::name{"active"}});
         act.data            = random_bytes(rng, action_data_bytes);
         act.return_value    = {};
         trx.actions.push_back(std::move(act));
      }
      trx.status          = chain::transaction_receipt_header::executed;
      trx.cpu_usage_us    = 150;
      trx.net_usage_words = fc::unsigned_int{16};
      trx.signatures.push_back(fake_signature());
      trx.trx_header.expiration    = fc::time_point_sec{1'700'000'100u};
      trx.trx_header.ref_block_num = static_cast<uint16_t>(i);
      trx.trx_header.ref_block_prefix = 0xcafebabe;
      trx.block_num       = bt.number;
      trx.block_time      = bt.timestamp;
      trx.producer_block_id = bt.id;
      bt.transactions.push_back(std::move(trx));
   }
   return bt;
}

// A data_handler that does no ABI decode (returns a null params variant).  The two
// paths hit the same no-op branch, so this isolates the envelope serialization cost
// from the ABI decode cost - which is shared unchanged between paths.
data_handler_function make_noop_data_handler() {
   return [](const std::variant<action_trace_v0>&) -> std::tuple<fc::variant, std::optional<fc::variant>> {
      return {fc::variant(), std::optional<fc::variant>{}};
   };
}

} // namespace

void trace_api_json_benchmarking() {
   using namespace sysio::trace_api;

   // Real produced blocks from the perf harness peak at ~25k trxs with 1 action each.
   // Benchmark several shapes so the scaling (per-trx, per-action, payload bytes) is
   // visible.
   struct shape { size_t trxs; size_t actions_per_trx; size_t data_bytes; };
   const shape shapes[] = {
      { 100,    1,   32 },   // small block, common action
      { 5'000,  1,   32 },   // medium block
      { 25'000, 1,   32 },   // high-TPS block size
      { 1'000,  4,   128 },  // complex trxs, modest block
   };

   const auto noop_handler = make_noop_data_handler();
   const bool irreversible = true;

   for (const auto& s : shapes) {
      const auto bt = make_synthetic_block_trace(s.trxs, s.actions_per_trx, s.data_bytes);
      const auto entry = data_log_entry{bt};

      const std::string label_prefix = std::to_string(s.trxs) + " trxs x "
                                     + std::to_string(s.actions_per_trx) + " acts ("
                                     + std::to_string(s.data_bytes) + "B data)";

      // Legacy total path: build variant tree + json::to_string.  Times the full
      // chain the HTTP handler would execute (variant build on main thread, then
      // json::to_string on HTTP thread).  Useful as a total-CPU baseline.
      benchmarking("variant+json: " + label_prefix, [&]() {
         auto v = detail::response_formatter::process_block(entry, irreversible, noop_handler);
         std::string out = fc::json::to_string(v, fc::json::yield_function_t());
         // Defeat the optimizer.
         asm volatile("" : : "g"(out.size()) : "memory");
      });

      // Variant-only: build the variant tree without serializing to JSON.  This is
      // the actual current main-thread cost in production - json::to_string runs on
      // an HTTP-pool thread after cb() hands the variant off.  Compare against
      // `stream:` below to decide whether moving JSON emission onto the main thread
      // is a net main-thread win or regression.
      benchmarking("variant-only: " + label_prefix, [&]() {
         auto v = detail::response_formatter::process_block(entry, irreversible, noop_handler);
         asm volatile("" : : "g"(v.is_object()) : "memory");
      });

      // Streaming path: process_block_to_json writes JSON directly into std::string.
      // This is the proposed new main-thread cost (no variant intermediate, no
      // separate HTTP-thread serialization step).
      benchmarking("stream:       " + label_prefix, [&]() {
         std::string out = detail::response_formatter::process_block_to_json(entry, irreversible, noop_handler);
         asm volatile("" : : "g"(out.size()) : "memory");
      });

      // Struct-pass (HTTP-thread streaming pivot floor): main-thread cost when
      // the API lambda hands the result struct (not a variant, not a string) to
      // the HTTP thread for serialization.  API lambda captures the struct into
      // a std::function<void(json_writer&)> closure and hands it to cb(); the
      // HTTP thread runs the closure later (off-main, not measured here).  The
      // capture is move-only so the per-trx vectors transfer pointers, not data.
      // std::function does heap-allocate because the captured block_trace_v0
      // exceeds the small-buffer optimisation threshold.
      //
      // Pre-builds a pool of data_log_entry copies outside the timing loop so
      // each iter consumes a fresh one.  Setup cost is excluded from the bench
      // (matches variant-only / stream which also start from a pre-built entry).
      const size_t pool_size = 64; // > any reasonable --runs
      std::vector<data_log_entry> fresh_a(pool_size, entry);
      std::vector<data_log_entry> fresh_b(pool_size, entry);
      size_t fresh_idx_a = 0;
      size_t fresh_idx_b = 0;
      benchmarking("struct-pass (fn):  " + label_prefix, [&]() {
         data_log_entry e = std::move(fresh_a[fresh_idx_a++ % pool_size]);
         std::function<void(fc::json_writer&)> body =
            [e = std::move(e)](fc::json_writer& w) mutable {
               asm volatile("" : : "g"(&e) : "memory");
            };
         auto holder = std::move(body); // mimic cb() posting onto the HTTP pool
         asm volatile("" : : "g"(&holder) : "memory");
      });
      benchmarking("struct-pass (mof): " + label_prefix, [&]() {
         data_log_entry e = std::move(fresh_b[fresh_idx_b++ % pool_size]);
         std::move_only_function<void(fc::json_writer&)> body =
            [e = std::move(e)](fc::json_writer& w) mutable {
               asm volatile("" : : "g"(&e) : "memory");
            };
         auto holder = std::move(body);
         asm volatile("" : : "g"(&holder) : "memory");
      });
   }

   // ---- Isolated hex-emission micro-bench ------------------------------------
   //
   // Probes the hex-encoding cost in isolation, the suspected dominant cause of
   // the streaming-vs-variant-only main-thread regression on data-heavy actions.
   // Two paths per byte size:
   //   to_hex+value_string : fc::to_hex(...) -> std::string -> w.value_string(sv)
   //   value_hex           : json_writer::value_hex writes hex straight into the buffer
   //
   // Each iteration emits a single JSON string token of N hex chars, so the delta
   // is the per-byte hex-encoding overhead (plus one std::string alloc/dealloc on
   // the to_hex path).  Higher N amortises the prefix/quote bookkeeping and
   // surfaces the loop cost.

   {
      auto rng = make_rng();
      const size_t hex_iter_bytes_total = 64 * 1024; // ~constant work per scenario
      const size_t sizes[] = { 32, 128, 512, 2048 };
      for (size_t bytes_per_call : sizes) {
         auto buf = random_bytes(rng, bytes_per_call);
         const size_t calls = std::max<size_t>(1, hex_iter_bytes_total / bytes_per_call);
         const std::string label = std::to_string(bytes_per_call) + "B per call x " + std::to_string(calls);

         benchmarking("hex variant:  " + label, [&]() {
            std::string out;
            fc::json_writer w(out);
            w.begin_array();
            for (size_t i = 0; i < calls; ++i) {
               w.value_string(fc::to_hex(buf.data(), buf.size()));
            }
            w.end_array();
            asm volatile("" : : "g"(out.size()) : "memory");
         });

         benchmarking("hex direct:   " + label, [&]() {
            std::string out;
            fc::json_writer w(out);
            w.begin_array();
            for (size_t i = 0; i < calls; ++i) {
               w.value_hex(buf.data(), buf.size());
            }
            w.end_array();
            asm volatile("" : : "g"(out.size()) : "memory");
         });
      }
   }

   // ---- Isolated integer-emission micro-bench --------------------------------
   //
   // Probes the integer-to-decimal cost in isolation, comparing snprintf (the
   // pre-change implementation of json_writer::value_uint64) against std::to_chars
   // (the post-change implementation).  Each iteration emits 1024 uint64 values
   // covering a realistic mix of magnitudes: small (status / counters), medium
   // (block numbers, cpu_usage_us), large (global_sequence on a long-running
   // chain).  Values are pre-shuffled so branch prediction can't dominate.

   {
      auto rng = make_rng();
      std::vector<uint64_t> mix;
      mix.reserve(1024);
      for (size_t i = 0; i < 1024; ++i) {
         std::uniform_int_distribution<int> bucket{0, 2};
         switch (bucket(rng)) {
            case 0: mix.push_back(rng() % 256); break;          // small
            case 1: mix.push_back(rng() % 100'000'000ull); break; // medium
            case 2: mix.push_back(rng()); break;                // large (full uint64)
         }
      }

      auto append_snprintf = [](std::string& out, uint64_t n) {
         char buf[24];
         int k = std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
         if (k > 0) out.append(buf, static_cast<size_t>(k));
      };

      auto append_to_chars = [](std::string& out, uint64_t n) {
         char buf[24];
         auto r = std::to_chars(buf, buf + sizeof(buf), n);
         out.append(buf, static_cast<size_t>(r.ptr - buf));
      };

      benchmarking("uint64 snprintf:  1024 mixed", [&]() {
         std::string out;
         out.reserve(16 * 1024);
         for (uint64_t n : mix) {
            append_snprintf(out, n);
            out.push_back(',');
         }
         asm volatile("" : : "g"(out.size()) : "memory");
      });

      benchmarking("uint64 to_chars:  1024 mixed", [&]() {
         std::string out;
         out.reserve(16 * 1024);
         for (uint64_t n : mix) {
            append_to_chars(out, n);
            out.push_back(',');
         }
         asm volatile("" : : "g"(out.size()) : "memory");
      });
   }
}

} // namespace sysio::benchmark
