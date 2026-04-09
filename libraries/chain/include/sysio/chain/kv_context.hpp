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

struct kv_iterator_slot {
   bool              in_use = false;
   bool              is_primary = true;
   kv_it_stat        status = kv_it_stat::iterator_end;
   account_name      code;
   uint16_t          table_id = 0;  ///< table namespace (primary or secondary index)

   // Primary iterator: prefix for bounded iteration
   std::vector<char>  prefix;

   // Current position key bytes (for re-seeking after invalidation)
   std::vector<char>  current_key;
   // For secondary iterators: current secondary key
   std::vector<char>  current_sec_key;
   // For secondary iterators: current primary key
   std::vector<char>  current_pri_key;

   // Cached chainbase ID for O(1) iterator_to fast path.
   // -1 means no cached ID; falls back to lower_bound re-seek.
   int64_t           cached_id = -1;
};

class kv_iterator_pool {
public:
   kv_iterator_pool() : _slots(config::max_kv_iterators) {}

   uint32_t allocate_primary(uint16_t table_id, account_name code, const char* prefix, uint32_t prefix_size) {
      uint32_t idx = find_free();
      auto& s = _slots[idx];
      s.in_use = true;
      s.is_primary = true;
      s.status = kv_it_stat::iterator_end;
      s.code = code;
      s.table_id = table_id;
      s.prefix.assign(prefix, prefix + prefix_size);
      s.current_key.clear();
      s.current_sec_key.clear();
      s.current_pri_key.clear();
      s.cached_id = -1;
      return idx;
   }

   uint32_t allocate_secondary(account_name code, uint16_t table_id) {
      uint32_t idx = find_free();
      auto& s = _slots[idx];
      s.in_use = true;
      s.is_primary = false;
      s.status = kv_it_stat::iterator_end;
      s.code = code;
      s.table_id = table_id;
      s.prefix.clear();
      s.current_key.clear();
      s.current_sec_key.clear();
      s.current_pri_key.clear();
      s.cached_id = -1;
      return idx;
   }

   void release(uint32_t handle) {
      SYS_ASSERT(handle < _slots.size() && _slots[handle].in_use,
         kv_invalid_iterator, "invalid KV iterator handle {}", handle);
      auto& s = _slots[handle];
      s.in_use = false;
      s.prefix.clear();
      s.current_key.clear();
      s.current_sec_key.clear();
      s.current_pri_key.clear();
      s.cached_id = -1;
      if (handle < _next_free) _next_free = handle;
   }

   kv_iterator_slot& get(uint32_t handle) {
      SYS_ASSERT(handle < _slots.size() && _slots[handle].in_use,
         kv_invalid_iterator, "invalid KV iterator handle {}", handle);
      return _slots[handle];
   }

   const kv_iterator_slot& get(uint32_t handle) const {
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
         "exceeded maximum number of KV iterators ({})", config::max_kv_iterators);
   }

   std::vector<kv_iterator_slot> _slots;
   uint32_t _next_free = 0;
};

} } // namespace sysio::chain
