#pragma once

#include <functional>
#include <optional>
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
    * by the abi_store on disk.  Given (account, global_sequence) it returns the raw
    * ABI bytes that were in effect at that point in chain history, or nullopt.
    *
    * Can be used directly as a Data_handler_provider OR shared between request_handlers
    * using the ::shared_provider abstraction.
    */
   class abi_data_handler {
   public:
      /// Callback: (account, global_sequence) -> raw ABI bytes in effect at that sequence, or nullopt.
      /// Called on the HTTP thread; must be thread-safe.
      using abi_lookup_fn = std::function<std::optional<std::vector<char>>(chain::name, uint64_t)>;

      explicit abi_data_handler( exception_handler except_handler, abi_lookup_fn lookup_fn = {} )
      :_abi_lookup_fn( std::move( lookup_fn ) )
      ,except_handler( std::move( except_handler ) )
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
      abi_lookup_fn _abi_lookup_fn;
      exception_handler except_handler;
   };
} }
