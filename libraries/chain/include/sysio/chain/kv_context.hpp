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

   // Primary iterator: prefix for bounded iteration
   std::vector<char>  prefix;

   // Secondary iterator: table + index_id
   name              table;
   uint8_t           index_id = 0;

   // Current position key bytes (for re-seeking after invalidation)
   std::vector<char>  current_key;
   // For secondary iterators: current secondary key
   std::vector<char>  current_sec_key;
   // For secondary iterators: current primary key
   std::vector<char>  current_pri_key;
};

class kv_iterator_pool {
public:
   kv_iterator_pool() : _slots(config::max_kv_iterators) {}

   uint32_t allocate_primary(account_name code, const char* prefix, uint32_t prefix_size) {
      for (uint32_t i = 0; i < _slots.size(); ++i) {
         if (!_slots[i].in_use) {
            auto& s = _slots[i];
            s.in_use = true;
            s.is_primary = true;
            s.status = kv_it_stat::iterator_end;
            s.code = code;
            s.prefix.assign(prefix, prefix + prefix_size);
            s.table = name();
            s.index_id = 0;
            s.current_key.clear();
            s.current_sec_key.clear();
            s.current_pri_key.clear();
            return i;
         }
      }
      SYS_THROW(kv_iterator_limit_exceeded,
         "exceeded maximum number of KV iterators ({})", config::max_kv_iterators);
   }

   uint32_t allocate_secondary(account_name code, name table, uint8_t index_id) {
      for (uint32_t i = 0; i < _slots.size(); ++i) {
         if (!_slots[i].in_use) {
            auto& s = _slots[i];
            s.in_use = true;
            s.is_primary = false;
            s.status = kv_it_stat::iterator_end;
            s.code = code;
            s.prefix.clear();
            s.table = table;
            s.index_id = index_id;
            s.current_key.clear();
            s.current_sec_key.clear();
            s.current_pri_key.clear();
            return i;
         }
      }
      SYS_THROW(kv_iterator_limit_exceeded,
         "exceeded maximum number of KV iterators ({})", config::max_kv_iterators);
   }

   void release(uint32_t handle) {
      SYS_ASSERT(handle < _slots.size() && _slots[handle].in_use,
         kv_invalid_iterator, "invalid KV iterator handle {}", handle);
      auto& s = _slots[handle];
      s.in_use = false;
      s.prefix.clear();
      s.prefix.shrink_to_fit();
      s.current_key.clear();
      s.current_key.shrink_to_fit();
      s.current_sec_key.clear();
      s.current_sec_key.shrink_to_fit();
      s.current_pri_key.clear();
      s.current_pri_key.shrink_to_fit();
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
   std::vector<kv_iterator_slot> _slots;
};

} } // namespace sysio::chain
