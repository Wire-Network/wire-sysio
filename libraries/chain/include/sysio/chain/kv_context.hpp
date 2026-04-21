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

// ------------------------------------------------------------------------------------------------------------
// Handle encoding (CONSENSUS-OBSERVABLE)
//
// Handle values returned from the KV iterator intrinsics are visible to contracts as the intrinsic's return
// value.  Contracts may read, store, and branch on those values, so the encoding below is part of the consensus
// surface: every bit that is set today and every bit we promise to leave zero today must remain that way unless
// a future protocol feature deliberately redefines them.
//
// Layout (bits numbered from LSB):
//
//   [ 0.. 9]  slot index (10 bits, covers max_kv_iterators = 1024)
//   [10..15]  RESERVED — must be zero
//   [16    ]  secondary-pool tag (1 = secondary, 0 = primary)
//   [17..30]  RESERVED — must be zero
//   [31    ]  MUST be zero — keeps the handle positive when read as int32_t (intrinsics return int32_t;
//             negative means "not found").
//
// Bit 16 was chosen over a high bit to keep handle values small and human-readable in logs and error messages.
// The reserved bits give future protocol features room to encode things like iterator generation counters (to
// catch stale-handle reuse across destroy/re-allocate) or additional pool discriminators, without disturbing
// already-deployed contract code.
//
// Any change to this encoding — including the tag bit position, the slot mask width, or the interpretation of
// any reserved bit — is a protocol change.  Contracts MUST NOT rely on reserved bits being anything other than
// zero; kv_handle_check_reserved_zero enforces the invariant at the host-intrinsic boundary so any accidental
// non-zero reserved bit is caught before it becomes load-bearing.
// ------------------------------------------------------------------------------------------------------------

// Primary pool capacity. Matches config::max_kv_iterators.
constexpr uint32_t kv_handle_slot_mask     = 0x000003FFu;  // bits  0.. 9: slot index in [0, 1024)
constexpr uint32_t kv_secondary_handle_tag = 0x00010000u;  // bit  16    : secondary-pool tag

// Everything that is neither slot index nor tag must be zero.  Negation is cast to uint32_t so the reserved
// mask stays exactly 32 bits regardless of integral promotion to int on the host.
constexpr uint32_t kv_handle_reserved_mask =
   static_cast<uint32_t>(~(kv_handle_slot_mask | kv_secondary_handle_tag));

static_assert(config::max_kv_iterators <= kv_handle_slot_mask + 1,
              "kv_handle_slot_mask is too narrow for config::max_kv_iterators");
static_assert((kv_handle_slot_mask & kv_secondary_handle_tag) == 0,
              "kv_handle_slot_mask and kv_secondary_handle_tag must not overlap");

inline bool kv_handle_is_secondary(uint32_t handle) {
   return (handle & kv_secondary_handle_tag) != 0;
}

inline uint32_t kv_handle_slot_index(uint32_t handle) {
   return handle & kv_handle_slot_mask;
}

inline uint32_t kv_make_secondary_handle(uint32_t slot_index) {
   return slot_index | kv_secondary_handle_tag;
}

// Enforce the reserved-bits-zero invariant at the host-intrinsic boundary.  Any non-zero bit in
// kv_handle_reserved_mask means the contract has been fabricating handle values rather than using one returned
// from the host, or has relied on an undocumented bit pattern.  Rejecting these early keeps the reserved bits
// genuinely reserved — available for future protocol features.
inline void kv_handle_check_reserved_zero(uint32_t handle) {
   SYS_ASSERT((handle & kv_handle_reserved_mask) == 0, kv_invalid_iterator,
              "KV iterator handle has non-zero reserved bits (handle={:#x})",
              handle);
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
   // Lazy cache of primary-key bytes, populated only when kv_idx_primary_key materializes them from the
   // referenced kv_object.  Empty otherwise so iteration does not pay the by_id materialization cost.
   std::vector<char>  current_pri_key;

   // id of the kv_object referenced by the current secondary row, used for pri_key/value materialization and
   // as the composite-key tiebreaker for re-seeking after invalidation.  -1 when not at a valid secondary
   // position.
   // Invariant: when status == iterator_ok, primary_id >= 0.  The slow-path re-seek in kv_idx_next/kv_idx_prev
   // asserts this to catch an internally-inconsistent slot rather than synthesize garbage.
   int64_t            primary_id = -1;

   void reset() {
      status = kv_it_stat::iterator_end;
      cached_id = -1;
      current_sec_key.clear();
      current_pri_key.clear();
      primary_id = -1;
   }
};

// Pool for primary (kv_it_*) iterators.  Slot type is tight (~72B): no secondary-only fields, so cache
// locality on hot iteration paths improves over the pre-split union slot.
//
// The slot array is lazily sized on first allocate() so actions that touch no KV iterators pay zero heap for
// this pool.  Once sized, the pool behaves identically to an eagerly-allocated one.
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

// Pool for secondary (kv_idx_*) iterators.  Independent free-list from the primary pool so a contract may
// hold up to max_kv_iterators of each type simultaneously.  Lazily sized on first allocate(), same as the
// primary pool.
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
