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
    * by the abi_log on disk.  Given (account, global_sequence) it returns the raw
    * ABI bytes that were in effect at that point in chain history, or nullopt.
    *
    * A bounded LRU caches constructed abi_serializers by (account, global_seq) so
    * bulk queries over the same contract do not rebuild the serializer per action.
    *
    * Can be used directly as a Data_handler_provider OR shared between request_handlers
    * using the ::shared_provider abstraction.
    */
   class abi_data_handler {
   public:
      /// Callback: (account, global_sequence) -> raw ABI bytes in effect at that sequence, or nullopt.
      /// Called on the HTTP thread; must be thread-safe.
      using abi_lookup_fn = std::function<std::optional<std::vector<char>>(chain::name, uint64_t)>;

      static constexpr size_t default_cache_capacity = 128;

      explicit abi_data_handler( exception_handler except_handler, abi_lookup_fn lookup_fn = {},
                                 size_t cache_capacity = default_cache_capacity )
      :_abi_lookup_fn( std::move( lookup_fn ) )
      ,except_handler( std::move( except_handler ) )
      ,_cache_capacity( cache_capacity )
      {}

      /**
       * Given an action trace, produce a tuple representing the `data` and `return_value` fields
       * in the trace, decoded via the ABI in effect at that action's global_sequence.
       *
       * @param action - trace of the action including metadata necessary for finding the ABI
       * @return tuple where the first element is a variant representing the decoded `data` field,
       *         and the second element represents the decoded `return_value` field.
       *         Both are empty variants when the ABI is unavailable or decoding fails.
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

         std::tuple<fc::variant, std::optional<fc::variant>> serialize_to_variant( const std::variant<action_trace_v0>& action ) {
            return handler->serialize_to_variant(action);
         }

         std::shared_ptr<abi_data_handler> handler;
      };

   private:
      // Look up or construct the abi_serializer in effect for (account, global_seq).
      // Returns nullptr if no ABI is available or construction failed.
      std::shared_ptr<chain::abi_serializer> get_serializer(chain::name account, uint64_t global_seq);

      using cache_key = std::pair<uint64_t /*account*/, uint64_t /*global_seq*/>;
      struct cache_key_hash {
         size_t operator()(const cache_key& k) const noexcept {
            return std::hash<uint64_t>{}(k.first) ^ (std::hash<uint64_t>{}(k.second) << 1);
         }
      };

      abi_lookup_fn     _abi_lookup_fn;
      exception_handler except_handler;

      // LRU cache of (account, global_seq) -> abi_serializer.
      // _cache_list: MRU at front, LRU at back.  _cache_map: key -> iterator into list.
      std::mutex                                                                             _cache_mtx;
      std::list<std::pair<cache_key, std::shared_ptr<chain::abi_serializer>>>                _cache_list;
      std::unordered_map<cache_key,
                         decltype(_cache_list)::iterator,
                         cache_key_hash>                                                     _cache_map;
      const size_t                                                                           _cache_capacity;
   };
} }
