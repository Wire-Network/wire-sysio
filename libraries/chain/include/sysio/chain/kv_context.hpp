#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/kv_store.hpp>
#include <boost/container/small_vector.hpp>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string_view>

namespace sysio { namespace chain {

/// Result of a KV call. Contracts branch on these values, so they are part of the consensus surface.
enum class kv_status : int32_t {
   ok     = 0, ///< the write happened, or the iterator is on a row
   end    = 1, ///< the iterator is past the last row in its prefix
   no_row = 2, ///< no row is stored at the key
   exists = 3  ///< a row is already stored at the key
};

/// What kv_get and kv_idx_find return when no row matches. No value size or handle is negative.
inline constexpr int32_t kv_miss = -1;

/// The `mode` argument of kv_set. Contracts pass these values, so they are part of the consensus surface.
enum class kv_set_mode : uint32_t {
   upsert = 0, ///< create the row, or replace the one stored at the key
   insert = 1, ///< create the row; kv_status::exists if the key is taken
   update = 2  ///< replace the row; kv_status::no_row if the key is free
};

// ---------------------------------------------------------------------------
// Handle encoding (CONSENSUS-OBSERVABLE)
//
// Iterator handles are returned to contracts from kv_it_create / kv_idx_create /
// kv_idx_find and accepted back by every kv_it_* / kv_idx_* entry point.  Contracts may read, store, and branch on
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
inline constexpr uint32_t kv_handle_reserved_mask   = ~(kv_handle_slot_index_mask | kv_secondary_handle_tag);

// Compile-time invariant: slot indices must fit in the handle's slot bits. Bumping max_kv_iterators past the mask
// width would silently truncate high-slot handles and dispatch them to the wrong slot.
static_assert(config::max_kv_iterators <= kv_handle_slot_index_mask + 1,
              "kv_handle_slot_index_mask too narrow for config::max_kv_iterators");

inline bool kv_handle_is_secondary(uint32_t handle) { return (handle & kv_secondary_handle_tag) != 0; }
inline uint32_t kv_handle_slot_index(uint32_t handle) { return handle & kv_handle_slot_index_mask; }
inline uint32_t kv_make_secondary_handle(uint32_t slot_index) { return slot_index | kv_secondary_handle_tag; }

// Throws kv_invalid_iterator if any reserved bit is set. Called at every host-intrinsic entry point that consumes
// a handle so a fabricated handle aborts the action instead of silently aliasing a real slot.
inline void kv_handle_check_reserved_zero(uint32_t handle) {
   SYS_ASSERT((handle & kv_handle_reserved_mask) == 0, kv_invalid_iterator,
              "KV iterator handle has reserved bits set: {}", handle);
}

// Validate handle and return its slot index; throws on fabricated or wrong-pool handles.
inline uint32_t validate_primary_handle(uint32_t handle, const char* op) {
   kv_handle_check_reserved_zero(handle);
   SYS_ASSERT(!kv_handle_is_secondary(handle), kv_invalid_iterator,
              "{} called on secondary iterator handle", op);
   return kv_handle_slot_index(handle);
}
inline uint32_t validate_secondary_handle(uint32_t handle, const char* op) {
   kv_handle_check_reserved_zero(handle);
   SYS_ASSERT(kv_handle_is_secondary(handle), kv_invalid_iterator,
              "{} called on primary iterator handle", op);
   return kv_handle_slot_index(handle);
}

// Bound iterator prefix size. Prefix bytes are copied into the slot; without a cap a contract could allocate arbitrary
// host-side storage per iterator. Uses the absolute key-size ceiling (not the dynamic max_kv_key_size).
inline void kv_check_prefix_size(uint32_t prefix_size) {
   SYS_ASSERT(prefix_size <= config::max_kv_key_size_limit, kv_key_too_large,
              "KV iterator prefix size {} exceeds limit {}", prefix_size, config::max_kv_key_size_limit);
}

/// Key bytes a slot holds without touching the heap: room for a checksum256 secondary key or a scope and two uint64s.
inline constexpr size_t kv_inline_key_bytes = 32;

/// Slots in a pool after its first allocation. Most actions hold only a few iterators open at once.
inline constexpr uint32_t kv_initial_iterator_slots = 8;

using kv_key_buffer = boost::container::small_vector<char, kv_inline_key_bytes>;

inline std::string_view to_sv(const kv_key_buffer& bytes) {
   return std::string_view(bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// Slot types
//
// Common fields are shared via inheritance; primary- and secondary-only fields
// live on the respective subclass so cache lines on the hot iterator paths
// carry only the data the operation needs.
// ---------------------------------------------------------------------------

template<typename Row>
struct kv_iterator_slot_common {
   using row_type = Row;

   bool              in_use = false;
   bool              at_end = true; ///< past the last row in the prefix; otherwise positioned on the saved key bytes
   // Whether `cursor` is on the row stored at the saved key, so a step needs no search. If not, a read or step re-seeks
   // by the key bytes. kv_erase, kv_idx_erase and a kv_idx_update that changes the key clear this through
   // kv_iterator_pool::forget_row. Only those calls erase or move a row while a slot is open: handles last for one
   // receiver's execution and nothing else writes these tables during it.
   bool              on_row = false;
   account_name      code;
   uint16_t          table_id = 0;
   std::unique_ptr<kv_cursor<Row>> cursor; ///< opened on the slot's first use and kept when the slot is reused
   kv_key_buffer     prefix; ///< bounds the iteration; applies to the key, or to the secondary key of an index entry
};

struct kv_primary_slot : kv_iterator_slot_common<kv_row> {
   static constexpr std::string_view pool_name = "primary";

   kv_key_buffer  current_key;  ///< current position key bytes (for re-seek)

   void reset() {
      at_end = true;
      on_row = false;
      prefix.clear();
      current_key.clear();
   }

   /// Whether the row at this slot's saved position is `row`.
   bool positioned_at(const kv_row& row) const {
      return code == row.code && table_id == row.table_id && to_sv(current_key) == row.key;
   }
};

struct kv_secondary_slot : kv_iterator_slot_common<kv_index_entry> {
   static constexpr std::string_view pool_name = "secondary";

   kv_key_buffer  current_sec_key;  ///< current secondary key bytes
   kv_key_buffer  current_pri_key;  ///< current primary key bytes

   void reset() {
      at_end = true;
      on_row = false;
      prefix.clear();
      current_sec_key.clear();
      current_pri_key.clear();
   }

   /// Whether the entry at this slot's saved position is `entry`.
   bool positioned_at(const kv_index_entry& entry) const {
      return code == entry.code && table_id == entry.table_id && to_sv(current_sec_key) == entry.sec_key &&
             to_sv(current_pri_key) == entry.pri_key;
   }
};

/**
 * @brief Where a KV read copies a row, and the sizes it reports back.
 *
 * `first` receives the key (primary rows) or secondary key (index entries). `second` receives the value or primary key.
 * A zero-size buffer skips that copy and a short buffer truncates it. Either way, a read that finds a row reports the
 * field's full size, so a contract can size a buffer from it. A call that finds no row leaves both sizes at 0.
 */
struct kv_row_buffers {
   char*    first           = nullptr;
   uint32_t first_capacity  = 0;
   char*    second          = nullptr;
   uint32_t second_capacity = 0;
   uint32_t first_size      = 0; ///< [out] full size of the first field
   uint32_t second_size     = 0; ///< [out] full size of the second field
};

/// Copies `field` from `offset` into `dest`, up to `dest_size` bytes. An offset at or past the end copies nothing.
inline void kv_copy_field(std::string_view field, uint32_t offset, char* dest, uint32_t dest_size) {
   if (offset >= field.size()) return;
   const size_t copy_size = std::min<size_t>(dest_size, field.size() - offset);
   if (copy_size > 0) memcpy(dest, field.data() + offset, copy_size);
}

// ---------------------------------------------------------------------------
// Iterator pool
//
// One pool per iterator family, so a contract may hold up to max_kv_iterators
// of each kind at once. The slot array starts empty and grows on demand, so an
// action that opens no iterator pays no heap and one that opens a few pays for
// a few slots. Growth moves slots, so do not hold a slot reference across an
// allocate(). A handle's slot number does not depend on the array's size.
// ---------------------------------------------------------------------------

template<typename Slot>
class kv_iterator_pool {
public:
   /// Opens the lowest free slot on (table_id, code), bounded by `prefix`, and returns its index.
   uint32_t allocate(uint16_t table_id, account_name code, std::string_view prefix) {
      uint32_t idx = find_free();
      auto& s = _slots[idx];
      s.reset();
      s.in_use   = true;
      s.code     = code;
      s.table_id = table_id;
      s.prefix.assign(prefix.begin(), prefix.end());
      return idx;
   }

   void release(uint32_t slot_index) {
      auto& s = get(slot_index);
      s.reset();
      s.in_use = false;
      if (slot_index < _next_free) _next_free = slot_index;
      while (_used_end > 0 && !_slots[_used_end - 1].in_use) --_used_end;
   }

   Slot& get(uint32_t slot_index) {
      SYS_ASSERT(slot_index < _slots.size() && _slots[slot_index].in_use,
                 kv_invalid_iterator, "invalid KV {} iterator slot {}", Slot::pool_name, slot_index);
      return _slots[slot_index];
   }

   const Slot& get(uint32_t slot_index) const {
      SYS_ASSERT(slot_index < _slots.size() && _slots[slot_index].in_use,
                 kv_invalid_iterator, "invalid KV {} iterator slot {}", Slot::pool_name, slot_index);
      return _slots[slot_index];
   }

   /// Closes every iterator. Handles last for one receiver's execution, so exec_one() calls this first.
   void release_all() {
      for (uint32_t i = 0; i < _used_end; ++i) {
         _slots[i].reset();
         _slots[i].in_use = false;
      }
      _next_free = 0;
      _used_end  = 0;
   }

   /// Points every open slot on `row` back at its saved key bytes. Call before a write erases `row` or moves it to
   /// another sort position. The key bytes stay, so the next read or step re-seeks from the old position.
   void forget_row(const typename Slot::row_type& row) {
      for (uint32_t i = 0; i < _used_end; ++i) {
         auto& slot = _slots[i];
         if (slot.on_row && slot.positioned_at(row)) slot.on_row = false;
      }
   }

   /// Slots the pool holds now, whether open or not.
   size_t capacity() const { return _slots.size(); }

private:
   uint32_t find_free() {
      uint32_t i = _next_free;
      while (i < _used_end && _slots[i].in_use) ++i;
      SYS_ASSERT(i < config::max_kv_iterators, kv_iterator_limit_exceeded,
                 "exceeded maximum number of KV {} iterators ({})", Slot::pool_name, config::max_kv_iterators);
      if (i == _slots.size()) {
         _slots.resize(std::clamp<size_t>(_slots.size() * 2, kv_initial_iterator_slots, config::max_kv_iterators));
      }
      _next_free = i + 1;
      _used_end  = std::max(_used_end, _next_free);
      return i;
   }

   std::vector<Slot> _slots;
   uint32_t _next_free = 0; ///< no slot below this one is free
   uint32_t _used_end  = 0; ///< one past the highest open slot; every slot from here up is closed
};

using kv_primary_iterator_pool   = kv_iterator_pool<kv_primary_slot>;
using kv_secondary_iterator_pool = kv_iterator_pool<kv_secondary_slot>;

} } // namespace sysio::chain
