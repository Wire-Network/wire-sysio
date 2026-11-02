#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/webassembly/preconditions.hpp>
#include <sysio/chain/apply_context.hpp>
#include <magic_enum/magic_enum.hpp>

namespace sysio { namespace chain { namespace webassembly {

   /// Narrows a table_id to the host's width. An id past the top aborts, or it would alias a live table.
   static uint16_t checked_table_id(uint32_t table_id) {
      constexpr uint32_t max_table_id = std::numeric_limits<uint16_t>::max();
      SYS_ASSERT(table_id <= max_table_id, kv_invalid_table_id,
                 "table_id {} exceeds maximum {}", table_id, max_table_id);
      return static_cast<uint16_t>(table_id);
   }

   /// Views a span of contract memory, valid for the host call.
   static std::string_view to_sv(span<const char> bytes) {
      return std::string_view(bytes.data(), bytes.size());
   }

   /// Converts kv_set's mode argument. Any value that is not a kv_set_mode aborts.
   static kv_set_mode checked_set_mode(uint32_t mode) {
      const auto set_mode = magic_enum::enum_cast<kv_set_mode>(mode);
      SYS_ASSERT(set_mode.has_value(), kv_invalid_mode, "kv_set mode {} is not upsert, insert or update", mode);
      return *set_mode;
   }

   /// Bundles a read's two output buffers for the context, which fills in the sizes to report.
   static kv_row_buffers to_buffers(span<char> first, span<char> second) {
      return kv_row_buffers{first.data(), static_cast<uint32_t>(first.size()),
                            second.data(), static_cast<uint32_t>(second.size())};
   }

   /// Hands back a read's sizes and status, in that order, once its copies are done. The sizes must not overlap: a
   /// misaligned pointer writes back when its proxy dies, in an order each compiler picks, so nodes could disagree.
   static int32_t report(kv_status status, const kv_row_buffers& row, aligned_ptr<uint32_t>& first_size,
                         aligned_ptr<uint32_t>& second_size) {
      SYS_ASSERT(!is_aliasing(detail::to_span(first_size), detail::to_span(second_size)), wasm_exception,
                 "pointers not allowed to alias");
      *first_size  = row.first_size;
      *second_size = row.second_size;
      return magic_enum::enum_integer(status);
   }

   // KV primary operations
   int32_t interface::kv_set(uint32_t table_id, uint64_t payer, span<const char> key, span<const char> value, uint32_t mode) {
      // Separate statements, because each compiler picks its own order for function arguments.
      const uint16_t    table    = checked_table_id(table_id);
      const kv_set_mode set_mode = checked_set_mode(mode);
      return magic_enum::enum_integer(context.kv_set(table, payer, to_sv(key), to_sv(value), set_mode));
   }

   int32_t interface::kv_erase(uint32_t table_id, span<const char> key) {
      return magic_enum::enum_integer(context.kv_erase(checked_table_id(table_id), to_sv(key)));
   }

   int32_t interface::kv_get(uint32_t table_id, uint64_t code, span<const char> key, uint32_t offset, span<char> value) {
      return context.kv_get(checked_table_id(table_id), name(code), to_sv(key), offset, value.data(), value.size());
   }

   // KV secondary index operations
   int32_t interface::kv_idx_store(uint32_t table_id, uint64_t payer, span<const char> pri_key, span<const char> sec_key) {
      return magic_enum::enum_integer(
         context.kv_idx_store(checked_table_id(table_id), payer, to_sv(pri_key), to_sv(sec_key)));
   }

   int32_t interface::kv_idx_update(uint32_t table_id, uint64_t payer, span<const char> pri_key,
                                    span<const char> old_sec_key, span<const char> new_sec_key) {
      return magic_enum::enum_integer(
         context.kv_idx_update(checked_table_id(table_id), payer, to_sv(pri_key), to_sv(old_sec_key), to_sv(new_sec_key)));
   }

   int32_t interface::kv_idx_erase(uint32_t table_id, span<const char> pri_key, span<const char> sec_key) {
      return magic_enum::enum_integer(context.kv_idx_erase(checked_table_id(table_id), to_sv(pri_key), to_sv(sec_key)));
   }

