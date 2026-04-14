#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <fc/io/raw.hpp>

namespace sysio::trace_api {

   std::shared_ptr<chain::abi_serializer> abi_data_handler::get_serializer(chain::name account, uint64_t global_seq) {
      const cache_key key{account, global_seq};

      {
         std::lock_guard lock(_cache_mtx);
         auto it = _cache_map.find(key);
         if (it != _cache_map.end()) {
            // Move to MRU (front).  splice is O(1) on std::list.
            _cache_list.splice(_cache_list.begin(), _cache_list, it->second);
            return it->second->second;
         }
      }

      // Miss: look up ABI bytes and build the serializer outside the lock to avoid
      // blocking other cache users during a potentially slow file read / unpack.
      if (!_abi_lookup_fn) return nullptr;

      auto abi_bytes = _abi_lookup_fn(account, global_seq);
      if (!abi_bytes || abi_bytes->empty()) return nullptr;

      std::shared_ptr<chain::abi_serializer> serializer;
      try {
         chain::abi_def abi;
         auto ds = fc::datastream<const char*>(abi_bytes->data(), abi_bytes->size());
         fc::raw::unpack(ds, abi);
         serializer = std::make_shared<chain::abi_serializer>(std::move(abi),
            chain::abi_serializer::create_yield_function(fc::microseconds::maximum()));
      } catch (...) {
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         return nullptr;
      }

      // Insert into cache.  Another thread may have raced us — if so, return that entry.
      std::lock_guard lock(_cache_mtx);
      auto it = _cache_map.find(key);
      if (it != _cache_map.end()) {
         _cache_list.splice(_cache_list.begin(), _cache_list, it->second);
         return it->second->second;
      }
      _cache_list.emplace_front(key, serializer);
      _cache_map.emplace(key, _cache_list.begin());
      while (_cache_list.size() > _cache_capacity) {
         _cache_map.erase(_cache_list.back().first);
         _cache_list.pop_back();
      }
      return serializer;
   }

   std::tuple<fc::variant, std::optional<fc::variant>> abi_data_handler::serialize_to_variant(const std::variant<action_trace_v0>& action) {
      return std::visit([&](const auto& a) -> std::tuple<fc::variant, std::optional<fc::variant>> {
         auto serializer = get_serializer(a.account, a.global_sequence);
         if (!serializer) return {};

         try {
            auto type_name = serializer->get_action_type(a.action);
            if (type_name.empty()) {
               return {};
            }

            // abi_serializer expects a yield function that takes a recursion depth
            // abis are user provided, do not use a deadline
            auto abi_yield = [](size_t recursion_depth) {
               SYS_ASSERT( recursion_depth < chain::abi_serializer::max_recursion_depth,
                           chain::abi_recursion_depth_exception,
                           "exceeded max_recursion_depth {} ", chain::abi_serializer::max_recursion_depth );
            };

            std::optional<fc::variant> ret_data;
            auto params = serializer->binary_to_variant(type_name, a.data, abi_yield);
            if (a.return_value.size() > 0) {
               auto return_type_name = serializer->get_action_result_type(a.action);
               if (!return_type_name.empty()) {
                  ret_data = serializer->binary_to_variant(return_type_name, a.return_value, abi_yield);
               }
            }
            return {std::move(params), std::move(ret_data)};
         } catch (...) {
            except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         }

         return {};
      }, action);
   }
}
