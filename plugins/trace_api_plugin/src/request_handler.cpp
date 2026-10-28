#include <sysio/trace_api/request_handler.hpp>

#include <algorithm>
#include <numeric>

#include <fc/container/flat.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/reflect/json_stream.hpp>
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
      // global_sequence is unique per action (chain invariant), so sort stability is not required.
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
         w.set("actor",      a.actor.to_string())
          .set("permission", a.permission.to_string());
         w.end_object();
      }
      w.end_array();
   }

   void write_actions(fc::json_writer& w,
                      const std::vector<action_trace_v0>& actions,
                      const stream_data_handler_function& data_handler) {
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
         // Field set and order mirror build_action_variant's variant_shape::full shape --
         // the streaming and variant paths must emit identical JSON (pinned by the
         // streaming_vs_variant_* parity tests).
         w.set("global_sequence", a.global_sequence)
          .set("receiver",        a.receiver.to_string())
          .set("account",         a.account.to_string())
          .set("name",            a.action.to_string());
         w.key("authorization");   write_authorizations(w, a.authorization);
         w.key("data");            w.value_hex(a.data.data(), a.data.size());
         w.key("return_value");    w.value_hex(a.return_value.data(), a.return_value.size());
         w.set("action_ordinal",                             a.action_ordinal.value)
          .set("creator_action_ordinal",                     a.creator_action_ordinal.value)
          .set("closest_unnotified_ancestor_action_ordinal", a.closest_unnotified_ancestor_action_ordinal.value)
          .set("recv_sequence",                              a.recv_sequence);
         // fc/container/flat.hpp co-locates to_json_stream(flat_map) with its to_variant
         // sibling, so the streaming and variant paths emit the same map shape by design.
         w.key("auth_sequence");
         fc::to_json_stream(a.auth_sequence, w);
         w.set("code_sequence", a.code_sequence.value)
          .set("abi_sequence",  a.abi_sequence.value);
         w.key("account_ram_deltas");
         w.begin_array();
         for (const auto& d : a.account_ram_deltas) {
            w.begin_object();
            w.set("account", d.account.to_string())
             .set("delta",   d.delta);
            w.end_object();
         }
         w.end_array();
         if (a.cpu_usage_us.has_value())
            w.set("cpu_usage_us", a.cpu_usage_us->value);
         if (a.net_usage.has_value())
            w.set("net_usage", a.net_usage->value);
         // ABI-decoded "params" / "return_data" are emitted directly into w by the streaming
         // data_handler -- no fc::variant tree, no fc::json::to_string splice.  The handler
         // is responsible for emitting zero, one, or both keys depending on what's available
         // and whether the decode succeeds; on failure it rolls back via json_writer::rewind
         // so the action object's preceding fields stay intact.
         data_handler(a, w);
         w.end_object();
      }
      w.end_array();
   }

   void write_transaction(fc::json_writer& w,
                          const transaction_trace_v0& t,
                          const stream_data_handler_function& data_handler) {
      w.begin_object();
      w.set("id",                t.id)
       .set("block_num",         t.block_num)
       // FC_SERIALIZE_AS_STRING-reflected; emits the ISO date string.
       .set("block_time",        t.block_time)
       .set("producer_block_id", t.producer_block_id);
      w.key("actions");          write_actions(w, t.actions, data_handler);
      w.set("cpu_usage_us",    t.cpu_usage_us)
       .set("net_usage_words", t.net_usage_words.value)
       .set("signatures",      t.signatures);
      // transaction_header is a reflected struct composed of fc::time_point_sec,
      // uint16/uint32 and unsigned_ints.  No native to_json_stream path yet so
      // fall back via the variant bridge for just that field.
      w.key("transaction_header");
      fc::to_json_stream_via_variant(t.trx_header, w);
      w.end_object();
   }

   void write_transactions(fc::json_writer& w,
                           const std::vector<transaction_trace_v0>& transactions,
                           const stream_data_handler_function& data_handler) {
      w.begin_array();
      for (const auto& t : transactions) {
         write_transaction(w, t, data_handler);
      }
      w.end_array();
   }
}

namespace sysio::trace_api::detail {
   void response_formatter::process_block_to_stream( const data_log_entry& trace, bool irreversible, const stream_data_handler_function& data_handler, fc::json_writer& w ) {
      std::visit([&](auto&& bt) {
         w.begin_object();
         w.set("id",                bt.id)
          .set("number",            bt.number)
          .set("previous_id",       bt.previous_id)
          .set("status",            irreversible ? "irreversible" : "pending")
          // to_iso8601_datetime appends a 'Z' suffix that process_block also emits.
          .set("timestamp",         to_iso8601_datetime(bt.timestamp))
          .set("producer",          bt.producer.to_string())
          .set("transaction_mroot", bt.transaction_mroot)
          .set("finality_mroot",    bt.finality_mroot);
         w.key("transactions");     write_transactions(w, bt.transactions, data_handler);
         w.end_object();
      }, trace);
   }

   std::string response_formatter::process_block_to_json( const data_log_entry& trace, bool irreversible, const stream_data_handler_function& data_handler ) {
      std::string out;
      fc::json_writer w(out);
      process_block_to_stream(trace, irreversible, data_handler, w);
      return out;
   }

   bool response_formatter::contains_transaction( const data_log_entry& trace, const chain::transaction_id_type& trxid ) {
      return std::visit([&](auto&& bt) {
         for (const auto& t : bt.transactions) {
            if (t.id == trxid)
               return true;
         }
         return false;
      }, trace);
   }

   bool response_formatter::process_transaction_to_stream( const data_log_entry& trace, const chain::transaction_id_type& trxid, const stream_data_handler_function& data_handler, fc::json_writer& w ) {
      return std::visit([&](auto&& bt) {
         for (const auto& t : bt.transactions) {
            if (t.id == trxid) {
               write_transaction(w, t, data_handler);
               return true;
            }
         }
         return false;
      }, trace);
   }

   std::string response_formatter::process_transaction_to_json( const data_log_entry& trace, const chain::transaction_id_type& trxid, const stream_data_handler_function& data_handler ) {
      std::string out;
      {
         fc::json_writer w(out);
         if (!process_transaction_to_stream(trace, trxid, data_handler, w))
            return {}; // miss: preserve the empty-string contract (out holds only the writer's reserve)
      }
      return out;
   }
}
