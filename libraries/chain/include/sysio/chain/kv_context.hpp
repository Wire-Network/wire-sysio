#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <vector>
#include <cstring>
#include <cstdint>

namespace sysio { namespace chain {

enum class kv_it_stat : int32_t {
   iterator_ok     = 0,
   iterator_end    = 1,
   iterator_erased = 2
};

// ---------------------------------------------------------------------------
// Handle encoding (CONSENSUS-OBSERVABLE)
//
// Iterator handles are returned to contracts from kv_it_create /
// kv_idx_find_secondary / kv_idx_lower_bound and accepted back by every
// kv_it_* / kv_idx_* entry point.  Contracts may read, store, and branch on
// these values, so the layout is part of the consensus surface.
//
//   [ 0.. 9]  slot index (10 bits, covers config::max_kv_iterators = 1024)
//   [10..15]  RESERVED, must be zero
//   [16    ]  secondary-pool tag (1 = secondary, 0 = primary)
//   [17..30]  RESERVED, must be zero
//   [31    ]  always zero - keeps handles positive when read as int32_t
//             (negative returns are reserved for "not found" sentinels).
//
// Reserved bits give future protocol changes room to encode e.g. iterator
// generation counters without disturbing deployed contract code.  A contract
// that fabricates a handle by setting reserved bits fails with a clean
// kv_invalid_iterator instead of silently aliasing a real slot through the
// truncated slot-index mask.  Any change to this layout is a protocol change.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kv_secondary_handle_tag   = 0x00010000u;
inline constexpr uint32_t kv_handle_slot_index_mask = 0x000003FFu; // bits 0..9
inline constexpr uint32_t kv_handle_reserved_mask   =
   ~(kv_handle_slot_index_mask | kv_secondary_handle_tag);

inline bool kv_handle_is_secondary(uint32_t handle) {
   return (handle & kv_secondary_handle_tag) != 0;
}

inline uint32_t kv_handle_slot_index(uint32_t handle) {
   return handle & kv_handle_slot_index_mask;
}

inline uint32_t kv_make_secondary_handle(uint32_t slot_index) {
   return slot_index | kv_secondary_handle_tag;
}

// Throws kv_invalid_iterator if any reserved bit is set.  Called from every
// host-intrinsic entry point that consumes a handle so a fabricated handle
// aborts the action instead of silently aliasing a real slot.
inline void kv_handle_check_reserved_zero(uint32_t handle) {
   SYS_ASSERT((handle & kv_handle_reserved_mask) == 0, kv_invalid_iterator,
              "KV iterator handle has reserved bits set: {}", handle);
}

// ---------------------------------------------------------------------------
// Slot types
//
// Common fields are shared via inheritance; primary- and secondary-only fields
// live on the respective subclass so cache lines on the hot iterator paths
// carry only the data the operation needs.
// ---------------------------------------------------------------------------

struct kv_iterator_slot_common {
   bool              in_use = false;
   kv_it_stat        status = kv_it_stat::iterator_end;
   account_name      code;
   uint16_t          table_id = 0;
   // Cached chainbase id for the iterator_to fast path.  -1 means no cached
   // id; falls back to lower_bound re-seek via the saved key bytes.
   int64_t           cached_id = -1;
};

struct kv_primary_slot : kv_iterator_slot_common {
   std::vector<char>  prefix;       ///< prefix for bounded iteration
   std::vector<char>  current_key;  ///< current position key bytes (for re-seek)

   void reset() {
      status    = kv_it_stat::iterator_end;
      cached_id = -1;
      prefix.clear();
      current_key.clear();
   }
};

struct kv_secondary_slot : kv_iterator_slot_common {
   std::vector<char>  current_sec_key;  ///< current secondary key bytes
   std::vector<char>  current_pri_key;  ///< current primary key bytes

   void reset() {
      status    = kv_it_stat::iterator_end;
      cached_id = -1;
      current_sec_key.clear();
      current_pri_key.clear();
   }
};

// ---------------------------------------------------------------------------
// Primary iterator pool (kv_it_*)
//
// The slot array is lazily sized on first allocate() so actions that touch no
// KV iterators (e.g. sysio.token transfer, which routes through kv_get and
// kv_set only) pay zero heap for this pool.  Once sized, the pool behaves
// identically to an eagerly-allocated one.
// ---------------------------------------------------------------------------
class kv_primary_iterator_pool {
public:
   kv_primary_iterator_pool() = default;

   uint32_t allocate(uint16_t table_id, account_name code,
                     const char* prefix, uint32_t prefix_size) {
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

   void release(uint32_t slot_index) {
      SYS_ASSERT(slot_index < _slots.size() && _slots[slot_index].in_use,
                 kv_invalid_iterator, "invalid KV primary iterator slot {}", slot_index);
      auto& s = _slots[slot_index];
      s.reset();
      s.in_use = false;
      if (slot_index < _next_free) _next_free = slot_index;
   }

   kv_primary_slot& get(uint32_t slot_index) {
      SYS_ASSERT(slot_index < _slots.size() && _slots[slot_index].in_use,
                 kv_invalid_iterator, "invalid KV primary iterator slot {}", slot_index);
      return _slots[slot_index];
   }

   const kv_primary_slot& get(uint32_t slot_index) const {
      SYS_ASSERT(slot_index < _slots.size() && _slots[slot_index].in_use,
                 kv_invalid_iterator, "invalid KV primary iterator slot {}", slot_index);
      return _slots[slot_index];
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
                "exceeded maximum number of KV primary iterators ({})",
                config::max_kv_iterators);
   }

   std::vector<kv_primary_slot> _slots;
   uint32_t _next_free = 0;
};

// ---------------------------------------------------------------------------
// Secondary iterator pool (kv_idx_*)
//
// Independent free-list from the primary pool so a contract may hold up to
// max_kv_iterators of each kind simultaneously.  Lazily sized on first
// allocate(), same as the primary pool.
// ---------------------------------------------------------------------------
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

   void release(uint32_t slot_index) {
      SYS_ASSERT(slot_index < _slots.size() && _slots[slot_index].in_use,
                 kv_invalid_iterator, "invalid KV secondary iterator slot {}", slot_index);
      auto& s = _slots[slot_index];
      s.reset();
      s.in_use = false;
      if (slot_index < _next_free) _next_free = slot_index;
   }

   kv_secondary_slot& get(uint32_t slot_index) {
      SYS_ASSERT(slot_index < _slots.size() && _slots[slot_index].in_use,
                 kv_invalid_iterator, "invalid KV secondary iterator slot {}", slot_index);
      return _slots[slot_index];
   }

   const kv_secondary_slot& get(uint32_t slot_index) const {
      SYS_ASSERT(slot_index < _slots.size() && _slots[slot_index].in_use,
                 kv_invalid_iterator, "invalid KV secondary iterator slot {}", slot_index);
      return _slots[slot_index];
   }

   // Clears cached_id on any slot referencing the given kv_index_object id.
   // Preserves stored key bytes and iterator status so the next op uses the
   // slow re-seek path from the old position.  Used before db.modify of a
   // secondary entry where the chainbase id survives but the object's sort
   // position moves.
   void invalidate_cache(account_name code, uint16_t table_id, int64_t object_id) {
      for (auto& s : _slots) {
         if (s.in_use &&
             s.code == code && s.table_id == table_id &&
             s.cached_id == object_id) {
            s.cached_id = -1;
         }
      }
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
                "exceeded maximum number of KV secondary iterators ({})",
                config::max_kv_iterators);
   }

   std::vector<kv_secondary_slot> _slots;
   uint32_t _next_free = 0;
};

} } // namespace sysio::chain
