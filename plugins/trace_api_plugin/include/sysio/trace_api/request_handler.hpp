#pragma once

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
         fc::variant result = {};
         // extract the transaction trace from the block trace
         auto resp = get_block_trace(block_height);
         if (!resp.is_null()) {
            auto& b_mvo = resp.get_object();
            if (b_mvo.contains("transactions")) {
               auto& transactions = b_mvo["transactions"];
               std::string input_id = trxid.str();
               for (uint32_t i = 0; i < transactions.size(); ++i) {
                  if (transactions[i].is_null()) continue;
                  auto& t_mvo = transactions[i].get_object();
                  if (t_mvo.contains("id")) {
                     const auto& t_id = t_mvo["id"].get_string();
                     if (t_id == input_id) {
                        result = transactions[i];
                        break;
                     }
                  }
               }
               if( result.is_null() )
                  _log("Exhausted all " + std::to_string(transactions.size()) + " transactions in block " + b_mvo["number"].as_string() + " without finding trxid " + trxid.str());
            }
         }
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
         return get_actions_impl(query, [this](const action_trace_v0& a,
                                               const transaction_trace_v0& trx) {
            auto action_var = fc::mutable_variant_object()
               ("action_ordinal",                             a.action_ordinal)
               ("creator_action_ordinal",                     a.creator_action_ordinal)
               ("closest_unnotified_ancestor_action_ordinal", a.closest_unnotified_ancestor_action_ordinal)
               ("global_sequence",                            a.global_sequence)
               ("recv_sequence",                              a.recv_sequence)
               ("auth_sequence",                              a.auth_sequence)
               ("code_sequence",                              a.code_sequence)
               ("abi_sequence",                               a.abi_sequence)
               ("receiver",                                   a.receiver.to_string())
               ("account",                                    a.account.to_string())
               ("name",                                       a.action.to_string())
               ("authorization",                              serialize_authorizations(a.authorization))
               ("data",                                       a.data.empty() ? "" : fc::to_hex(a.data.data(), a.data.size()))
               ("return_value",                               a.return_value.empty() ? "" : fc::to_hex(a.return_value.data(), a.return_value.size()))
               ("trx_id",                                     trx.id.str())
               ("block_num",                                  trx.block_num)
               ("block_time",                                 trx.block_time)
               ("producer_block_id",                          trx.producer_block_id);

            // account_ram_deltas
            {
               fc::variants deltas;
               deltas.reserve(a.account_ram_deltas.size());
               for (const auto& d : a.account_ram_deltas)
                  deltas.emplace_back(fc::mutable_variant_object()
                     ("account", d.account.to_string())
                     ("delta",   d.delta));
               action_var("account_ram_deltas", std::move(deltas));
            }

            if (a.cpu_usage_us.has_value())
               action_var("cpu_usage_us", *a.cpu_usage_us);
            if (a.net_usage.has_value())
               action_var("net_usage", *a.net_usage);

            auto [params, return_data] = data_handler_provider.serialize_to_variant(a);
            if (!params.is_null())
               action_var("params", params);
            if (return_data.has_value())
               action_var("return_data", *return_data);

            return action_var;
         });
      }

      /// Slim response for get_token_transfers: transfer-relevant fields only.
      /// Omits execution-tree ordinals, receipt sequences, ram_deltas, and resource usage.
      actions_result get_token_transfer_actions(const action_query& query) {
         return get_actions_impl(query, [this](const action_trace_v0& a,
                                               const transaction_trace_v0& trx) {
            auto action_var = fc::mutable_variant_object()
               ("global_sequence",  a.global_sequence)
               ("receiver",         a.receiver.to_string())
               ("account",          a.account.to_string())
               ("name",             a.action.to_string())
               ("authorization",    serialize_authorizations(a.authorization))
               ("data",             a.data.empty() ? "" : fc::to_hex(a.data.data(), a.data.size()))
               ("return_value",     a.return_value.empty() ? "" : fc::to_hex(a.return_value.data(), a.return_value.size()))
               ("trx_id",           trx.id.str())
               ("block_num",        trx.block_num)
               ("block_time",       trx.block_time)
               ("producer_block_id", trx.producer_block_id);

            auto [params, return_data] = data_handler_provider.serialize_to_variant(a);
            if (!params.is_null())
               action_var("params", params);
            if (return_data.has_value())
               action_var("return_data", *return_data);

            return action_var;
         });
      }

   private:
      static fc::variants serialize_authorizations(const std::vector<authorization_trace_v0>& auths) {
         fc::variants result;
         result.reserve(auths.size());
         for (const auto& a : auths)
            result.emplace_back(fc::mutable_variant_object()
               ("actor",      a.actor.to_string())
               ("permission", a.permission.to_string()));
         return result;
      }

      template<typename ActionVariantBuilder>
      actions_result get_actions_impl(const action_query& query, ActionVariantBuilder&& build_action_var) {
         actions_result result;

         const uint32_t end = query.block_num_end;
         for (uint32_t block_num = query.block_num_start; block_num <= end; ++block_num) {
            auto data = logfile_provider.get_block(block_num);
            if (!data) continue;

            std::visit([&](const auto& bt) {
               for (const auto& trx : bt.transactions) {
                  // trx.actions is stored in schedule order (how the chain's apply_context
                  // scheduled action slots), which is NOT global_sequence order when an
                  // action queues both inline actions and require_recipient notifications:
                  // notifications run before inlines, so the inline's global_sequence is
                  // higher than later-scheduled notifications'.  Sort pointers by
                  // global_sequence so clients always see execution order (matches
                  // chain_plugin's push_transaction and the legacy get_block response).
                  std::vector<const action_trace_v0*> sorted;
                  sorted.reserve(trx.actions.size());
                  for (const auto& a : trx.actions)
                     sorted.push_back(&a);
                  std::sort(sorted.begin(), sorted.end(), [](const auto* l, const auto* r){
                     return l->global_sequence < r->global_sequence;
                  });

                  for (const action_trace_v0* ap : sorted) {
                     const auto& a = *ap;
                     if (query.receiver && a.receiver != *query.receiver) continue;
                     if (query.account  && a.account  != *query.account)  continue;
                     if (query.action   && a.action   != *query.action)   continue;

                     result.actions.push_back(build_action_var(a, trx));
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
