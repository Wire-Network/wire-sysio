#include <benchmark.hpp>

#include <sysio/chain/abi_def.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/trace_api/request_handler.hpp>
#include <sysio/trace_api/trace.hpp>

#include <fc/crypto/hex.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/io/json.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/io/raw.hpp>
#include <fc/variant.hpp>

#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

// Times the trace_api responses, written by fc::json_writer directly into a std::string.
//
// Each run serializes a fresh synthetic block_trace_v0 with a configurable number of
// transactions and actions-per-transaction.  The shapes and field types match what
// real producers emit, so the timings reflect realistic HTTP workloads.

namespace sysio::benchmark {

namespace {

using namespace sysio::trace_api;

// Deterministic filler for reproducible timings across runs.  Uses a stable PRNG so
// the synthetic trace's byte payloads don't change shape between runs - otherwise any
// compiler/cache effect would dominate the signal.
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

// A data_handler that does no ABI decode (emits no params/return_data keys), isolating the
// envelope serialization cost from the ABI decode cost.
stream_data_handler_function make_noop_stream_data_handler() {
   return [](const std::variant<action_trace_v0>&, fc::json_writer&) {};
}

constexpr uint32_t    bench_slice_stride = 10'000;
constexpr chain::name token_account      = chain::string_to_name("sysio.token");
constexpr chain::name transfer_action    = chain::string_to_name("transfer");

// Serves an in-memory block range to request_handler, as store_provider serves the trace log.  get_block copies
// the block, as reading it from the log would, and both measured paths pay that copy.
struct bench_logfile_provider {
   std::shared_ptr<const std::map<uint32_t, block_trace_v0>> blocks;

   get_block_t get_block(uint32_t height) const {
      const auto it = blocks->find(height);
      if (it == blocks->end())
         return {};
      return std::make_tuple(data_log_entry{it->second}, true);
   }
   uint32_t     slice_stride() const noexcept { return bench_slice_stride; }
   uint32_t     slice_number(uint32_t block_num) const noexcept { return block_num / bench_slice_stride; }
   bloom_reader get_bloom(uint32_t) const { return {}; }
};

using bench_request_handler = request_handler<bench_logfile_provider, abi_data_handler::shared_provider>;

// sysio.token::transfer action data, packed as the chain records it.
chain::bytes pack_transfer(chain::name from, chain::name to, const chain::asset& quantity, const std::string& memo) {
   chain::bytes out(fc::raw::pack_size(from) + fc::raw::pack_size(to) + fc::raw::pack_size(quantity) +
                    fc::raw::pack_size(memo));
   fc::datastream<char*> ds(out.data(), out.size());
   fc::raw::pack(ds, from);
   fc::raw::pack(ds, to);
   fc::raw::pack(ds, quantity);
   fc::raw::pack(ds, memo);
   return out;
}

// block_count blocks, each holding transfers_per_block single-transfer transactions.
std::map<uint32_t, block_trace_v0> make_transfer_blocks(uint32_t block_count, uint32_t transfers_per_block) {
   const chain::bytes data = pack_transfer(chain::name{"alice"}, chain::name{"bob"},
                                           chain::asset::from_string("1.0000 SYS"), "benchmark transfer");
   std::map<uint32_t, block_trace_v0> blocks;
   uint64_t seq = 1;
   for (uint32_t b = 1; b <= block_count; ++b) {
      block_trace_v0& bt = blocks[b];
      bt.id        = fc::sha256::hash(std::string{"block-"} + std::to_string(b));
      bt.number    = b;
      bt.timestamp = chain::block_timestamp_type(1'700'000'000u + b);
      bt.producer  = chain::name{"defproducera"};
      bt.transactions.reserve(transfers_per_block);
      for (uint32_t t = 0; t < transfers_per_block; ++t) {
         action_trace_v0 act;
         act.global_sequence = seq++;
         act.receiver        = token_account;
         act.account         = token_account;
         act.action          = transfer_action;
         act.authorization.push_back({chain::name{"alice"}, chain::name{"active"}});
         act.data            = data;

         transaction_trace_v0 trx;
         trx.id                = fc::sha256::hash(std::string{"trx-"} + std::to_string(act.global_sequence));
         trx.actions.push_back(std::move(act));
         trx.cpu_usage_us      = 150;
         trx.net_usage_words   = fc::unsigned_int{16};
         trx.block_num         = b;
         trx.block_time        = bt.timestamp;
         trx.producer_block_id = bt.id;
         bt.transactions.push_back(std::move(trx));
      }
   }
   return blocks;
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

   const auto noop_stream_handler = make_noop_stream_data_handler();
   const bool irreversible = true;

   for (const auto& s : shapes) {
      const auto bt = make_synthetic_block_trace(s.trxs, s.actions_per_trx, s.data_bytes);
      const auto entry = data_log_entry{bt};

      const std::string label_prefix = std::to_string(s.trxs) + " trxs x "
                                     + std::to_string(s.actions_per_trx) + " acts ("
                                     + std::to_string(s.data_bytes) + "B data)";

      // process_block_to_json writes the block's JSON directly into std::string.
      benchmarking("stream:       " + label_prefix, [&]() {
         std::string out = detail::response_formatter::process_block_to_json(entry, irreversible, noop_stream_handler);
         // Defeat the optimizer.
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
      // (matches stream, which also starts from a pre-built entry).
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
         fc::move_only_function<void(fc::json_writer&)> body =
            [e = std::move(e)](fc::json_writer& w) mutable {
               asm volatile("" : : "g"(&e) : "memory");
            };
         auto holder = std::move(body);
         asm volatile("" : : "g"(&holder) : "memory");
      });
   }

   // ---- get_actions: scan + ABI decode + response -------------------------------
   //
   // get_actions_json over sysio.token transfers decoded with the real token ABI: the scan, then each action
   // decoded straight into the json_writer.  10,000 actions is the per-response ceiling (max_actions_per_response).
   {
      const std::vector<char> token_abi = fc::raw::pack(fc::json::from_file<chain::abi_def>(TOKEN_ABI_PATH));
      auto abi_handler = std::make_shared<abi_data_handler>(
         exception_handler{},
         [](chain::name account, uint64_t) -> std::optional<uint64_t> {
            return account == token_account ? std::optional<uint64_t>{0} : std::nullopt;
         },
         [&token_abi](chain::name account, uint64_t) -> std::optional<std::vector<char>> {
            return account == token_account ? std::optional<std::vector<char>>{token_abi} : std::nullopt;
         });

      struct actions_shape { uint32_t blocks; uint32_t transfers_per_block; };
      const actions_shape shapes[] = {
         { 10,  100 },  //  1,000 transfers
         { 100, 100 },  // 10,000 transfers
      };
      for (const auto& s : shapes) {
         bench_request_handler handler(
            bench_logfile_provider{std::make_shared<const std::map<uint32_t, block_trace_v0>>(
               make_transfer_blocks(s.blocks, s.transfers_per_block))},
            abi_data_handler::shared_provider(abi_handler), [](const std::string&) {});

         action_query query;
         query.block_num_start = 1;
         query.block_num_end   = s.blocks;
         query.receiver        = token_account;
         query.account         = token_account;
         query.action          = transfer_action;

         const std::string label = std::to_string(s.blocks * s.transfers_per_block) + " transfers";

         benchmarking("get_actions stream: " + label, [&]() {
            std::string out = handler.get_actions_json(query);
            asm volatile("" : : "g"(out.size()) : "memory");
         });
      }
   }

   // ---- Isolated hex-emission micro-bench ------------------------------------
   //
   // Probes the hex-encoding cost in isolation.  Two paths per byte size:
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
