#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <vector>
#include <cstring>

namespace sysio { namespace chain {

enum class kv_it_stat : int32_t {
   iterator_ok     = 0,
   iterator_end    = 1,
   iterator_erased = 2
};

// ---------------------------------------------------------------------------
// Handle encoding
//
// Primary and secondary iterators live in separate pools with their own slot
// arrays.  A single handle space must still fit in int32_t because the host
// intrinsics expose handles through that type (with negative values reserved
// for "not found" returns).  Bit 30 tags secondary handles; bit 31 stays
// clear so every encoded handle is positive when observed as int32_t.
//
// config::max_kv_iterators is 1024 (well under 2^30), so the raw slot index
// always fits in the lower 30 bits.
// ---------------------------------------------------------------------------
constexpr uint32_t kv_secondary_handle_tag = 0x40000000u;

inline bool kv_handle_is_secondary(uint32_t handle) {
   return (handle & kv_secondary_handle_tag) != 0;
}

inline uint32_t kv_handle_slot_index(uint32_t handle) {
   return handle & ~kv_secondary_handle_tag;
}

inline uint32_t kv_make_secondary_handle(uint32_t slot_index) {
   return slot_index | kv_secondary_handle_tag;
}

// Fields every slot carries regardless of direction.
struct kv_iterator_slot_common {
   bool              in_use = false;
   kv_it_stat        status = kv_it_stat::iterator_end;
   account_name      code;
   uint16_t          table_id = 0;  ///< table namespace (primary or secondary index)

   // Cached chainbase ID for O(1) iterator_to fast path.
   // -1 means no cached ID; falls back to lower_bound re-seek.
   //   - primary iterator: kv_object id
   //   - secondary iterator: kv_index_object id
   int64_t           cached_id = -1;
};

struct kv_primary_slot : kv_iterator_slot_common {
   std::vector<char>  prefix;       ///< prefix for bounded iteration
   std::vector<char>  current_key;  ///< current position key bytes (for re-seek after invalidation)

   void reset() {
      status = kv_it_stat::iterator_end;
      cached_id = -1;
      prefix.clear();
      current_key.clear();
   }
};

struct kv_secondary_slot : kv_iterator_slot_common {
   std::vector<char>  current_sec_key;  ///< current secondary key bytes (for re-seek)
   // Lazy cache of primary-key bytes, populated only when kv_idx_primary_key
   // materializes them from the referenced kv_object.  Empty otherwise so
   // iteration does not pay the by_id materialization cost.
   std::vector<char>  current_pri_key;

   // id of the kv_object referenced by the current secondary row, used for
   // pri_key/value materialization and as the composite-key tiebreaker for
   // re-seeking after invalidation.  -1 when not at a valid secondary
   // position.
   // Invariant: when status == iterator_ok, primary_id >= 0.  The slow-path
   // re-seek in kv_idx_next/kv_idx_prev asserts this to catch an
   // internally-inconsistent slot rather than synthesize garbage.
   int64_t            primary_id = -1;

   void reset() {
      status = kv_it_stat::iterator_end;
      cached_id = -1;
      current_sec_key.clear();
      current_pri_key.clear();
      primary_id = -1;
   }
};

// Pool for primary (kv_it_*) iterators.  Slot type is tight (~72B): no
// secondary-only fields, so cache locality on hot iteration paths improves
// over the pre-split union slot.
//
// The slot array is lazily sized on first allocate() so actions that touch
// no KV iterators pay zero heap for this pool.  Once sized, the pool
// behaves identically to an eagerly-allocated one.
class kv_primary_iterator_pool {
public:
   kv_primary_iterator_pool() = default;

   uint32_t allocate(uint16_t table_id, account_name code, const char* prefix, uint32_t prefix_size) {
      if (_slots.empty()) _slots.resize(config::max_kv_iterators);
      uint32_t idx = find_free();
      auto& s = _slots[idx];
      s.reset();
      s.in_use   = true;
      s.code     = code;
      s.table_id = table_id;
      s.prefix.assign(prefix, prefix + prefix_size);
      return idx;
   }

   void release(uint32_t handle) {
      SYS_ASSERT(handle < _slots.size() && _slots[handle].in_use,
         kv_invalid_iterator, "invalid KV iterator handle {}", handle);
      auto& s = _slots[handle];
      s.reset();
      s.in_use = false;
      if (handle < _next_free) _next_free = handle;
   }

   kv_primary_slot& get(uint32_t handle) {
      SYS_ASSERT(handle < _slots.size() && _slots[handle].in_use,
         kv_invalid_iterator, "invalid KV iterator handle {}", handle);
      return _slots[handle];
   }

   const kv_primary_slot& get(uint32_t handle) const {
      SYS_ASSERT(handle < _slots.size() && _slots[handle].in_use,
         kv_invalid_iterator, "invalid KV iterator handle {}", handle);
      return _slots[handle];
   }

private:
   uint32_t find_free() {
      for (uint32_t i = _next_free; i < _slots.size(); ++i) {
         if (!_slots[i].in_use) {
            _next_free = i + 1;
            return i;
         }
      }
      SYS_THROW(kv_iterator_limit_exceeded,
         "exceeded maximum number of KV primary iterators ({})", config::max_kv_iterators);
   }

   std::vector<kv_primary_slot> _slots;
   uint32_t _next_free = 0;
};

// Pool for secondary (kv_idx_*) iterators.  Independent free-list from the
// primary pool so a contract may hold up to max_kv_iterators of each type
// simultaneously.  Lazily sized on first allocate(), same as the primary
// pool.
class kv_secondary_iterator_pool {
public:
   kv_secondary_iterator_pool() = default;

   uint32_t allocate(account_name code, uint16_t table_id) {
      if (_slots.empty()) _slots.resize(config::max_kv_iterators);
      uint32_t idx = find_free();
      auto& s = _slots[idx];
      s.reset();
      s.in_use   = true;
      s.code     = code;
      s.table_id = table_id;
      return idx;
   }

   void release(uint32_t handle) {
      SYS_ASSERT(handle < _slots.size() && _slots[handle].in_use,
         kv_invalid_iterator, "invalid KV iterator handle {}", handle);
      auto& s = _slots[handle];
      s.reset();
      s.in_use = false;
      if (handle < _next_free) _next_free = handle;
   }

   kv_secondary_slot& get(uint32_t handle) {
      SYS_ASSERT(handle < _slots.size() && _slots[handle].in_use,
         kv_invalid_iterator, "invalid KV iterator handle {}", handle);
      return _slots[handle];
   }

   const kv_secondary_slot& get(uint32_t handle) const {
      SYS_ASSERT(handle < _slots.size() && _slots[handle].in_use,
         kv_invalid_iterator, "invalid KV iterator handle {}", handle);
      return _slots[handle];
   }

private:
   uint32_t find_free() {
      for (uint32_t i = _next_free; i < _slots.size(); ++i) {
         if (!_slots[i].in_use) {
            _next_free = i + 1;
            return i;
         }
      }
      SYS_THROW(kv_iterator_limit_exceeded,
         "exceeded maximum number of KV secondary iterators ({})", config::max_kv_iterators);
   }

   std::vector<kv_secondary_slot> _slots;
   uint32_t _next_free = 0;
};

} } // namespace sysio::chain
