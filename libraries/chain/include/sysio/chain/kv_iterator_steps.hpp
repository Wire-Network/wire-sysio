#pragma once

#include <sysio/chain/kv_context.hpp>
#include <sysio/chain/kv_store.hpp>

#include <algorithm>
#include <string_view>

// How the iterator intrinsics move a slot over an engine's cursors. They live here so that a test can drive the same
// code on either engine: unittests/kv_store_tests.cpp checks them against a model that holds its position by key.

namespace sysio::chain {

// What differs between the two iterator families. Prefix and seek bytes apply to bound_key(), the key that leads the
// family's sort order: the row key, or the secondary key of an index entry.

struct kv_primary_family {
   using row_type  = kv_row;
   using slot_type = kv_primary_slot;

   static auto open_cursor(const kv_store& store) { return store.open_row_cursor(); }
   static std::string_view bound_key(const row_type& row) { return row.key; }
   static kv_position position(const slot_type& slot) { return {slot.code, slot.table_id, to_sv(slot.current_key)}; }
   static void save_position(slot_type& slot, const row_type& row) {
      slot.current_key.assign(row.key.begin(), row.key.end());
   }
   static void clear_position(slot_type& slot) { slot.current_key.clear(); }
   static void read(const row_type& row, uint32_t offset, kv_row_buffers& out) {
      out.first_size  = static_cast<uint32_t>(row.key.size());
      out.second_size = static_cast<uint32_t>(row.value.size());
      kv_copy_field(row.key, 0, out.first, out.first_capacity);
      kv_copy_field(row.value, offset, out.second, out.second_capacity);
   }
};

struct kv_secondary_family {
   using row_type  = kv_index_entry;
   using slot_type = kv_secondary_slot;

   static auto open_cursor(const kv_store& store) { return store.open_index_cursor(); }
   static std::string_view bound_key(const row_type& entry) { return entry.sec_key; }
   static kv_position position(const slot_type& slot) {
      return {slot.code, slot.table_id, to_sv(slot.current_sec_key), to_sv(slot.current_pri_key)};
   }
   static void save_position(slot_type& slot, const row_type& entry) {
      slot.current_sec_key.assign(entry.sec_key.begin(), entry.sec_key.end());
      slot.current_pri_key.assign(entry.pri_key.begin(), entry.pri_key.end());
   }
   static void clear_position(slot_type& slot) {
      slot.current_sec_key.clear();
      slot.current_pri_key.clear();
   }
   static void read(const row_type& entry, uint32_t /*offset*/, kv_row_buffers& out) {
      out.first_size  = static_cast<uint32_t>(entry.sec_key.size());
      out.second_size = static_cast<uint32_t>(entry.pri_key.size());
      kv_copy_field(entry.sec_key, 0, out.first, out.first_capacity);
      kv_copy_field(entry.pri_key, 0, out.second, out.second_capacity);
   }
};

/// Gives a newly allocated `slot` a cursor, unless an earlier use of the slot left one.
template<typename Family>
void kv_prepare_cursor(const kv_store& store, typename Family::slot_type& slot) {
   if (!slot.cursor) slot.cursor = Family::open_cursor(store);
}

/// Puts `slot` at end.
template<typename Family>
void kv_park_at_end(typename Family::slot_type& slot) {
   slot.at_end = true;
   slot.on_row = false;
   Family::clear_position(slot);
}

/// Puts `slot` on `row`, where its cursor just moved, if the move found a row in the slot's table and prefix. Otherwise
/// puts it at end.
template<typename Family>
void kv_land(typename Family::slot_type& slot, bool found, const typename Family::row_type& row) {
   if (found && row.code == slot.code && row.table_id == slot.table_id &&
       Family::bound_key(row).starts_with(to_sv(slot.prefix))) {
      slot.at_end = false;
      slot.on_row = true;
      Family::save_position(slot, row);
   } else {
      kv_park_at_end<Family>(slot);
   }
}

/// Moves `slot` to the first row at or above `seek`, and fills `row` from it. A seek below the prefix starts from the
/// prefix.
template<typename Family>
void kv_seek(typename Family::slot_type& slot, std::string_view seek, typename Family::row_type& row) {
   const bool found = slot.cursor->seek_lower({slot.code, slot.table_id, std::max(seek, to_sv(slot.prefix))}, row);
   kv_land<Family>(slot, found, row);
}

/// Moves `slot` to the row after its position, and fills `row` from it. At end it stays at end.
template<typename Family>
void kv_step_next(typename Family::slot_type& slot, typename Family::row_type& row) {
   if (slot.at_end) return;

   // Fast path: step from the row this slot is on.
   // Slow path: re-seek by key bytes (the row was erased). upper, not lower: "next" must advance strictly past the
   // current key, and if the row was erased then reinserted at the same key within this transaction, a lower seek
   // would land on it again and the iterator would never advance. Index entries sharing the secondary key but with a
   // greater primary key still compare greater, so they are not skipped.
   const bool found = slot.on_row ? slot.cursor->next(row) : slot.cursor->seek_upper(Family::position(slot), row);
   kv_land<Family>(slot, found, row);
}

/// Moves `slot` to the row before its position, and fills `row` from it. From end it moves to the last row in the
/// prefix, and from the first row of the store to end.
template<typename Family>
void kv_step_prev(typename Family::slot_type& slot, typename Family::row_type& row) {
   if (slot.at_end) {
      if (const auto successor = kv_prefix_successor(to_sv(slot.prefix))) {
         slot.cursor->seek_lower({slot.code, slot.table_id, *successor}, row);
      } else {
         slot.cursor->seek_past_table(slot.code, slot.table_id, row);
      }
   } else if (!slot.on_row) {
      slot.cursor->seek_lower(Family::position(slot), row);
   }
   const bool found = slot.cursor->prev(row);
   kv_land<Family>(slot, found, row);
}

/// Reports the row a move just landed on.
template<typename Family>
kv_status kv_report_landing(const typename Family::slot_type& slot, const typename Family::row_type& row,
                            kv_row_buffers& out) {
   if (slot.at_end) return kv_status::end;
   Family::read(row, 0, out);
   return kv_status::ok;
}

/// Reports the row stored at `slot`'s position, which by now may be a different row, or none, than the one it landed
/// on.
template<typename Family>
kv_status kv_report_position(typename Family::slot_type& slot, uint32_t offset, kv_row_buffers& out) {
   if (slot.at_end) return kv_status::end;
   typename Family::row_type row;
   if (slot.on_row) {
      slot.cursor->read(row);
   } else {
      if (!slot.cursor->seek_lower(Family::position(slot), row) || !slot.positioned_at(row)) return kv_status::no_row;
      slot.on_row = true;
   }
   Family::read(row, offset, out);
   return kv_status::ok;
}

} // namespace sysio::chain