   // KV primary iterators
   int32_t interface::kv_it_create(uint32_t table_id, uint64_t code, span<const char> prefix, span<const char> seek) {
      return context.kv_it_create(checked_table_id(table_id), name(code), to_sv(prefix), to_sv(seek));
   }

   int32_t interface::kv_it_next(int32_t handle, span<char> key, span<char> value,
                                 aligned_ptr<uint32_t> key_size, aligned_ptr<uint32_t> value_size) {
      auto row = to_buffers(key, value);
      return report(context.kv_it_next(handle, row), row, key_size, value_size);
   }

   int32_t interface::kv_it_prev(int32_t handle, span<char> key, span<char> value,
                                 aligned_ptr<uint32_t> key_size, aligned_ptr<uint32_t> value_size) {
      auto row = to_buffers(key, value);
      return report(context.kv_it_prev(handle, row), row, key_size, value_size);
   }

   int32_t interface::kv_it_lower_bound(int32_t handle, span<const char> seek, span<char> key, span<char> value,
                                        aligned_ptr<uint32_t> key_size, aligned_ptr<uint32_t> value_size) {
      auto row = to_buffers(key, value);
      return report(context.kv_it_lower_bound(handle, to_sv(seek), row), row, key_size, value_size);
   }

   int32_t interface::kv_it_read(int32_t handle, uint32_t offset, span<char> key, span<char> value,
                                 aligned_ptr<uint32_t> key_size, aligned_ptr<uint32_t> value_size) {
      auto row = to_buffers(key, value);
      return report(context.kv_it_read(handle, offset, row), row, key_size, value_size);
   }

   void interface::kv_it_destroy(int32_t handle) {
      context.kv_it_destroy(handle);
   }

   // KV secondary iterators
   int32_t interface::kv_idx_create(uint32_t table_id, uint64_t code, span<const char> prefix, span<const char> seek) {
      return context.kv_idx_create(checked_table_id(table_id), name(code), to_sv(prefix), to_sv(seek));
   }

   int32_t interface::kv_idx_next(int32_t handle, span<char> sec_key, span<char> pri_key,
                                  aligned_ptr<uint32_t> sec_size, aligned_ptr<uint32_t> pri_size) {
      auto entry = to_buffers(sec_key, pri_key);
      return report(context.kv_idx_next(handle, entry), entry, sec_size, pri_size);
   }

   int32_t interface::kv_idx_prev(int32_t handle, span<char> sec_key, span<char> pri_key,
                                  aligned_ptr<uint32_t> sec_size, aligned_ptr<uint32_t> pri_size) {
      auto entry = to_buffers(sec_key, pri_key);
      return report(context.kv_idx_prev(handle, entry), entry, sec_size, pri_size);
   }

   int32_t interface::kv_idx_lower_bound(int32_t handle, span<const char> seek, span<char> sec_key, span<char> pri_key,
                                         aligned_ptr<uint32_t> sec_size, aligned_ptr<uint32_t> pri_size) {
      auto entry = to_buffers(sec_key, pri_key);
      return report(context.kv_idx_lower_bound(handle, to_sv(seek), entry), entry, sec_size, pri_size);
   }

   int32_t interface::kv_idx_read(int32_t handle, span<char> sec_key, span<char> pri_key,
                                  aligned_ptr<uint32_t> sec_size, aligned_ptr<uint32_t> pri_size) {
      auto entry = to_buffers(sec_key, pri_key);
      return report(context.kv_idx_read(handle, entry), entry, sec_size, pri_size);
   }

   void interface::kv_idx_destroy(int32_t handle) {
      context.kv_idx_destroy(handle);
   }

   int32_t interface::kv_idx_find(uint32_t table_id, uint64_t code, span<const char> prefix, span<const char> sec_key) {
      return context.kv_idx_find(checked_table_id(table_id), name(code), to_sv(prefix), to_sv(sec_key));
   }

} } } // namespace sysio::chain::webassembly
