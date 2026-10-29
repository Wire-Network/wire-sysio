#include <sysio/trace_api/request_handler.hpp>

#include <algorithm>
#include <numeric>

#include <fc/container/flat.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/io/raw.hpp>
#include <fc/reflect/json_stream.hpp>
#include <fc/variant_object.hpp>

namespace sysio::trace_api {

namespace {
   /// get_actions' per-action finality marker, mirroring get_block's "status" field.
   const char* block_status(bool irreversible) {
      return irreversible ? "irreversible" : "pending";
   }
}

// trx_cpu_usage_us / trx_net_usage_words are full-shape only: they are the parent transaction's resource totals
// (action-level cpu_usage_us / net_usage are per-action and in different units: action net_usage is bytes, trx
// net_usage_words is ceil(net_usage / 8)).  Slim (get_token_transfers) omits all resource fields, so it omits the
// trx-level totals too.
void write_transaction_fields(fc::json_writer& w, const matched_transaction& trx, action_shape shape) {
   w.set("trx_id",            trx.id)
    .set("block_num",         trx.block_num)
    // FC_SERIALIZE_AS_STRING-reflected; emits the ISO date string.
    .set("block_time",        trx.block_time)
    .set("producer_block_id", trx.producer_block_id)
    .set("block_status",      block_status(trx.irreversible));
   if (shape == action_shape::full) {
      w.set("trx_cpu_usage_us",    trx.cpu_usage_us)
       .set("trx_net_usage_words", trx.net_usage_words.value);
   }
}

void write_action_fields(fc::json_writer& w, const action_trace_v0& a, action_shape shape) {
   w.set("global_sequence", a.global_sequence)
    .set("receiver",        a.receiver.to_string())
    .set("account",         a.account.to_string())
    .set("name",            a.action.to_string());
   w.key("authorization");
   w.begin_array();
   for (const auto& auth : a.authorization) {
      w.begin_object();
      w.set("actor",      auth.actor.to_string())
       .set("permission", auth.permission.to_string());
      w.end_object();
   }
   w.end_array();
   w.key("data");         w.value_hex(a.data.data(), a.data.size());
   w.key("return_value"); w.value_hex(a.return_value.data(), a.return_value.size());

   if (shape != action_shape::full)
      return;

   w.set("action_ordinal",                             a.action_ordinal.value)
    .set("creator_action_ordinal",                     a.creator_action_ordinal.value)
    .set("closest_unnotified_ancestor_action_ordinal", a.closest_unnotified_ancestor_action_ordinal.value)
    .set("recv_sequence",                              a.recv_sequence);
   // fc/container/flat.hpp emits the map as an array of [key, value] pairs, as its to_variant does.
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
}

size_t packed_size(const actions_scan& scan) {
   size_t size = 0;
   for (const auto& trx : scan.transactions)
      size += fc::raw::pack_size(trx.actions);
   return size;
}

} // namespace sysio::trace_api

// ============================================================================
// Block and transaction responses, written directly into an fc::json_writer
// with no fc::mutable_variant_object / fc::variant tree on the response path.
// ============================================================================
namespace {
   using namespace sysio::trace_api;

   std::string to_iso8601_datetime( const fc::time_point& t) {
      return t.to_iso_string() + "Z";
   }

   void write_actions(fc::json_writer& w,
                      const std::vector<action_trace_v0>& actions,
                      const stream_data_handler_function& data_handler) {
      // Sort indices by global_sequence so
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
         write_action_fields(w, a, action_shape::full);
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
          // to_iso8601_datetime appends a 'Z' suffix.
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
