#include <benchmark.hpp>

#include <sysio/trace_api/request_handler.hpp>
#include <sysio/trace_api/trace.hpp>

#include <fc/crypto/hex.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/io/json.hpp>
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

      // Legacy path: process_block -> variant -> json::to_string.  Times the full
      // chain the HTTP handler would execute.
      benchmarking("variant: " + label_prefix, [&]() {
         auto v = detail::response_formatter::process_block(entry, irreversible, noop_handler);
         std::string out = fc::json::to_string(v, fc::json::yield_function_t());
         // Defeat the optimizer.
         asm volatile("" : : "g"(out.size()) : "memory");
      });

      // Streaming path: process_block_to_json writes directly into std::string.
      benchmarking("stream:  " + label_prefix, [&]() {
         std::string out = detail::response_formatter::process_block_to_json(entry, irreversible, noop_handler);
         asm volatile("" : : "g"(out.size()) : "memory");
      });
   }
}

} // namespace sysio::benchmark
