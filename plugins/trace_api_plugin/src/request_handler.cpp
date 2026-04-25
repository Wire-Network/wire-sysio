#include <sysio/trace_api/request_handler.hpp>

#include <algorithm>
#include <numeric>

#include <fc/crypto/hex.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/reflect/json_stream.hpp>
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

// ============================================================================
// Streaming counterpart: write JSON directly to std::string via fc::json_writer
// ----------------------------------------------------------------------------
// Emits byte-identical JSON to process_block + fc::json::to_string, without
// constructing any fc::mutable_variant_object / fc::variant nodes on the
// response path.  Profiling showed the variant-tree build was the dominant
// allocator on trace_api HTTP queries (~41% of per-test allocations on a
// validator serving get_block).  This path skips that tree entirely.
// ============================================================================
namespace {
   using namespace sysio::trace_api;

   void write_authorizations(fc::json_writer& w,
                             const std::vector<authorization_trace_v0>& auths) {
      w.begin_array();
      for (const auto& a : auths) {
         w.begin_object();
         w.key("account");    w.value_string(a.account.to_string());
         w.key("permission"); w.value_string(a.permission.to_string());
         w.end_object();
      }
      w.end_array();
   }

   void write_actions(fc::json_writer& w,
                      const std::vector<action_trace_v0>& actions,
                      const data_handler_function& data_handler) {
      // Iteration order matches process_actions: sort indices by global_sequence so
      // clients see execution order (notifications run before the inline actions that
      // queued them, so action-slot order is not global_sequence order).
      std::vector<int> indices(actions.size());
      std::iota(indices.begin(), indices.end(), 0);
      std::sort(indices.begin(), indices.end(), [&actions](int l, int r) {
         return actions.at(l).global_sequence < actions.at(r).global_sequence;
      });

      w.begin_array();
      for (int idx : indices) {
         const auto& a = actions.at(idx);
         w.begin_object();
         w.key("global_sequence"); w.value_uint64(a.global_sequence);
         w.key("receiver");        w.value_string(a.receiver.to_string());
         w.key("account");         w.value_string(a.account.to_string());
         w.key("action");          w.value_string(a.action.to_string());
         w.key("authorization");   write_authorizations(w, a.authorization);
         w.key("data");            w.value_hex(a.data.data(), a.data.size());
         w.key("return_value");    w.value_hex(a.return_value.data(), a.return_value.size());
         // The abi decode path still produces fc::variant (structure is ABI-dependent,
         // no streaming equivalent here).  Splice its serialized form as raw JSON so
         // the action body stays in the streaming pipeline.
         auto [params, return_data] = data_handler(a);
         if (!params.is_null()) {
            w.key("params");
            w.raw_value(fc::json::to_string(params, fc::json::yield_function_t()));
         }
         if (return_data.has_value()) {
            w.key("return_data");
            w.raw_value(fc::json::to_string(*return_data, fc::json::yield_function_t()));
         }
         w.end_object();
      }
      w.end_array();
   }

   void write_transactions(fc::json_writer& w,
                           const std::vector<transaction_trace_v0>& transactions,
                           const data_handler_function& data_handler) {
      w.begin_array();
      for (const auto& t : transactions) {
         w.begin_object();
         w.key("id");                fc::to_json_stream(t.id, w);
         w.key("block_num");         w.value_uint32(t.block_num);
         // block_timestamp_type serializes as its slot (uint32) in to_variant; preserve
         // that shape so clients see identical output.
         w.key("block_time");        w.value_uint32(t.block_time.slot);
         w.key("producer_block_id"); fc::to_json_stream(t.producer_block_id, w);
         w.key("actions");           write_actions(w, t.actions, data_handler);
         // status is fc::enum_type<uint8_t, status_enum>; to_variant emits the numeric
         // underlying value.  Match that.
         w.key("status");            w.value_uint64(static_cast<uint64_t>(t.status));
         w.key("cpu_usage_us");      w.value_uint32(t.cpu_usage_us);
         w.key("net_usage_words");   w.value_uint64(t.net_usage_words.value);
         w.key("signatures");
         w.begin_array();
         for (const auto& sig : t.signatures) fc::to_json_stream(sig, w);
         w.end_array();
         // transaction_header is a reflected struct composed of fc::time_point_sec,
         // uint16/uint32 and unsigned_ints.  No native to_json_stream path yet so
         // fall back via the variant bridge for just that field.
         w.key("transaction_header");
         fc::to_json_stream_via_variant(t.trx_header, w);
         w.end_object();
      }
      w.end_array();
   }
}

namespace sysio::trace_api::detail {
   std::string response_formatter::process_block_to_json( const data_log_entry& trace, bool irreversible, const data_handler_function& data_handler ) {
      std::string out;
      fc::json_writer w(out);
      std::visit([&](auto&& bt) {
         w.begin_object();
         w.key("id");                w.value_string(bt.id.str());
         w.key("number");            w.value_uint32(bt.number);
         w.key("previous_id");       w.value_string(bt.previous_id.str());
         w.key("status");            w.value_string(irreversible ? "irreversible" : "pending");
         // to_iso8601_datetime appends a 'Z' suffix that process_block also emits.
         w.key("timestamp");         w.value_string(to_iso8601_datetime(bt.timestamp));
         w.key("producer");          w.value_string(bt.producer.to_string());
         w.key("transaction_mroot"); fc::to_json_stream(bt.transaction_mroot, w);
         w.key("finality_mroot");    fc::to_json_stream(bt.finality_mroot, w);
         w.key("transactions");      write_transactions(w, bt.transactions, data_handler);
         w.end_object();
      }, trace);
      return out;
   }
}
