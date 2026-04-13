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
      uint64_t after_global_seq = 0;            ///< pagination cursor: skip actions with global_seq <= this
      uint32_t limit = 100;                     ///< max results per request (clamped to 1000 by the handler)
   };

   /**
    * Result returned by get_actions.
    */
   struct actions_result {
      fc::variants actions;
      bool     more = false;          ///< true if there are more results past 'limit'
      uint64_t last_global_seq = 0;   ///< global_seq of the last returned action; use as after_global_seq for the next page
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
       * Scan a block range for action traces matching the given filter and return paginated results.
       *
       * Blocks are scanned in ascending order.  Within each block, actions are visited in ascending
       * global_sequence order.  Scanning stops as soon as 'limit' matching actions are found, and
       * 'more' is set to true to signal that the caller should paginate.
       *
       * Use the returned 'last_global_seq' as 'after_global_seq' on the next call to continue from
       * where this call left off.
       *
       * @param query - filter and pagination parameters
       * @return actions_result containing the matching actions and pagination state
       */
      actions_result get_actions(const action_query& query) {
         actions_result result;

         const uint32_t end = query.block_num_end;
         for (uint32_t block_num = query.block_num_start; block_num <= end; ++block_num) {
            auto data = logfile_provider.get_block(block_num);
            if (!data) continue;

            std::visit([&](const auto& bt) {
               for (const auto& trx : bt.transactions) {
                  // actions within a transaction are already in global_sequence order after
                  // process_actions sorts them, but the raw vector may not be — sort here
                  std::vector<const action_trace_v0*> sorted;
                  sorted.reserve(trx.actions.size());
                  for (const auto& a : trx.actions)
                     sorted.push_back(&a);
                  std::sort(sorted.begin(), sorted.end(), [](const auto* l, const auto* r){
                     return l->global_sequence < r->global_sequence;
                  });

                  for (const action_trace_v0* ap : sorted) {
                     const auto& a = *ap;
                     if (a.global_sequence <= query.after_global_seq) continue;
                     if (query.receiver && a.receiver != *query.receiver) continue;
                     if (query.account  && a.account  != *query.account)  continue;
                     if (query.action   && a.action   != *query.action)   continue;

                     auto action_var = fc::mutable_variant_object()
                        ("global_sequence",  a.global_sequence)
                        ("receiver",         a.receiver.to_string())
                        ("account",          a.account.to_string())
                        ("action",           a.action.to_string())
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

                     result.last_global_seq = a.global_sequence;
                     result.actions.push_back(std::move(action_var));

                     if (result.actions.size() >= query.limit) {
                        result.more = true;
                        return;   // exits the lambda; outer loop will also check result.more
                     }
                  }
                  if (result.more) return;
               }
            }, std::get<0>(*data));

            if (result.more) break;
         }

         return result;
      }

   private:
      LogfileProvider logfile_provider;
      DataHandlerProvider data_handler_provider;
      log_handler _log;
   };


}
