#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/apply_context.hpp>

namespace sysio { namespace chain { namespace webassembly {

   static uint16_t checked_table_id(uint32_t table_id) {
      SYS_ASSERT(table_id <= std::numeric_limits<uint16_t>::max(), kv_key_too_large,
                 "table_id {} exceeds uint16 maximum {}", table_id, std::numeric_limits<uint16_t>::max());
      return static_cast<uint16_t>(table_id);
   }

   // KV primary operations
   int64_t interface::kv_set(uint32_t table_id, uint64_t payer, legacy_span<const char> key, legacy_span<const char> value) {
      return context.kv_set(checked_table_id(table_id), payer, key.data(), key.size(), value.data(), value.size());
   }

   int32_t interface::kv_get(uint32_t table_id, uint64_t code, legacy_span<const char> key, legacy_span<char> value) {
      return context.kv_get(checked_table_id(table_id), name(code), key.data(), key.size(), value.data(), value.size());
   }

   int64_t interface::kv_erase(uint32_t table_id, legacy_span<const char> key) {
      return context.kv_erase(checked_table_id(table_id), key.data(), key.size());
   }

   int32_t interface::kv_contains(uint32_t table_id, uint64_t code, legacy_span<const char> key) {
      return context.kv_contains(checked_table_id(table_id), name(code), key.data(), key.size());
   }

   // KV primary iterators
   uint32_t interface::kv_it_create(uint32_t table_id, uint64_t code, legacy_span<const char> prefix) {
      return context.kv_it_create(checked_table_id(table_id), name(code), prefix.data(), prefix.size());
   }

   void interface::kv_it_destroy(uint32_t handle) {
      context.kv_it_destroy(handle);
   }

   int32_t interface::kv_it_status(uint32_t handle) {
      return context.kv_it_status(handle);
   }

   int32_t interface::kv_it_next(uint32_t handle) {
      return context.kv_it_next(handle);
   }

   int32_t interface::kv_it_prev(uint32_t handle) {
      return context.kv_it_prev(handle);
   }

   int32_t interface::kv_it_lower_bound(uint32_t handle, legacy_span<const char> key) {
      return context.kv_it_lower_bound(handle, key.data(), key.size());
   }

   int32_t interface::kv_it_key(uint32_t handle, uint32_t offset, legacy_span<char> dest, legacy_ptr<uint32_t> actual_size) {
      uint32_t sz = 0;
      int32_t status = context.kv_it_key(handle, offset, dest.data(), dest.size(), sz);
      *actual_size = sz;
      return status;
   }

   int32_t interface::kv_it_value(uint32_t handle, uint32_t offset, legacy_span<char> dest, legacy_ptr<uint32_t> actual_size) {
      uint32_t sz = 0;
      int32_t status = context.kv_it_value(handle, offset, dest.data(), dest.size(), sz);
      *actual_size = sz;
      return status;
   }

   // KV secondary index operations
   void interface::kv_idx_store(uint64_t payer, uint32_t table_id, legacy_span<const char> pri_key, legacy_span<const char> sec_key) {
      context.kv_idx_store(payer, checked_table_id(table_id),
                           pri_key.data(), pri_key.size(), sec_key.data(), sec_key.size());
   }

   void interface::kv_idx_remove(uint32_t table_id, legacy_span<const char> pri_key, legacy_span<const char> sec_key) {
      context.kv_idx_remove(checked_table_id(table_id),
                            pri_key.data(), pri_key.size(), sec_key.data(), sec_key.size());
   }

   void interface::kv_idx_update(uint64_t payer, uint32_t table_id, legacy_span<const char> pri_key,
                                 legacy_span<const char> old_sec_key, legacy_span<const char> new_sec_key) {
      context.kv_idx_update(payer, checked_table_id(table_id),
                            pri_key.data(), pri_key.size(),
                            old_sec_key.data(), old_sec_key.size(),
                            new_sec_key.data(), new_sec_key.size());
   }

   int32_t interface::kv_idx_find_secondary(uint64_t code, uint32_t table_id, legacy_span<const char> sec_key) {
      return context.kv_idx_find_secondary(name(code), checked_table_id(table_id),
                                           sec_key.data(), sec_key.size());
   }

   int32_t interface::kv_idx_lower_bound(uint64_t code, uint32_t table_id, legacy_span<const char> sec_key) {
      return context.kv_idx_lower_bound(name(code), checked_table_id(table_id),
                                        sec_key.data(), sec_key.size());
   }

   int32_t interface::kv_idx_next(uint32_t handle) {
      return context.kv_idx_next(handle);
   }

   int32_t interface::kv_idx_prev(uint32_t handle) {
      return context.kv_idx_prev(handle);
   }

   int32_t interface::kv_idx_key(uint32_t handle, uint32_t offset, legacy_span<char> dest, legacy_ptr<uint32_t> actual_size) {
      uint32_t sz = 0;
      int32_t status = context.kv_idx_key(handle, offset, dest.data(), dest.size(), sz);
      *actual_size = sz;
      return status;
   }

   int32_t interface::kv_idx_primary_key(uint32_t handle, uint32_t offset, legacy_span<char> dest, legacy_ptr<uint32_t> actual_size) {
      uint32_t sz = 0;
      int32_t status = context.kv_idx_primary_key(handle, offset, dest.data(), dest.size(), sz);
      *actual_size = sz;
      return status;
   }

   void interface::kv_idx_destroy(uint32_t handle) {
      context.kv_idx_destroy(handle);
   }

} } } // namespace sysio::chain::webassembly
