// Head-to-head microbenchmark: Wire's custom transaction_dedup (boost::unordered_flat_map +
// std::set sorted index + hand-rolled revision/session undo) vs the chainbase transaction
// dedup it replaced (Spring's transaction_multi_index: three ordered B-tree indices in a
// chainbase segment, undone for free by the chainbase undo stack).
//
// Both sides run IDENTICAL, pre-generated workloads. The question this answers: how large is
// the per-transaction performance gain from moving dedup out of chainbase, and is it worth the
// recurring consensus-correctness cost of hand-maintaining a parallel undo/revision/persistence
// system (the bug class found in PR #391 and the 2026-06-11 re-review)?
//
// Build (release):  ninja -C <build> dedup_bench
// Run:              ./benchmark/dedup/dedup_bench [--big] [--tps N] [--blocks N]

#include <sysio/chain/transaction_dedup.hpp>
#include <sysio/chain/multi_index_includes.hpp>

#include <chainbase/chainbase.hpp>

#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/composite_key.hpp>

#include <fc/filesystem.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace sysio::chain;
using clk = std::chrono::steady_clock;

namespace {

double ns_since(clk::time_point t0) {
   return std::chrono::duration<double, std::nano>(clk::now() - t0).count();
}

// Resident set size in bytes, from /proc/self/statm (field 2 = resident pages).
size_t rss_bytes() {
   std::ifstream f("/proc/self/statm");
   size_t total_pages = 0, resident_pages = 0;
   f >> total_pages >> resident_pages;
   return resident_pages * static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

// splitmix64 fills a 32-byte id the way a real (uniformly random) transaction id is distributed
// across both the hash map's buckets and the B-tree's comparison paths.
uint64_t splitmix64(uint64_t& s) {
   uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
   z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
   z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
   return z ^ (z >> 31);
}

transaction_id_type make_id(uint64_t n) {
   transaction_id_type id;
   uint64_t s = n + 1;
   auto* w = reinterpret_cast<uint64_t*>(id.data());
   w[0] = splitmix64(s); w[1] = splitmix64(s); w[2] = splitmix64(s); w[3] = splitmix64(s);
   return id;
}

} // namespace

// ---------------------------------------------------------------------------
// Chainbase dedup: a faithful copy of Spring's transaction_object / transaction_multi_index
// (the exact thing Wire removed in commit dd935a5b0e), driven through real chainbase undo
// sessions exactly as the old controller / transaction_context drove it.
// ---------------------------------------------------------------------------
namespace bench {

constexpr uint16_t bench_transaction_object_type = 1000;

class transaction_object : public chainbase::object<bench_transaction_object_type, transaction_object> {
   OBJECT_CTOR(transaction_object)
   id_type             id;
   fc::time_point_sec  expiration;
   transaction_id_type trx_id;
};

struct by_expiration;
struct by_trx_id;
using transaction_multi_index = chainbase::shared_multi_index_container<
   transaction_object,
   boost::multi_index::indexed_by<
      boost::multi_index::ordered_unique<boost::multi_index::tag<by_id>,
         BOOST_MULTI_INDEX_MEMBER(transaction_object, transaction_object::id_type, id)>,
      boost::multi_index::ordered_unique<boost::multi_index::tag<by_trx_id>,
         BOOST_MULTI_INDEX_MEMBER(transaction_object, transaction_id_type, trx_id)>,
      boost::multi_index::ordered_unique<boost::multi_index::tag<by_expiration>,
         boost::multi_index::composite_key<transaction_object,
            BOOST_MULTI_INDEX_MEMBER(transaction_object, fc::time_point_sec, expiration),
            BOOST_MULTI_INDEX_MEMBER(transaction_object, transaction_object::id_type, id)>>
   >
>;

} // namespace bench

CHAINBASE_SET_INDEX_TYPE(bench::transaction_object, bench::transaction_multi_index)

namespace {

// Wraps a chainbase::database to expose the same surface as transaction_dedup, mapping each
// operation to exactly what the old controller / transaction_context did.
struct chainbase_dedup {
   fc::temp_directory tmp;
   chainbase::database db;

   explicit chainbase_dedup(uint64_t segment_bytes, chainbase::pinnable_mapped_file::map_mode mode)
   : db(tmp.path(), chainbase::database::read_write, segment_bytes, false, mode) {
      db.add_index<bench::transaction_multi_index>();
   }

