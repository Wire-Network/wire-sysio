#pragma once

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <boost/container_hash/hash.hpp>
#include <sysio/chain/abi_def.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/trace_api/trace.hpp>
#include <sysio/trace_api/common.hpp>

namespace fc { class json_writer; }

namespace sysio {
   namespace chain {
      struct abi_serializer;
   }

   namespace trace_api {

   /**
    * Data Handler that uses sysio::chain::abi_serializer to decode action data.
    *
    * ABIs are resolved dynamically via two callbacks, typically backed by the abi_log on disk:
    *   - abi_seq_resolver_fn: (account, action_global_sequence) -> effective_abi_global_seq of the
    *     setabi record in effect at that action.  Pure index lookup, no blob I/O - called once per
    *     decoded action to compute the serializer cache key.
    *   - abi_blob_fetcher_fn: (account, effective_abi_global_seq) -> abi_bytes for that exact
    *     recorded version.  Performs the blob read - called only on a serializer cache miss, so
    *     bulk queries over actions sharing one ABI version pay for a single blob read instead of
    *     one per action.
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
      /// Callback: (account, action_global_sequence) -> effective_abi_global_seq in effect at that
      /// action, or nullopt when no ABI version is recorded.  Called on the HTTP thread per decoded
      /// action; must be thread-safe and cheap (no blob I/O).
      using abi_seq_resolver_fn = std::function<std::optional<uint64_t>(chain::name, uint64_t)>;

      /// Callback: (account, effective_abi_global_seq) -> the recorded ABI blob for that exact
      /// version, or nullopt.  Called on the HTTP thread only on serializer cache misses; must be
      /// thread-safe.
      using abi_blob_fetcher_fn = std::function<std::optional<std::vector<char>>(chain::name, uint64_t)>;

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

      explicit abi_data_handler( exception_handler except_handler, abi_seq_resolver_fn seq_resolver = {},
                                 abi_blob_fetcher_fn blob_fetcher = {},
                                 size_t cache_capacity = default_cache_capacity )
      :_abi_seq_resolver( std::move( seq_resolver ) )
      ,_abi_blob_fetcher( std::move( blob_fetcher ) )
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
       * Tuple-shape wrapper used by the response_formatter::process_block pipeline
       * (get_block / get_transaction_trace), whose data_handler_function is keyed
       * to the {params, return_data} tuple.  Returns empty variants on decode
       * failure -- callers that need the decode error surfaced (get_actions /
       * get_token_transfers) use decode() directly.
       */
      std::tuple<fc::variant, std::optional<fc::variant>> serialize_to_variant(const std::variant<action_trace_v0>& action);

      /**
       * Streaming counterpart of serialize_to_variant: emits the ABI-decoded "params" and (when applicable)
       * "return_data" key/value pairs directly into `w` via fc::json_writer, without ever materialising an
       * fc::variant tree.  Pre-conditions: `w` is positioned inside the action's enclosing JSON object such that
       * additional `key(...) value(...)` pairs are valid (i.e. caller has already written global_sequence, receiver,
       * etc.).  No-op if the action's account has no registered ABI or the action is not in the ABI.
       *
       * Fields decode independently: a parse failure on one is logged via `except_handler` and rolled back via
       * `w.rewind()` to its own per-field checkpoint, but does NOT prevent the other from being attempted.  The
       * lone short-circuit case is `chain::abi_recursion_depth_exception` thrown during the params decode -- the
       * ABI is structurally bad, so retrying the other field with the same ABI cannot succeed in a useful way
       * and the function returns early with neither field emitted.  serialize_to_variant uses the same rule.
       *
       * @param action - trace of the action including metadata necessary for finding the ABI
       * @param w      - writer to emit into; on return, zero, one, or both of "params" / "return_data" have been added
       */
      void serialize_to_json_stream(const std::variant<action_trace_v0>& action, fc::json_writer& w);

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

         void serialize_to_json_stream( const std::variant<action_trace_v0>& action, fc::json_writer& w ) {
            handler->serialize_to_json_stream(action, w);
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
            size_t seed = std::hash<chain::name>{}(k.first);
            boost::hash_combine(seed, k.second);
            return seed;
         }
      };

      abi_seq_resolver_fn _abi_seq_resolver;
      abi_blob_fetcher_fn _abi_blob_fetcher;
      exception_handler   except_handler;

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
