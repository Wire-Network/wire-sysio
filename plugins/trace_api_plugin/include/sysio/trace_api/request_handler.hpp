#pragma once

#include <algorithm>
#include <limits>
#include <optional>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/trace_api/metadata_log.hpp>
#include <sysio/trace_api/data_log.hpp>
#include <sysio/trace_api/common.hpp>

namespace sysio::trace_api {
   using data_handler_function = std::function<std::tuple<fc::variant, std::optional<fc::variant>>( const std::variant<action_trace_v0>& action)>;

   namespace detail {
      class response_formatter {
      public:
         static fc::variant process_block( const data_log_entry& trace, bool irreversible, const data_handler_function& data_handler );
      };
   }

   // Serialise an action's authorization vector to {actor, permission} JSON
   // objects.  Shared by get_block (response_formatter) and the get_actions
   // / get_token_transfers handlers below.
   inline fc::variants serialize_authorizations(const std::vector<authorization_trace_v0>& auths) {
      fc::variants result;
      result.reserve(auths.size());
      for (const auto& a : auths)
         result.emplace_back(fc::mutable_variant_object()
            ("actor",      a.actor.to_string())
            ("permission", a.permission.to_string()));
      return result;
   }

   // ABI decode payload used by the shared variant builder below.  Mirrors
   // abi_data_handler::decode_result fields that end up in the HTTP response.
   struct decoded_action {
      fc::variant                params;
      std::optional<fc::variant> return_data;
      std::string                error_message;
   };

   enum class variant_shape {
      full,  // get_actions / get_block: every field on action_trace_v0
      slim,  // get_token_transfers: drops ordinals, receipt seqs, ram_deltas, resource usage
   };

   // Shared action->variant builder used by get_actions, get_token_transfers,
   // and the legacy process_block path.  Lives in request_handler.cpp so the
   // formatting logic isn't duplicated in every template instantiation.
   fc::mutable_variant_object build_action_variant(const action_trace_v0& a,
                                                   const decoded_action& decoded,
                                                   variant_shape shape);

   /**
    * Filter parameters for the get_actions endpoint.
    */
   struct action_query {
      std::optional<chain::name> receiver;      ///< filter by receiver account (any if unset)
      std::optional<chain::name> account;       ///< filter by contract/code account (any if unset)
      std::optional<chain::name> action;        ///< filter by action name (any if unset)
      uint32_t block_num_start = 0;
      uint32_t block_num_end   = std::numeric_limits<uint32_t>::max();
   };

   /**
    * Result returned by get_actions.
    */
   struct actions_result {
      fc::variants actions;
   };

   template<typename LogfileProvider, typename DataHandlerProvider>
   class request_handler {
   public:
      request_handler(LogfileProvider&& logfile_provider, DataHandlerProvider&& data_handler_provider, log_handler log)
      :logfile_provider(std::move(logfile_provider))
      ,data_handler_provider(std::move(data_handler_provider))
      ,_log(log)
      {
         _log("Constructed request_handler");
      }

      /**
       * Fetch the trace for a given block height and convert it to a fc::variant for conversion to a final format
       * (eg JSON)
       *
       * @param block_height - the height of the block whose trace is requested
       * @return a properly formatted variant representing the trace for the given block height if it exists, an
       * empty variant otherwise.
       * @throws bad_data_exception when there are issues with the underlying data preventing processing.
       */
      fc::variant get_block_trace( uint32_t block_height ) {
         auto data = logfile_provider.get_block(block_height);
         if (!data) {
            _log("No block found at block height " + std::to_string(block_height) );
            return {};
         }

         auto data_handler = [this](const std::variant<action_trace_v0>& action) -> std::tuple<fc::variant, std::optional<fc::variant>> {
            return std::visit([&](const auto& a) {
               return data_handler_provider.serialize_to_variant(a);
            }, action);
         };

         return detail::response_formatter::process_block(std::get<0>(*data), std::get<1>(*data), data_handler);
      }

      /**
       * Fetch the trace for a given transaction id and convert it to a fc::variant for conversion to a final format
       * (eg JSON)
       *
       * @param trxid - the transaction id whose trace is requested
       * @param block_height - the height of the block whose trace contains requested transaction trace
       * @return a properly formatted variant representing the trace for the given transaction id if it exists, an
       * empty variant otherwise.
       * @throws bad_data_exception when there are issues with the underlying data preventing processing.
       */
      fc::variant get_transaction_trace(chain::transaction_id_type trxid, uint32_t block_height){
         _log("get_transaction_trace called" );
         // Named local with the exact return type so the compiler can NRVO it
         // directly into the caller's slot.  Scan the raw transaction_trace_v0[]
         // for a matching id (cheap sha256 equality) and build the variant for
         // ONLY the matching trx.  The previous implementation materialised the
         // full block variant (ABI-decoding every action) and then string-matched
         // through the resulting JSON, which cost O(block) work per lookup.
         fc::variant result;

         auto data = logfile_provider.get_block(block_height);
         if (!data) {
            _log("No block found at block height " + std::to_string(block_height));
            return result;
         }

         auto data_handler = [this](const std::variant<action_trace_v0>& action) -> std::tuple<fc::variant, std::optional<fc::variant>> {
            return std::visit([&](const auto& a) {
               return data_handler_provider.serialize_to_variant(a);
            }, action);
         };
         const bool irreversible = std::get<1>(*data);

         std::visit([&](const auto& block_trace) {
            for (const auto& trx : block_trace.transactions) {
               if (trx.id == trxid) {
                  // Build a single-transaction variant by calling the shared
                  // formatter on a synthesized block containing only this trx.
                  // Avoids decoding any other transactions in the block.
                  auto single = block_trace;           // copy
                  single.transactions = {trx};
                  auto block_var = detail::response_formatter::process_block(
                     data_log_entry{single}, irreversible, data_handler);
                  auto& txs = block_var.get_object()["transactions"];
                  if (!txs.is_null() && txs.size() > 0)
                     result = txs[size_t{0}];
                  return;
               }
            }
            _log("Transaction id " + trxid.str() + " not found in block " + std::to_string(block_height));
         }, std::get<0>(*data));

         return result;
      }

