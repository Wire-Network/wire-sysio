#pragma once

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <sysio/chain/abi_def.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/trace_api/trace.hpp>
#include <sysio/trace_api/common.hpp>

namespace sysio {
   namespace chain {
      struct abi_serializer;
   }

   namespace trace_api {

   /**
    * Data Handler that uses sysio::chain::abi_serializer to decode action data.
    *
    * ABIs are resolved dynamically via an abi_lookup_fn callback, typically backed
    * by the abi_log on disk.  Given (account, action_global_sequence) it returns
    * {effective_abi_global_seq, abi_bytes} -- the setabi record that was in effect
    * at that action, or nullopt.
    *
    * A bounded LRU caches constructed abi_serializers by (account, effective_abi_global_seq)
    * so bulk queries that span many actions sharing the same ABI version hit the same
    * cache entry instead of rebuilding the serializer per action.
    *
    * Decode results flag success/failure via abi_data_handler::decode_status so the
    * caller can emit a decode_error field on failure while still returning the raw
    * hex payload.
    *
    * Can be used directly as a Data_handler_provider OR shared between request_handlers
    * using the ::shared_provider abstraction.
    */
   class abi_data_handler {
   public:
      struct lookup_entry {
         uint64_t          effective_global_seq = 0;
         std::vector<char> abi_bytes;
      };

      /// Callback: (account, action_global_sequence) -> {effective_abi_global_seq, abi_bytes}
      /// for the ABI version in effect at that action.  Called on the HTTP thread; must be thread-safe.
      using abi_lookup_fn = std::function<std::optional<lookup_entry>(chain::name, uint64_t)>;

      enum class decode_status {
         not_attempted,  // no ABI available for this action
         ok,             // decoded successfully
         failed          // ABI was available but decoding threw
      };

      struct decode_result {
         decode_status              status = decode_status::not_attempted;
         fc::variant                params;         // decoded action data (empty on failure)
         std::optional<fc::variant> return_data;    // decoded return_value (empty when no return type)
         std::string                error_message;  // populated only when status == failed
      };

      // At ~500 KB per abi_serializer (for large contracts like sysio.system),
      // 256 entries caps cache memory at roughly 130 MB in the worst case.
      static constexpr size_t default_cache_capacity = 256;

      explicit abi_data_handler( exception_handler except_handler, abi_lookup_fn lookup_fn = {},
                                 size_t cache_capacity = default_cache_capacity )
      :_abi_lookup_fn( std::move( lookup_fn ) )
      ,except_handler( std::move( except_handler ) )
      ,_cache_capacity( cache_capacity )
      {}

      /**
       * Decode the data + return_value of an action using the ABI that was in
       * effect when it executed.  Callers inspect decode_result::status to decide
       * whether to emit params/return_data or a decode_error field.
       */
      decode_result decode(const action_trace_v0& action);

      /**
       * Legacy convenience wrapper: returns only {params, return_data} and drops
       * the status/error fields.  Retained for callers that still expect the
       * tuple shape; prefer decode() for new code.
       */
      std::tuple<fc::variant, std::optional<fc::variant>> serialize_to_variant(const std::variant<action_trace_v0>& action);

      /**
       * Utility class that allows multiple request_handlers to share the same abi_data_handler
       */
      class shared_provider {
         public:
         explicit shared_provider(const std::shared_ptr<abi_data_handler>& handler)
         :handler(handler)
         {}

         decode_result decode(const action_trace_v0& action) {
            return handler->decode(action);
         }

         std::tuple<fc::variant, std::optional<fc::variant>> serialize_to_variant( const std::variant<action_trace_v0>& action ) {
            return handler->serialize_to_variant(action);
         }

         std::shared_ptr<abi_data_handler> handler;
      };

   private:
      // Look up or construct the abi_serializer in effect for the action.  Returns
      // nullptr if no ABI is available or construction failed.  The cache key is
      // (account, effective_abi_global_seq) so multiple actions sharing an ABI
      // version all hit the same entry.
      std::shared_ptr<chain::abi_serializer> get_serializer(chain::name account, uint64_t action_global_seq);

      using cache_key = std::pair<chain::name /*account*/, uint64_t /*effective_abi_global_seq*/>;
      struct cache_key_hash {
         size_t operator()(const cache_key& k) const noexcept {
            return std::hash<chain::name>{}(k.first) ^ (std::hash<uint64_t>{}(k.second) << 1);
         }
      };

      abi_lookup_fn     _abi_lookup_fn;
      exception_handler except_handler;

      // LRU cache of (account, effective_abi_global_seq) -> abi_serializer.
      // _cache_list: MRU at front, LRU at back.  _cache_map: key -> iterator into list.
      std::mutex                                                                             _cache_mtx;
      std::list<std::pair<cache_key, std::shared_ptr<chain::abi_serializer>>>                _cache_list;
      std::unordered_map<cache_key,
                         decltype(_cache_list)::iterator,
                         cache_key_hash>                                                     _cache_map;
      const size_t                                                                           _cache_capacity;
   };
} }
