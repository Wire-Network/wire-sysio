#include <sysio/trace_api/request_handler.hpp>

#include <algorithm>

#include <fc/variant_object.hpp>

namespace {
   using namespace sysio::trace_api;

   std::string to_iso8601_datetime( const fc::time_point& t) {
      return t.to_iso_string() + "Z";
   }

   fc::variants process_authorizations(const std::vector<authorization_trace_v0>& authorizations) {
      fc::variants result;
      result.reserve(authorizations.size());
      for ( const auto& a: authorizations) {
         result.emplace_back(fc::mutable_variant_object()
            ("account", a.account.to_string())
            ("permission", a.permission.to_string())
         );
      }

      return result;

   }

   template<typename ActionTrace>
   fc::variants process_actions(const std::vector<ActionTrace>& actions, const data_handler_function & data_handler) {
      fc::variants result;
      result.reserve(actions.size());
      // create a vector of indices to sort based on actions to avoid copies
      std::vector<int> indices(actions.size());
      std::iota(indices.begin(), indices.end(), 0);
      std::sort(indices.begin(), indices.end(), [&actions](const int& lhs, const int& rhs) -> bool {
         return actions.at(lhs).global_sequence < actions.at(rhs).global_sequence;
      });
      for ( int index : indices) {
         const auto& a = actions.at(index);
         auto action_variant = fc::mutable_variant_object();

         action_variant("global_sequence", a.global_sequence)
               ("receiver", a.receiver.to_string())
               ("account", a.account.to_string())
               ("action", a.action.to_string())
               ("authorization", process_authorizations(a.authorization))
               ("data", fc::to_hex(a.data.data(), a.data.size()));

         if constexpr(std::is_same_v<ActionTrace, action_trace_v0>){
            auto [params, return_data] = data_handler(a);
            if (!params.is_null()) {
               action_variant("params", params);
            }
         }
         else if constexpr(std::is_same_v<ActionTrace, action_trace_v1>){
            action_variant("return_value", fc::to_hex(a.return_value.data(),a.return_value.size())) ;
            auto [params, return_data] = data_handler(a);
            if (!params.is_null()) {
               action_variant("params", params);
            }
            if(return_data.has_value()){
               action_variant("return_data", *return_data);
            }
         }
         result.emplace_back( std::move(action_variant) );
      }
      return result;
   }

   template<typename TransactionTrace>
   fc::variants process_transactions(const std::vector<TransactionTrace>& transactions, const data_handler_function& data_handler) {
      fc::variants result;
      result.reserve(transactions.size());
      for ( const auto& t: transactions) {
         if constexpr(std::is_same_v<TransactionTrace, transaction_trace_v0>){
            result.emplace_back(
               fc::mutable_variant_object()
                  ("id", t.id.str())
                  ("actions", process_actions<action_trace_v0>(t.actions, data_handler)));
         } else {
            auto common_mvo = fc::mutable_variant_object();
            common_mvo("status", t.status)
                  ("cpu_usage_us", t.cpu_usage_us)
                  ("net_usage_words", t.net_usage_words)
                  ("signatures", t.signatures)
                  ("transaction_header", t.trx_header);
            if constexpr(std::is_same_v<TransactionTrace, transaction_trace_v1>){
               result.emplace_back(
                  fc::mutable_variant_object()
                     ("id", t.id.str())
                     ("actions", process_actions<action_trace_v0>(t.actions, data_handler))
                     (std::move(common_mvo)));
            }
            else if constexpr(std::is_same_v<TransactionTrace, transaction_trace_v2>){
               result.emplace_back(
                  fc::mutable_variant_object()
                     ("id", t.id.str())
                     ("actions", process_actions<action_trace_v1>(std::get<std::vector<action_trace_v1>>(t.actions), data_handler))
                     (std::move(common_mvo)));
            }
            else if constexpr(std::is_same_v<TransactionTrace, transaction_trace_v3>){
               result.emplace_back(
                  fc::mutable_variant_object()
                     ("id", t.id.str())
                     ("block_num", t.block_num)
                     ("block_time", t.block_time)
                     ("producer_block_id", t.producer_block_id)
                     ("actions", process_actions<action_trace_v1>(std::get<std::vector<action_trace_v1>>(t.actions), data_handler))
                     (std::move(common_mvo))
               );
            }
         }
      }

      return result;
   }
}

namespace sysio::trace_api::detail {
    fc::variant response_formatter::process_block( const data_log_entry& trace, bool irreversible, const data_handler_function& data_handler ) {
       auto common_mvo  = std::visit([&](auto&& arg) -> fc::mutable_variant_object {
          return fc::mutable_variant_object()
             ("id", arg.id.str())
             ("number", arg.number )
             ("previous_id", arg.previous_id.str())
             ("status", irreversible ? "irreversible" : "pending" )
             ("timestamp", to_iso8601_datetime(arg.timestamp))
             ("producer", arg.producer.to_string());}, trace);
       if  (std::holds_alternative<block_trace_v0> (trace)){
          auto& block_trace = std::get<block_trace_v0>(trace);
          return  fc::mutable_variant_object()
                     (std::move(common_mvo))
                     ("transactions", process_transactions<transaction_trace_v0>(block_trace.transactions, data_handler ));
       }else if(std::holds_alternative<block_trace_v1>(trace)){
          auto& block_trace = std::get<block_trace_v1>(trace);
          return	fc::mutable_variant_object()
                (std::move(common_mvo))
                ("transaction_mroot", block_trace.transaction_mroot)
                ("action_mroot", block_trace.action_mroot)
                ("schedule_version", block_trace.schedule_version)
                ("transactions", process_transactions<transaction_trace_v1>( block_trace.transactions_v1, data_handler )) ;
       }else if(std::holds_alternative<block_trace_v2>(trace)){
          auto& block_trace = std::get<block_trace_v2>(trace);
          auto transactions = std::visit([&](auto&& arg){
             return process_transactions(arg, data_handler);
          }, block_trace.transactions);
          return	fc::mutable_variant_object()
                (std::move(common_mvo))
                ("transaction_mroot", block_trace.transaction_mroot)
                ("action_mroot", block_trace.action_mroot)
                ("schedule_version", block_trace.schedule_version)
                ("transactions", transactions) ;
       }else{
          return fc::mutable_variant_object();
       }
    }
}
