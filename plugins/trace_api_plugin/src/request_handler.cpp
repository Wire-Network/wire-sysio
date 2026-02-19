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

   fc::variants process_actions(const std::vector<action_trace_v0>& actions, const data_handler_function& data_handler) {
      fc::variants result;
      result.reserve(actions.size());
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

         action_variant("return_value", fc::to_hex(a.return_value.data(), a.return_value.size()));
         auto [params, return_data] = data_handler(a);
         if (!params.is_null()) {
            action_variant("params", params);
         }
         if(return_data.has_value()){
            action_variant("return_data", *return_data);
         }

         result.emplace_back( std::move(action_variant) );
      }
      return result;
   }

   fc::variants process_transactions(const std::vector<transaction_trace_v0>& transactions, const data_handler_function& data_handler) {
      fc::variants result;
      result.reserve(transactions.size());
      for ( const auto& t: transactions) {
         result.emplace_back(
            fc::mutable_variant_object()
               ("id", t.id.str())
               ("block_num", t.block_num)
               ("block_time", t.block_time)
               ("producer_block_id", t.producer_block_id)
               ("actions", process_actions(t.actions, data_handler))
               ("status", t.status)
               ("cpu_usage_us", t.cpu_usage_us)
               ("net_usage_words", t.net_usage_words)
               ("signatures", t.signatures)
               ("transaction_header", t.trx_header)
         );
      }

      return result;
   }
}

namespace sysio::trace_api::detail {
    fc::variant response_formatter::process_block( const data_log_entry& trace, bool irreversible, const data_handler_function& data_handler ) {
       return std::visit([&](auto&& block_trace) -> fc::variant {
          return fc::mutable_variant_object()
             ("id", block_trace.id.str())
             ("number", block_trace.number)
             ("previous_id", block_trace.previous_id.str())
             ("status", irreversible ? "irreversible" : "pending")
             ("timestamp", to_iso8601_datetime(block_trace.timestamp))
             ("producer", block_trace.producer.to_string())
             ("transaction_mroot", block_trace.transaction_mroot)
             ("finality_mroot", block_trace.finality_mroot)
             ("transactions", process_transactions(block_trace.transactions, data_handler));
       }, trace);
    }
}
