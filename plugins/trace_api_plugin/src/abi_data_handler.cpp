#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/io/raw.hpp>

namespace sysio::trace_api {

   namespace {
      // ABIs are user-provided; the yield function only enforces the recursion-depth cap and
      // intentionally has no wall-clock deadline.  Shared between the variant and streaming paths
      // so they stay in lock-step on what counts as "too deep".
      auto make_abi_yield() {
         return [](size_t recursion_depth) {
            SYS_ASSERT( recursion_depth < chain::abi_serializer::max_recursion_depth, chain::abi_recursion_depth_exception,
                        "exceeded max_recursion_depth {}", chain::abi_serializer::max_recursion_depth );
         };
      }
   }

   std::shared_ptr<chain::abi_serializer> abi_data_handler::get_serializer(chain::name account, uint64_t action_global_seq) {
      if (!_abi_seq_resolver) return nullptr;

      // Resolve the effective ABI version first - an index-only probe with no blob I/O.  Using it
      // as the cache key means N actions sharing the same setabi all hit the same entry; keying on
      // the action's global_seq instead (as a prior impl did) defeats the cache.  The blob itself
      // is fetched only on a cache miss, so a bulk query over thousands of actions sharing one ABI
      // version pays for a single blob read.
      const std::optional<uint64_t> effective_seq = _abi_seq_resolver(account, action_global_seq);
      if (!effective_seq) return nullptr;

      const cache_key key{account, *effective_seq};

      {
         std::lock_guard lock(_cache_mtx);
         auto it = _cache_map.find(key);
         if (it != _cache_map.end()) {
            // Move to MRU (front).  splice is O(1) on std::list.
            _cache_list.splice(_cache_list.begin(), _cache_list, it->second);
            return it->second->second;
         }
      }

      // Miss: fetch the blob and build the serializer outside the lock to avoid
      // blocking other cache users during the read and a potentially slow unpack.
      if (!_abi_blob_fetcher) return nullptr;
      const std::optional<std::vector<char>> abi_bytes = _abi_blob_fetcher(account, *effective_seq);
      // An empty blob is a recorded ABI *clear* (setabi with no abi): the version exists so the
      // resolver finds it, but there is nothing to decode with.
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

      auto abi_yield = make_abi_yield();

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

   void abi_data_handler::serialize_to_json_stream(const std::variant<action_trace_v0>& action, fc::json_writer& w) {
      std::visit([&](const auto& a) {
         auto serializer = get_serializer(a.account, a.global_sequence);
         if (!serializer) return;
         auto type_name = serializer->get_action_type(a.action);
         if (type_name.empty()) return;

         auto abi_yield = make_abi_yield();

         // Kept in lock-step with decode() + serialize_to_variant: the get_block /
         // get_transaction_trace pipeline emits params/return_data all-or-nothing.  Any decode
         // failure rewinds every byte this function wrote so the streamed shape matches the
         // variant path's legacy empty shape (no params, no return_data).
         auto cp = w.checkpoint();
         try {
            w.key("params");
            serializer->binary_to_json_stream(type_name, a.data, w, abi_yield);
         } catch (...) {
            w.rewind(cp);
            except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
            return;
         }

         if (a.return_value.empty()) return;
         auto return_type_name = serializer->get_action_result_type(a.action);
         if (return_type_name.empty()) return;

         try {
            w.key("return_data");
            serializer->binary_to_json_stream(return_type_name, a.return_value, w, abi_yield);
         } catch (...) {
            // Roll back params as well: the variant path returns the legacy empty shape when
            // return_value fails to decode, and the two paths must emit identical JSON.
            w.rewind(cp);
            except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         }
      }, action);
   }
}
