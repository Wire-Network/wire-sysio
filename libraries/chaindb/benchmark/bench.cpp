#include <chainbase/undo_index.hpp>
#include <chainbase/chainbase.hpp>
#include <filesystem>
#include <iostream>
#include <chrono>

#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>

namespace bip = boost::interprocess;
namespace fs  = std::filesystem;
namespace bmi = boost::multi_index;

template<typename T>
using test_allocator_base = chainbase::chainbase_node_allocator<T, chainbase::segment_manager>;

template<typename T>
class test_allocator : public test_allocator_base<T> {
public:
   using base = test_allocator_base<T>;
   test_allocator(chainbase::segment_manager *mgr) : base(mgr) {}
   template<typename U>   test_allocator(const test_allocator<U>& o) : base(o.get_segment_manager()) {}
   template<typename U>   struct rebind { using other = test_allocator<U>; };
   typename base::pointer allocate(std::size_t count) {
      return base::allocate(count);
   }
};

using shared_string = chainbase::shared_string;

// undo_index's 42-bit packed offsets between a node and the index's header limit
// the addressable separation to ~8 TiB. Allocating the index on the stack while
// nodes live in a kernel-chosen mmap region can exceed that range and produce
// truncated/bogus parent offsets that SIGSEGV during tree rebalancing.
// Construct the index inside the segment so the header sits next to the nodes.
template <typename... T>
struct undo_index_in_segment {
   using undo_index_t = chainbase::undo_index<T...>;

   template <typename A>
   undo_index_in_segment(A& alloc) : segment_manager(*alloc.get_segment_manager()) {
      p = segment_manager.template construct<undo_index_t>("")(alloc);
   }

   ~undo_index_in_segment() {
      if (p) segment_manager.destroy_ptr(p);
   }

   undo_index_t* operator->() { return p; }
   undo_index_t& operator*()  { return *p; }

   chainbase::segment_manager& segment_manager;
   undo_index_t* p = nullptr;

   undo_index_in_segment(const undo_index_in_segment&) = delete;
   undo_index_in_segment& operator=(const undo_index_in_segment&) = delete;
};

struct elem_t {
   template<typename C, typename A>
   elem_t(C&& c, A&&) {
      c(*this);
   }

   friend std::ostream& operator<<(std::ostream& os, const elem_t& e) {
      os  << '[' << e.id << ", " << e.val << ']';
      return os;
   }

   uint64_t id;
   uint64_t val;
   shared_string str;
};

// Two-index element. id is primary, name is a secondary indexed key.
// Modifies touch only `val` (non-indexed) so post_modify walk is pure overhead.
struct elem2_t {
   template<typename C, typename A>
   elem2_t(C&& c, A&&) { c(*this); }

   uint64_t id;
   uint64_t name;   // indexed but never mutated
   uint64_t val;    // not indexed, bumped on every modify
};

struct by_id {};
struct by_name {};

template<typename time_unit = std::milli>
struct stopwatch {
   stopwatch(const char* label) : _label(label) { _start = clock::now(); }
   ~stopwatch() {
      using duration_t = std::chrono::duration<float, time_unit>;
      point end = clock::now();
      float elapsed = std::chrono::duration_cast<duration_t>(end - _start).count();
      printf("%-55s %10.2f s\n", _label, elapsed / 1000);
      fflush(stdout);
   }
   using clock = std::chrono::high_resolution_clock;
   using point = std::chrono::time_point<clock>;
   point _start;
   const char* _label;
};

template<typename T>
struct key_impl;
template<typename C, typename T>
struct key_impl<T C::*> { template<auto F> using fn = boost::multi_index::member<C, T, F>; };

template<auto Fn>
using key = typename key_impl<decltype(Fn)>::template fn<Fn>;

