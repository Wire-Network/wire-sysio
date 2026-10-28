#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/io/raw.hpp>
#include <fc/reflect/json_stream.hpp>

#include <string_view>

namespace sysio::trace_api {

   namespace {
      // decode_to_json_stream's decode_error texts when the exception carries no message of its own.
      constexpr std::string_view unknown_params_error       = "unknown exception decoding action data";
      constexpr std::string_view return_value_error_prefix  = "return_value decode failed: ";
      constexpr std::string_view unknown_return_value_error = "unknown exception decoding return_value";

      // ABIs are user-provided; the yield function only enforces the recursion-depth cap and
      // intentionally has no wall-clock deadline.  Shared by both decoders so they agree on what
      // counts as "too deep".
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

   void abi_data_handler::decode_to_json_stream(const action_trace_v0& a, fc::json_writer& w) {
      auto serializer = get_serializer(a.account, a.global_sequence);
      if (!serializer) return;

      auto type_name = serializer->get_action_type(a.action);
      if (type_name.empty()) return;

      auto abi_yield = make_abi_yield();

      // decode_error is written before except_handler runs: if the failure was the response budget aborting the
      // writer, that write rethrows the abort and the budget rejection is not logged as a decode failure.
      const auto params_cp = w.checkpoint();
      try {
         w.key("params");
         serializer->binary_to_json_stream(type_name, a.data, w, abi_yield);
      } catch (const std::exception& e) {
         w.rewind(params_cp);
         w.set("decode_error", std::string_view(e.what()));
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         return;
      } catch (...) {
         w.rewind(params_cp);
         w.set("decode_error", unknown_params_error);
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         return;
      }

      if (a.return_value.empty()) return;
      auto return_type_name = serializer->get_action_result_type(a.action);
      if (return_type_name.empty()) return;

      const auto return_cp = w.checkpoint();
      try {
         w.key("return_data");
         serializer->binary_to_json_stream(return_type_name, a.return_value, w, abi_yield);
      } catch (const std::exception& e) {
         // Params decoded: keep them and report the return_value failure.
         w.rewind(return_cp);
         w.set("decode_error", std::string(return_value_error_prefix) + e.what());
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
      } catch (...) {
         w.rewind(return_cp);
         w.set("decode_error", unknown_return_value_error);
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
      }
   }

   void abi_data_handler::serialize_to_json_stream(const std::variant<action_trace_v0>& action, fc::json_writer& w) {
      std::visit([&](const auto& a) {
         auto serializer = get_serializer(a.account, a.global_sequence);
         if (!serializer) return;
         auto type_name = serializer->get_action_type(a.action);
         if (type_name.empty()) return;

         auto abi_yield = make_abi_yield();

         // get_block / get_transaction_trace emit params/return_data all-or-nothing: any decode
         // failure rewinds every byte this function wrote (no params, no return_data).
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
            // Roll back params as well: all-or-nothing.
            w.rewind(cp);
            except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         }
      }, action);
   }
}
