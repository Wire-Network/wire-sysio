#include <sysio/chain_api_plugin/chain_api_plugin.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/http_plugin/bind_stream.hpp>
#include <fc/time.hpp>
#include <fc/io/json.hpp>

namespace sysio {

using namespace sysio;

class chain_api_plugin_impl {
public:
   explicit chain_api_plugin_impl(controller& db)
      : db(db) {}

   controller& db;
};


chain_api_plugin::chain_api_plugin() = default;
chain_api_plugin::~chain_api_plugin() = default;

void chain_api_plugin::set_program_options(options_description&, options_description&) {}
void chain_api_plugin::plugin_initialize(const variables_map&) {}

// Only want a simple 'Invalid transaction id' if unable to parse the body
template<>
chain_apis::read_only::get_transaction_status_params
parse_params<chain_apis::read_only::get_transaction_status_params, http_params_types::params_required>(const std::string& body) {
   if (body.empty()) {
      SYS_THROW(chain::invalid_http_request, "A Request body is required");
   }

   try {
      auto v = fc::json::from_string( body ).as<chain_apis::read_only::get_transaction_status_params>();
      if( v.id == transaction_id_type() ) throw false;
      return v;
   } catch( ... ) {
      SYS_THROW(chain::invalid_http_request, "Invalid transaction id");
   }
}

// if actions.data & actions.hex_data provided, use the hex_data since only currently support unexploded data
template<>
chain_apis::read_only::get_transaction_id_params
parse_params<chain_apis::read_only::get_transaction_id_params, http_params_types::params_required>(const std::string& body) {
   if (body.empty()) {
      SYS_THROW(chain::invalid_http_request, "A Request body is required");
   }

   try {
      fc::variant trx_var =  fc::json::from_string( body );
      if( trx_var.is_object() ) {
         fc::variant_object& vo = trx_var.get_object();
         if( vo.contains("actions") && vo["actions"].is_array() ) {
            fc::mutable_variant_object mvo{vo};
            fc::variants& action_variants = mvo["actions"].get_array();
            for( auto& action_v : action_variants ) {
               if( action_v.is_object() ) {
                 fc::variant_object& action_vo = action_v.get_object();
                  if( action_vo.contains( "data" ) && action_vo.contains( "hex_data" ) ) {
                     fc::mutable_variant_object maction_vo{action_vo};
                     maction_vo["data"] = maction_vo["hex_data"];
                     action_vo = maction_vo;
                     vo = mvo;
                  } else if( action_vo.contains( "data" ) ) {
                     if( !action_vo["data"].is_string() ) {
                        SYS_THROW(chain::invalid_http_request, "Request supports only un-exploded 'data' (hex form)");
                     }
                  }
               }
               else {
                  SYS_THROW(chain::invalid_http_request, "Transaction contains invalid or empty action");
               }
            }
         }
         else {
            SYS_THROW(chain::invalid_http_request, "Transaction actions are missing or invalid");
         }
      }
      else {
         SYS_THROW(chain::invalid_http_request, "Transaction object is missing or invalid");
      }
      auto trx = trx_var.as<chain_apis::read_only::get_transaction_id_params>();
      if( trx.id() == transaction().id() ) {
         SYS_THROW(chain::invalid_http_request, "Invalid transaction object");
      }
      return trx;
   } SYS_RETHROW_EXCEPTIONS(chain::invalid_http_request, "Invalid transaction");
}

void chain_api_plugin::plugin_startup() {
   dlog( "starting chain_api_plugin" );
   my.reset(new chain_api_plugin_impl(app().get_plugin<chain_plugin>().chain()));
   auto& chain = app().get_plugin<chain_plugin>();
   auto& _http_plugin = app().get_plugin<http_plugin>();
   fc::microseconds max_response_time = _http_plugin.get_max_response_time();

   auto ro_api = chain.get_read_only_api(max_response_time);
   auto rw_api = chain.get_read_write_api(max_response_time);

   ro_api.set_shorten_abi_errors( !http_plugin::verbose_errors() );

   using ro = chain_apis::read_only;
   using rw = chain_apis::read_write;
   using cat = api_category;
   using pt = http_params_types;

   // Run get_info on http thread only
   _http_plugin.add_async_api_stream({
      bind_stream<&ro::get_info, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_info", cat::chain_ro, pt::no_params, 200),
   });

   _http_plugin.add_api_stream({
      // transaction related APIs will be posted to read_write queue after keys are recovered, they are safe to run
      // in parallel until they post to the read_write queue
      bind_stream<&ro::compute_transaction, dispatch::async>(
         _http_plugin, ro_api, "/v1/chain/compute_transaction", cat::chain_ro, pt::params_required, 200),
      bind_stream<&rw::push_transaction, dispatch::async>(
         _http_plugin, rw_api, "/v1/chain/push_transaction", cat::chain_rw, pt::params_required, 202),
      bind_stream<&rw::push_transactions, dispatch::async>(
         _http_plugin, rw_api, "/v1/chain/push_transactions", cat::chain_rw, pt::params_required, 202),
      bind_stream<&rw::send_transaction, dispatch::async>(
         _http_plugin, rw_api, "/v1/chain/send_transaction", cat::chain_rw, pt::params_required, 202),
      bind_stream<&rw::send_transaction2, dispatch::async>(
         _http_plugin, rw_api, "/v1/chain/send_transaction2", cat::chain_rw, pt::params_required, 202),
   }, appbase::exec_queue::read_only);

   _http_plugin.add_api_stream({
      bind_stream<&ro::get_activated_protocol_features, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_activated_protocol_features", cat::chain_ro, pt::possible_no_params, 200),
      // get_block_stream returns Phase-2 closure (function<t_or_exception<emit_fn>()>)
      bind_stream<&ro::get_block_stream, dispatch::post_direct>(
         _http_plugin, ro_api, "/v1/chain/get_block", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_block_info, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_block_info", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_block_header_state, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_block_header_state", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_code, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_code", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_code_hash, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_code_hash", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_consensus_parameters, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_consensus_parameters", cat::chain_ro, pt::no_params, 200),
      bind_stream<&ro::get_abi, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_abi", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_raw_code_and_abi, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_raw_code_and_abi", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_raw_abi, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_raw_abi", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_finalizer_info, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_finalizer_info", cat::chain_ro, pt::no_params, 200),
      bind_stream<&ro::get_table_by_scope, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_table_by_scope", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_currency_balance, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_currency_balance", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_currency_stats, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_currency_stats", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_producers, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_producers", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_producer_schedule, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_producer_schedule", cat::chain_ro, pt::no_params, 200),
      bind_stream<&ro::get_required_keys, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_required_keys", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_transaction_id, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_transaction_id", cat::chain_ro, pt::params_required, 200),
      // get_account returns Phase-2 closure (function<t_or_exception<get_account_results>()>)
      bind_stream<&ro::get_account, dispatch::post>(
         _http_plugin, ro_api, "/v1/chain/get_account", cat::chain_ro, pt::params_required, 200),
      // get_table_rows_stream returns Phase-2 closure
      bind_stream<&ro::get_table_rows_stream, dispatch::post_direct>(
         _http_plugin, ro_api, "/v1/chain/get_table_rows", cat::chain_ro, pt::params_required, 200),
   }, appbase::exec_queue::read_only);

