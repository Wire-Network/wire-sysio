#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <fc/io/raw.hpp>

namespace sysio::trace_api {

   std::shared_ptr<chain::abi_serializer> abi_data_handler::get_serializer(chain::name account, uint64_t action_global_seq) {
      if (!_abi_lookup_fn) return nullptr;

      // Resolve the effective ABI version first.  Using it as the cache key means
      // N actions sharing the same setabi all hit the same entry; keying on the
      // action's global_seq instead (as a prior impl did) defeats the cache.
      auto lookup = _abi_lookup_fn(account, action_global_seq);
      if (!lookup || lookup->abi_bytes.empty()) return nullptr;

      const cache_key key{account, lookup->effective_global_seq};

      {
         std::lock_guard lock(_cache_mtx);
         auto it = _cache_map.find(key);
         if (it != _cache_map.end()) {
            // Move to MRU (front).  splice is O(1) on std::list.
            _cache_list.splice(_cache_list.begin(), _cache_list, it->second);
            return it->second->second;
         }
      }

      // Miss: build the serializer outside the lock to avoid blocking other
      // cache users during a potentially slow unpack.
      std::shared_ptr<chain::abi_serializer> serializer;
      try {
         chain::abi_def abi;
         auto ds = fc::datastream<const char*>(lookup->abi_bytes.data(), lookup->abi_bytes.size());
         fc::raw::unpack(ds, abi);
         serializer = std::make_shared<chain::abi_serializer>(std::move(abi),
            chain::abi_serializer::create_yield_function(fc::microseconds::maximum()));
      } catch (...) {
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         return nullptr;
      }

      // Insert into cache.  Another thread may have raced us -- if so, return that entry.
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

   abi_data_handler::decode_result abi_data_handler::decode(const action_trace_v0& a) {
      // Named local return so NRVO constructs directly into the caller's slot.
      decode_result result;

      auto serializer = get_serializer(a.account, a.global_sequence);
      if (!serializer) return result;

      auto type_name = serializer->get_action_type(a.action);
      if (type_name.empty()) return result;

      // abis are user provided, do not use a deadline
      auto abi_yield = [](size_t recursion_depth) {
         SYS_ASSERT( recursion_depth < chain::abi_serializer::max_recursion_depth,
                     chain::abi_recursion_depth_exception,
                     "exceeded max_recursion_depth {} ", chain::abi_serializer::max_recursion_depth );
      };

      // Separate try blocks so that a failure decoding return_value does not
      // discard successfully-decoded params (and vice versa).
      try {
         result.params = serializer->binary_to_variant(type_name, a.data, abi_yield);
         result.status = decode_status::ok;
      } catch (const std::exception& e) {
         result.status        = decode_status::failed;
         result.error_message = e.what();
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         return result;
      } catch (...) {
         result.status        = decode_status::failed;
         result.error_message = "unknown exception decoding action data";
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         return result;
      }

      if (a.return_value.size() > 0) {
         auto return_type_name = serializer->get_action_result_type(a.action);
         if (!return_type_name.empty()) {
            try {
               result.return_data = serializer->binary_to_variant(return_type_name, a.return_value, abi_yield);
            } catch (const std::exception& e) {
               // Params decoded OK but return_value failed: keep params, flag failure
               // and surface the error message.  Callers can still emit params.
               result.status        = decode_status::failed;
               result.error_message = std::string("return_value decode failed: ") + e.what();
               except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
            } catch (...) {
               result.status        = decode_status::failed;
               result.error_message = "unknown exception decoding return_value";
               except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
            }
         }
      }

      return result;
   }

   std::tuple<fc::variant, std::optional<fc::variant>> abi_data_handler::serialize_to_variant(const std::variant<action_trace_v0>& action) {
      return std::visit([&](const auto& a) -> std::tuple<fc::variant, std::optional<fc::variant>> {
         auto r = decode(a);
         if (r.status == decode_status::ok)
            return {std::move(r.params), std::move(r.return_data)};
         // failed or not_attempted -> legacy empty shape
         return {};
      }, action);
   }
}