   void record(const transaction_id_type& id, fc::time_point_sec exp) {
      db.create<bench::transaction_object>([&](auto& t) { t.trx_id = id; t.expiration = exp; });
   }
   bool is_known(const transaction_id_type& id) const {
      return db.find<bench::transaction_object, bench::by_trx_id>(id) != nullptr;
   }
   uint32_t clear_expired(fc::time_point now) {
      auto& idx = db.get_mutable_index<bench::transaction_multi_index>();
      const auto& by_exp = idx.indices().get<bench::by_expiration>();
      uint32_t n = 0;
      while (!by_exp.empty() && now > by_exp.begin()->expiration.to_time_point()) {
         idx.remove(*by_exp.begin());
         ++n;
      }
      return n;
   }
   // Unified undo lifecycle, mirroring transaction_dedup's: a block revision and a transaction
   // session are the same add_undo_session, driven straight through the chainbase undo stack.
   // start_undo_session(true).push() opens a revision and keeps it on the stack (exactly as the
   // controller keeps a block's db session until LIB advances); commit(revision) drops it later.
   void add_undo_session() { db.start_undo_session(true).push(); }
   void squash()           { db.squash(); }
   void undo()             { db.undo(); }
   void commit(int64_t rev){ db.commit(rev); }

   int64_t revision() const { return db.revision(); }
   size_t  free_mem() const { return db.get_free_memory(); }
};

// --- Adapter bridging the one remaining API difference --------------------------------------
// Both sides now present the same chainbase-aligned surface (add_undo_session / squash / undo /
// commit(revision) / revision()); only clear_expired's return type still differs (the custom one
// returns {removed,total}, chainbase a bare count), and the benchmark loop ignores it either way.
uint32_t do_clear_expired(transaction_dedup& d, fc::time_point now) { return d.clear_expired(now).first; }
uint32_t do_clear_expired(chainbase_dedup& d, fc::time_point now)   { return d.clear_expired(now); }

// --- Workload ------------------------------------------------------------

struct params {
   size_t per_block = 2000;        // transactions per block (== TPS at 1s cadence)
   size_t blocks    = 200;         // simulated blocks
   size_t lifetime_blocks = 3600;  // entries live this many blocks before expiring
};

struct workload {
   std::vector<transaction_id_type> ids;
   std::vector<fc::time_point_sec>  exps;
};

// Entry i is born in block i/per_block and expires lifetime_blocks later, so advancing the clock
// one block expires ~per_block of the oldest entries (steady state: inflow == outflow).
workload gen_workload(size_t count, size_t per_block, size_t lifetime_blocks) {
   workload w;
   w.ids.reserve(count);
   w.exps.reserve(count);
   for (size_t i = 0; i < count; ++i) {
      w.ids.push_back(make_id(i));
      uint32_t born_block = static_cast<uint32_t>(i / per_block);
      w.exps.emplace_back(fc::time_point_sec(static_cast<uint32_t>(born_block + lifetime_blocks)));
   }
   return w;
}

template<typename D>
void prefill(D& d, const workload& w, size_t live) {
   for (size_t i = 0; i < live; ++i) d.record(w.ids[i], w.exps[i]);
}

struct phase_times {
   double is_known_hit_ns = 0, is_known_miss_ns = 0;
   double producer_ns = 0, relay_ns = 0;
   size_t lookups = 0, producer_trx = 0, relay_trx = 0;
   size_t mem_per_entry = 0;
};

// Pure is_known cost at steady state: half hits (present ids), half misses (never-recorded ids).
template<typename D>
void bench_is_known(D& d, const workload& w, size_t live, phase_times& pt) {
   constexpr size_t probes = 1'000'000;
   volatile uint64_t sink = 0;
   auto t0 = clk::now();
   for (size_t i = 0; i < probes; ++i) sink = sink + d.is_known(w.ids[(i * 2654435761ull) % live]); // hits
   pt.is_known_hit_ns += ns_since(t0);
   t0 = clk::now();
   for (size_t i = 0; i < probes; ++i) sink = sink + d.is_known(make_id(w.ids.size() + i));          // misses
   pt.is_known_miss_ns += ns_since(t0);
   pt.lookups += probes;
   (void)sink;
}

// One realistic block: clear_expired, then per trx { is_known (dup check); push/record/squash },
// commit, and advance LIB ~5 blocks back. If `relay`, first speculate an entire block of records
// and abort it (the validator/relay worst case: an extra block of work plus a full undo).
template<typename D>
void bench_block_cycle(D& d, const workload& w, size_t start_idx, const params& p, bool relay,
                       phase_times& pt) {
   size_t idx = start_idx;
   uint32_t now_block = static_cast<uint32_t>(start_idx / p.per_block);
   volatile uint64_t sink = 0;
   std::vector<int64_t> committed; // block revisions, for LIB advance

   auto t0 = clk::now();
   for (size_t b = 0; b < p.blocks && idx + 2 * p.per_block < w.ids.size(); ++b) {
      ++now_block;
      if (relay) {
         // Speculative block: open a block session, clear + record a block's worth, then discard
         // the whole block with one undo (the validator/relay worst case: an extra block + a full undo).
         d.add_undo_session();
         do_clear_expired(d, fc::time_point(fc::seconds(now_block)));
         for (size_t k = 0; k < p.per_block; ++k) {
            d.add_undo_session();
            d.record(w.ids[idx + k], w.exps[idx + k]);
            d.squash();
         }
         d.undo();
      }
      // Real block: open a block session; each trx is a nested session squashed into the block.
      // The block session is kept on the stack (no explicit commit) until LIB advances past it.
      d.add_undo_session();
      do_clear_expired(d, fc::time_point(fc::seconds(now_block)));
      for (size_t k = 0; k < p.per_block; ++k, ++idx) {
         sink = sink + d.is_known(w.ids[idx]);
         d.add_undo_session();
         d.record(w.ids[idx], w.exps[idx]);
         d.squash();
      }
      committed.push_back(d.revision());
      if (committed.size() > 5)
         d.commit(committed[committed.size() - 6]);
   }
   double elapsed = ns_since(t0);
   if (relay) { pt.relay_ns += elapsed; pt.relay_trx += (idx - start_idx); }
   else       { pt.producer_ns += elapsed; pt.producer_trx += (idx - start_idx); }
   (void)sink;
}

template<typename D>
void measure(D& d, const workload& w, size_t live, const params& p, phase_times& pt) {
   bench_is_known(d, w, live, pt);
   bench_block_cycle(d, w, live, p, /*relay*/false, pt);
   // Re-fill the consumed indices for an independent relay run starting where producer left off.
   bench_block_cycle(d, w, live + p.blocks * p.per_block, p, /*relay*/true, pt);
}

void run_custom(const params& p, const workload& w, size_t live, phase_times& pt) {
   size_t rss0 = rss_bytes();
   transaction_dedup d;
   prefill(d, w, live);
   pt.mem_per_entry = live ? (rss_bytes() - rss0) / live : 0;
   measure(d, w, live, p, pt);
}

void run_chainbase(const params& p, const workload& w, size_t live, uint64_t segment,
                   chainbase::pinnable_mapped_file::map_mode mode, phase_times& pt) {
   chainbase_dedup d(segment, mode);
   size_t free0 = d.free_mem();
   prefill(d, w, live);
   pt.mem_per_entry = live ? (free0 - d.free_mem()) / live : 0;
   measure(d, w, live, p, pt);
}

void print_row(const char* tag, const phase_times& pt) {
   double hit  = pt.is_known_hit_ns  / pt.lookups;
   double miss = pt.is_known_miss_ns / pt.lookups;
   double prod = pt.producer_trx ? pt.producer_ns / pt.producer_trx : 0;
   double rly  = pt.relay_trx    ? pt.relay_ns    / pt.relay_trx    : 0;
   printf("  %-21s  is_known: hit %6.1f  miss %6.1f |  per-trx: producer %7.1f  relay %7.1f  (ns) | mem %4zu B/entry\n",
          tag, hit, miss, prod, rly, pt.mem_per_entry);
}

} // namespace

int main(int argc, char** argv) {
   params p;
   bool big = false;
   for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--big") big = true;
      else if (a == "--tps"    && i + 1 < argc) p.per_block = std::stoul(argv[++i]);
      else if (a == "--blocks" && i + 1 < argc) p.blocks    = std::stoul(argv[++i]);
   }