// ---- Scenario 1: original mixed find/modify/emplace/remove, single index, no sessions.
static void scenario_original(chainbase::segment_manager* mgr) {
   constexpr size_t num_elems = 32 * 1024 * 1024;
   test_allocator<elem_t> alloc(mgr);
   undo_index_in_segment<elem_t, test_allocator<elem_t>,
      bmi::ordered_unique<key<&elem_t::id>>> i0(alloc);
   boost::random::mt19937 gen;
   boost::random::uniform_int_distribution<> dist(1, num_elems);

   stopwatch sw("scenario_original (1 idx, no session, 32M iters)");
   for (size_t i=0; i<num_elems; ++i) {
      size_t id = dist(gen);
      const elem_t* e = i0->find(id);
      if (e) {
         i0->modify(*e, [old=e](elem_t& e) { e.val = old->val + 1; });
      } else {
         auto& e = i0->emplace([](elem_t& e) {
            e.val = 0;
            e.str = "a string";
         });
         if (e.id % 5 == 0)
            i0->remove(e);
      }
   }
}

// ---- Scenario 2: 2 indexes, modifies only non-indexed `val`.
// Every modify runs post_modify across both indexes even though no key changes.
static void scenario_two_idx_nonkey_modify(chainbase::segment_manager* mgr) {
   constexpr size_t num_inserts = 2 * 1024 * 1024;
   constexpr size_t num_modifies = 16 * 1024 * 1024;

   test_allocator<elem2_t> alloc(mgr);
   undo_index_in_segment<elem2_t, test_allocator<elem2_t>,
      bmi::ordered_unique<bmi::tag<by_id>,   key<&elem2_t::id>>,
      bmi::ordered_unique<bmi::tag<by_name>, key<&elem2_t::name>>> idx(alloc);

   for (size_t i = 0; i < num_inserts; ++i) {
      idx->emplace([&](elem2_t& e) { e.name = i * 2654435761ULL; e.val = 0; });
   }

   boost::random::mt19937 gen(42);
   boost::random::uniform_int_distribution<uint64_t> dist(0, num_inserts - 1);

   stopwatch sw("scenario_two_idx_nonkey_modify (2 idx, 16M modifies)");
   for (size_t i = 0; i < num_modifies; ++i) {
      const elem2_t* e = idx->find(dist(gen));
      if (!e) continue;
      idx->modify(*e, [](elem2_t& o) { ++o.val; });
   }
}

// ---- Scenario 3: single index, many undo sessions starting/squashing.
// Exercises _undo_stack push/pop pattern (targets deque -> vector change).
static void scenario_undo_session_churn(chainbase::segment_manager* mgr) {
   constexpr size_t num_inserts = 256 * 1024;
   constexpr size_t num_sessions = 2 * 1024 * 1024;
   constexpr size_t modifies_per_session = 4;

   test_allocator<elem_t> alloc(mgr);
   undo_index_in_segment<elem_t, test_allocator<elem_t>,
      bmi::ordered_unique<key<&elem_t::id>>> idx(alloc);

   for (size_t i = 0; i < num_inserts; ++i) {
      idx->emplace([&](elem_t& e) { e.val = 0; });
   }

   boost::random::mt19937 gen(7);
   boost::random::uniform_int_distribution<uint64_t> dist(0, num_inserts - 1);

   stopwatch sw("scenario_undo_session_churn (2M sessions, squash)");
   for (size_t s = 0; s < num_sessions; ++s) {
      auto session = idx->start_undo_session(true);
      for (size_t m = 0; m < modifies_per_session; ++m) {
         const elem_t* e = idx->find(dist(gen));
         if (e) idx->modify(*e, [](elem_t& o) { ++o.val; });
      }
      session.squash();  // squash is the common hot path during apply_block
   }
}

int main(int argc, char** argv) {
   // Allow selecting a single scenario by name (for stable per-scenario runs).
   auto want = [&](const char* name) {
      if (argc < 2) return true;
      for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == name) return true;
      return false;
   };

   fs::path temp = fs::temp_directory_path() / "pinnable_mapped_file";
   try {
      constexpr size_t db_size = 8ull * 1024 * 1024 * 1024; // 8 GiB
      chainbase::pinnable_mapped_file db(temp, true, db_size, false,
                                         chainbase::pinnable_mapped_file::map_mode::heap);

      if (want("original"))
         scenario_original(db.get_segment_manager());
      if (want("two_idx"))
         scenario_two_idx_nonkey_modify(db.get_segment_manager());
      if (want("undo"))
         scenario_undo_session_churn(db.get_segment_manager());
   } catch (...) {
      fs::remove_all(temp);
      throw;
   }
   fs::remove_all(temp);
   return 0;
}