   if (chain.account_queries_enabled()) {
      _http_plugin.add_async_api_stream({
         bind_stream<&ro::get_accounts_by_authorizers, dispatch::sync>(
            _http_plugin, ro_api, "/v1/chain/get_accounts_by_authorizers",
            cat::chain_ro, pt::params_required, 200),
      });
   }

   _http_plugin.add_async_api_stream({
      // chain_plugin send_read_only_transaction will post to read_exclusive queue
      bind_stream<&ro::send_read_only_transaction, dispatch::async>(
         _http_plugin, ro_api, "/v1/chain/send_read_only_transaction", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_raw_block, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_raw_block", cat::chain_ro, pt::params_required, 200),
      bind_stream<&ro::get_block_header, dispatch::sync>(
         _http_plugin, ro_api, "/v1/chain/get_block_header", cat::chain_ro, pt::params_required, 200),
   });

   if (chain.transaction_finality_status_enabled()) {
      _http_plugin.add_api_stream({
         bind_stream<&ro::get_transaction_status, dispatch::sync>(
            _http_plugin, ro_api, "/v1/chain/get_transaction_status",
            cat::chain_ro, pt::params_required, 200),
      }, appbase::exec_queue::read_only);
   }

   // Let ro_api's tracked_votes know whether chain_ro category is enabled
   // to avoid extra processing.
   bool chain_ro_enabled = _http_plugin.is_enabled(api_category::chain_ro);
   ro_api.set_tracked_votes_tracking_enabled(chain_ro_enabled);
}

void chain_api_plugin::plugin_shutdown() {}

}