   std::vector<size_t> live_sets = big ? std::vector<size_t>{100'000, 1'000'000, 6'000'000}
                                       : std::vector<size_t>{100'000, 1'000'000};

   using mm = chainbase::pinnable_mapped_file::map_mode;
   struct cb_mode { const char* name; mm mode; };
   // heap = anonymous + huge pages (best case); mapped = MAP_SHARED file-backed (production
   // default, real writeback); mapped_private = MAP_PRIVATE COW (file written only at exit).
   std::vector<cb_mode> modes = { {"chainbase/heap", mm::heap},
                                  {"chainbase/mapped", mm::mapped},
                                  {"chainbase/mapped_priv", mm::mapped_private} };

   printf("transaction dedup head-to-head: custom (flat_map + sorted set) vs chainbase (3 ordered B-trees)\n");
   printf("per_block=%zu  blocks=%zu  lifetime=%zu blocks   (producer = no fork; relay = speculate+abort+apply)\n\n",
          p.per_block, p.blocks, p.lifetime_blocks);

   for (size_t live : live_sets) {
      // Enough ids for prefill + a producer run + a relay run (relay records 2x per block).
      size_t total = live + 2 * (p.blocks + 2) * p.per_block + p.blocks * p.per_block;
      workload w = gen_workload(total, p.per_block, p.lifetime_blocks);
      // chainbase segment must be a multiple of 1 MiB; size for ~512 B/entry plus 1 GiB headroom.
      uint64_t segment = static_cast<uint64_t>(total) * 512 + (1ull << 30);
      constexpr uint64_t MiB = 1ull << 20;
      segment = (segment + MiB - 1) & ~(MiB - 1);

      printf("live set = %zu entries:\n", live);
      phase_times cpt;
      run_custom(p, w, live, cpt);
      print_row("custom", cpt);
      for (const auto& m : modes) {
         phase_times hpt;
         run_chainbase(p, w, live, segment, m.mode, hpt);
         print_row(m.name, hpt);
      }
      printf("\n");
   }
   return 0;
}