      /**
       * Scan a block range for action traces matching the given filter.
       *
       * Blocks are scanned in ascending order.  Within each block, actions are visited in ascending
       * global_sequence order.  All matching actions in the (caller-clamped) range are returned;
       * the caller is responsible for capping block_num_end so that the response stays bounded.
       *
       * @param query - filter parameters
       * @return actions_result containing the matching actions
       */
      actions_result get_actions(const action_query& query) {
         return get_actions_impl(query, variant_shape::full);
      }

      /// Slim response for get_token_transfers: transfer-relevant fields only.
      /// Omits execution-tree ordinals, receipt sequences, ram_deltas, and resource usage.
      actions_result get_token_transfer_actions(const action_query& query) {
         return get_actions_impl(query, variant_shape::slim);
      }

   private:
      actions_result get_actions_impl(const action_query& query, variant_shape shape) {
         actions_result result;

         // Hoist filter state out of the hot loop: avoids re-loading the optional's discriminator and value on every
         // action comparison in the inner scan.
         const bool        has_receiver = query.receiver.has_value();
         const bool        has_account  = query.account.has_value();
         const bool        has_action   = query.action.has_value();
         const chain::name receiver_v   = has_receiver ? *query.receiver : chain::name{};
         const chain::name account_v    = has_account  ? *query.account  : chain::name{};
         const chain::name action_v     = has_action   ? *query.action   : chain::name{};

         // Reused across all transactions in all blocks: clear() keeps the vector's capacity so repeated scans of
         // trxs with similar action counts avoid per-trx allocations.
         std::vector<const action_trace_v0*> matches;

         const uint32_t end = query.block_num_end;
         for (uint32_t block_num = query.block_num_start; block_num <= end; ++block_num) {
            auto data = logfile_provider.get_block(block_num);
            if (!data) continue;

            std::visit([&](const auto& bt) {
               for (const auto& trx : bt.transactions) {
                  // Filter first, sort after.  trx.actions is stored in schedule order (how apply_context scheduled
                  // action slots), which is NOT global_sequence order when a parent action queues both inline actions
                  // and require_recipient notifications: notifications run before inlines, so the inline's
                  // global_sequence is higher than later-scheduled notifications'.  Sort the matches by
                  // global_sequence so clients see execution order, matching chain_plugin's push_transaction response.
                  // global_sequence is unique per action, so sort stability is not required.  Sorting only after
                  // filtering avoids the cost for transactions whose actions are all rejected by the filter - the
                  // common case when scanning for a specific receiver/account/action across a wide block range.
                  matches.clear();
                  for (const auto& a : trx.actions) {
                     if (has_receiver && a.receiver != receiver_v) continue;
                     if (has_account  && a.account  != account_v)  continue;
                     if (has_action   && a.action   != action_v)   continue;
                     matches.push_back(&a);
                  }
                  if (matches.empty()) continue;
                  std::ranges::sort(matches, {}, &action_trace_v0::global_sequence);

                  // Hoist per-trx variant fields so a multi-match trx doesn't repeat the checksum->hex conversion or
                  // re-read the same block-level members for each emitted action.
                  const std::string trx_id_str = trx.id.str();
                  const uint32_t    trx_block  = trx.block_num;
                  const auto&       trx_time   = trx.block_time;
                  const auto&       trx_pbid   = trx.producer_block_id;

                  for (const action_trace_v0* ap : matches) {
                     const auto& a = *ap;
                     // Decode via the provider; build the variant via the shared helper so get_actions /
                     // get_token_transfers / get_block all agree on field shapes.
                     auto dec = data_handler_provider.decode(a);
                     decoded_action da{std::move(dec.params), std::move(dec.return_data), std::move(dec.error_message)};
                     fc::mutable_variant_object av = build_action_variant(a, da, shape);
                     av("trx_id",            trx_id_str)
                       ("block_num",         trx_block)
                       ("block_time",        trx_time)
                       ("producer_block_id", trx_pbid);
                     result.actions.emplace_back(std::move(av));
                  }
               }
            }, std::get<0>(*data));
         }

         return result;
      }

      LogfileProvider logfile_provider;
      DataHandlerProvider data_handler_provider;
      log_handler _log;
   };


}
