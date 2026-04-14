#include <sysio/trace_api/request_handler.hpp>

#include <algorithm>

#include <fc/variant_object.hpp>

namespace sysio::trace_api {

fc::mutable_variant_object build_action_variant(const action_trace_v0& a,
                                                 const decoded_action& decoded,
                                                 variant_shape shape) {
   fc::mutable_variant_object v;
   // Fields common to all shapes.
   v("global_sequence",  a.global_sequence)
    ("receiver",         a.receiver.to_string())
    ("account",          a.account.to_string())
    ("name",             a.action.to_string())
    ("authorization",    serialize_authorizations(a.authorization))
    ("data",             fc::to_hex(a.data.data(), a.data.size()))
    ("return_value",     fc::to_hex(a.return_value.data(), a.return_value.size()));

   if (shape == variant_shape::full) {
      v("action_ordinal",                             a.action_ordinal)
       ("creator_action_ordinal",                     a.creator_action_ordinal)
       ("closest_unnotified_ancestor_action_ordinal", a.closest_unnotified_ancestor_action_ordinal)
       ("recv_sequence",                              a.recv_sequence)
       ("auth_sequence",                              a.auth_sequence)
       ("code_sequence",                              a.code_sequence)
       ("abi_sequence",                               a.abi_sequence);

      fc::variants deltas;
      deltas.reserve(a.account_ram_deltas.size());
      for (const auto& d : a.account_ram_deltas) {
         deltas.emplace_back(fc::mutable_variant_object()
            ("account", d.account.to_string())
            ("delta",   d.delta));
      }
      v("account_ram_deltas", std::move(deltas));

      if (a.cpu_usage_us.has_value())
         v("cpu_usage_us", *a.cpu_usage_us);
      if (a.net_usage.has_value())
         v("net_usage", *a.net_usage);
   }

   if (!decoded.params.is_null())
      v("params", decoded.params);
   if (decoded.return_data.has_value())
      v("return_data", *decoded.return_data);
   if (!decoded.error_message.empty())
      v("decode_error", decoded.error_message);

   return v;
}

} // namespace sysio::trace_api

namespace {
   using namespace sysio::trace_api;

   std::string to_iso8601_datetime( const fc::time_point& t) {
      return t.to_iso_string() + "Z";
   }

   fc::variants process_actions(const std::vector<action_trace_v0>& actions, const data_handler_function& data_handler) {
      fc::variants result;
      result.reserve(actions.size());
      // global_sequence is unique per action (chain invariant), so sort stability
      // is not required.
      std::vector<const action_trace_v0*> sorted;
      sorted.reserve(actions.size());
      for (const auto& a : actions) sorted.push_back(&a);
      std::sort(sorted.begin(), sorted.end(), [](const auto* l, const auto* r){
         return l->global_sequence < r->global_sequence;
      });

      for (const action_trace_v0* ap : sorted) {
         const auto& a = *ap;
         auto [params, return_data] = data_handler(a);
         decoded_action decoded{std::move(params), std::move(return_data), {}};
         // legacy process_block path used serialize_to_variant's tuple which doesn't
         // convey decode_error, so leave the field empty here; callers that want the
         // error path should go through get_actions instead.

         fc::mutable_variant_object action_variant = build_action_variant(a, decoded, variant_shape::full);
         // block-trace-local fields (populated by the enclosing transaction, not here)
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
