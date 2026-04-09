#pragma once

#include <sysio/chain_plugin/account_query_db.hpp>
#include <sysio/chain_plugin/trx_retry_db.hpp>
#include <sysio/chain_plugin/trx_finality_status_processing.hpp>
#include <sysio/chain_plugin/tracked_votes.hpp>
#include <sysio/chain_plugin/get_info_db.hpp>

#include <sysio/chain/application.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/block.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/plugin_interface.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain/fixed_bytes.hpp>

#include <boost/container/flat_set.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <fc/time.hpp>

#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

namespace fc { class variant; }

namespace sysio {
   namespace chain { class abi_resolver; }

   using chain::controller;
   using std::unique_ptr;
   using std::pair;
   using namespace appbase;
   using chain::name;
   using chain::uint128_t;
   using chain::public_key_type;
   using chain::transaction;
   using chain::transaction_id_type;
   using boost::container::flat_set;
   using chain::asset;
   using chain::symbol;
   using chain::authority;
   using chain::account_name;
   using chain::action_name;
   using chain::abi_def;
   using chain::abi_serializer;
   using chain::abi_serializer_cache_builder;
   using chain::abi_resolver;
   using chain::packed_transaction;

   enum class throw_on_yield { no, yes };
   inline auto make_resolver(const controller& control, fc::microseconds abi_serializer_max_time, throw_on_yield yield_throw ) {
      return [&control, abi_serializer_max_time, yield_throw](const account_name& name) -> std::optional<abi_serializer> {
         if (name.good()) {
            const auto* accnt = control.find_account_metadata( name );
            if( accnt != nullptr ) {
               try {
                  if( abi_def abi; abi_serializer::to_abi( accnt->abi, abi ) ) {
                     return abi_serializer( std::move( abi ), abi_serializer::create_yield_function( abi_serializer_max_time ) );
                  }
               } catch( ... ) {
                  if( yield_throw == throw_on_yield::yes )
                     throw;
               }
            }
         }
         return {};
      };
   }

   template<class T>
   inline abi_resolver get_serializers_cache(const controller& db, const T& obj, const fc::microseconds& max_time) {
      return abi_resolver(abi_serializer_cache_builder(make_resolver(db, max_time, throw_on_yield::no)).add_serializers(obj).get());
   }

namespace chain_apis {
struct empty{};

struct linked_action {
   name                account;
   std::optional<name> action;
};

struct permission {
   name                                       perm_name;
   name                                       parent;
   authority                                  required_auth;
   std::optional<std::vector<linked_action>>  linked_actions;
};


// see specializations for uint64_t and double in source file
template<typename Type>
Type convert_to_type(const string& str, const string& desc) {
   try {
      return fc::variant(str).as<Type>();
   } FC_RETHROW_EXCEPTIONS(warn, "Could not convert {} string '{}' to key type.", desc, str )
}

uint64_t convert_to_type(const sysio::name &n, const string &desc);

template<>
uint64_t convert_to_type(const string& str, const string& desc);

template<>
double convert_to_type(const string& str, const string& desc);

class read_write;

class api_base {
public:
   static constexpr uint32_t max_return_items = 1000;
   static void handle_db_exhaustion();
   static void handle_bad_alloc();

protected:
   struct send_transaction_params_t {
      bool return_failure_trace = true;
      bool retry_trx = false; ///< request transaction retry on validated transaction
      std::optional<uint16_t> retry_trx_num_blocks{}; ///< if retry_trx, report trace at specified blocks from executed or lib if not specified
      chain::transaction_metadata::trx_type trx_type;
      fc::variant transaction;
   };

   template<class API, class Result>
   static void send_transaction_gen(API& api, send_transaction_params_t params, chain::plugin_interface::next_function<Result> next);
};

class read_only : public api_base {
   const controller& db;
   const std::optional<get_info_db>& gidb;
   const std::optional<account_query_db>& aqdb;
   std::optional<tracked_votes>& last_tracked_votes;
   const fc::microseconds abi_serializer_max_time;
   const fc::microseconds http_max_response_time;
   bool  shorten_abi_errors = true;
   const trx_finality_status_processing* trx_finality_status_proc;
   friend class api_base;

public:
   static const string KEYi64;

   read_only(const controller&                      db,
             const std::optional<get_info_db>&      gidb,
             const std::optional<account_query_db>& aqdb,
             std::optional<tracked_votes>&          last_tracked_votes, // tracking_enabled of last_tracked_votes is set after it is constructed. const cannot be used here.
             const fc::microseconds&                abi_serializer_max_time,
             const fc::microseconds&                http_max_response_time,
             const trx_finality_status_processing*  trx_finality_status_proc)
      : db(db)
      , gidb(gidb)
      , aqdb(aqdb)
      , last_tracked_votes(last_tracked_votes)
      , abi_serializer_max_time(abi_serializer_max_time)
      , http_max_response_time(http_max_response_time)
      , trx_finality_status_proc(trx_finality_status_proc) {
   }

   void validate() const {}

   // return deadline for call
   fc::time_point start() const {
      validate();
      return fc::time_point::now().safe_add(http_max_response_time);
   }

   void set_shorten_abi_errors( bool f ) { shorten_abi_errors = f; }

   void set_tracked_votes_tracking_enabled( bool flag ) {
      if (last_tracked_votes) {
         last_tracked_votes->set_tracking_enabled(flag);
      }
   }

   using get_info_params = empty;

   get_info_db::get_info_results get_info(const get_info_params&, const fc::time_point& deadline) const;

   struct get_transaction_status_params {
      chain::transaction_id_type           id;
   };

   struct get_transaction_status_results {
      string                               state;
      std::optional<uint32_t>              block_number;
      std::optional<chain::block_id_type>  block_id;
      std::optional<fc::time_point>        block_timestamp;
      std::optional<fc::time_point>        expiration;
      uint32_t                             head_number = 0;
      chain::block_id_type                 head_id;
      fc::time_point                       head_timestamp;
      uint32_t                             irreversible_number = 0;
      chain::block_id_type                 irreversible_id;
      fc::time_point                       irreversible_timestamp;
      chain::block_id_type                 earliest_tracked_block_id;
      uint32_t                             earliest_tracked_block_number = 0;
   };
   get_transaction_status_results get_transaction_status(const get_transaction_status_params& params, const fc::time_point& deadline) const;


   struct get_activated_protocol_features_params {
      std::optional<uint32_t>  lower_bound;
      std::optional<uint32_t>  upper_bound;
      uint32_t                 limit = std::numeric_limits<uint32_t>::max(); // ignored
      bool                     search_by_block_num = false;
      bool                     reverse = false;
      std::optional<uint32_t>  time_limit_ms; // ignored
   };

   struct get_activated_protocol_features_results {
      fc::variants             activated_protocol_features;
      std::optional<uint32_t>  more;
   };

   get_activated_protocol_features_results
   get_activated_protocol_features( const get_activated_protocol_features_params& params, const fc::time_point& deadline )const;

   struct producer_info {
      name                       producer_name;
   };

   // account_resource_info holds similar data members as in account_resource_limit, but decoupling making them independently to be refactored in future
   struct account_resource_info {
      int64_t used = 0;
      int64_t available = 0;
      int64_t max = 0;
      std::optional<chain::block_timestamp_type> last_usage_update_time;    // optional for backward nodeop support
      std::optional<int64_t> current_used;  // optional for backward nodeop support
      void set( const sysio::chain::resource_limits::account_resource_limit& arl)
      {
         used = arl.used;
         available = arl.available;
         max = arl.max;
         last_usage_update_time = arl.last_usage_update_time;
         current_used = arl.current_used;
      }
   };

   struct get_account_results {
      name                       account_name;
      uint32_t                   head_block_num = 0;
      fc::time_point             head_block_time;

      bool                       privileged = false;
      fc::time_point             last_code_update;

      std::optional<asset>       core_liquid_balance;

      int64_t                    ram_quota  = 0;
      int64_t                    net_weight = 0;
      int64_t                    cpu_weight = 0;

      account_resource_info      net_limit;
      account_resource_info      cpu_limit;
      int64_t                    ram_usage = 0;

      vector<permission>         permissions;

      fc::variant                total_resources;

      std::optional<sysio::chain::resource_limits::account_resource_limit> subjective_cpu_bill_limit;
      std::vector<linked_action> sysio_any_linked_actions;
   };

   struct get_account_params {
      name                  account_name;
      std::optional<symbol> expected_core_symbol;
   };
   using get_account_return_t = std::function<chain::t_or_exception<get_account_results>()>;
   get_account_return_t get_account( const get_account_params& params, const fc::time_point& deadline )const;


   struct get_code_results {
      name                   account_name;
      string                 wast;
      string                 wasm;
      fc::sha256             code_hash;
      std::optional<abi_def> abi;
   };

   struct get_code_params {
      name account_name;
      bool code_as_wasm = true;
   };

   struct get_code_hash_results {
      name                   account_name;
      fc::sha256             code_hash;
   };

   struct get_code_hash_params {
      name account_name;
   };

   struct get_abi_results {
      name                   account_name;
      std::optional<abi_def> abi;
   };

   struct get_abi_params {
      name account_name;
   };

   struct get_raw_code_and_abi_results {
      name                   account_name;
      chain::blob            wasm;
      chain::blob            abi;
   };

   struct get_raw_code_and_abi_params {
      name                   account_name;
   };

   struct get_raw_abi_params {
      name                      account_name;
      std::optional<fc::sha256> abi_hash;
   };

   struct get_raw_abi_results {
      name                       account_name;
      fc::sha256                 code_hash;
      fc::sha256                 abi_hash;
      std::optional<chain::blob> abi;
   };


   get_code_results get_code( const get_code_params& params, const fc::time_point& deadline )const;
   get_code_hash_results get_code_hash( const get_code_hash_params& params, const fc::time_point& deadline )const;
   get_abi_results get_abi( const get_abi_params& params, const fc::time_point& deadline )const;
   get_raw_code_and_abi_results get_raw_code_and_abi( const get_raw_code_and_abi_params& params, const fc::time_point& deadline)const;
   get_raw_abi_results get_raw_abi( const get_raw_abi_params& params, const fc::time_point& deadline)const;


   struct get_required_keys_params {
      fc::variant transaction;
      flat_set<public_key_type> available_keys;
   };
   struct get_required_keys_result {
      flat_set<public_key_type> required_keys;
   };

   get_required_keys_result get_required_keys( const get_required_keys_params& params, const fc::time_point& deadline)const;

   using get_transaction_id_params = transaction;
   using get_transaction_id_result = transaction_id_type;

   get_transaction_id_result get_transaction_id( const get_transaction_id_params& params, const fc::time_point& deadline)const;

   struct get_raw_block_params {
      string block_num_or_id;
   };

   chain::signed_block_ptr get_raw_block(const get_raw_block_params& params, const fc::time_point& deadline) const;

   using get_block_params = get_raw_block_params;
   std::function<chain::t_or_exception<fc::variant>()> get_block(const get_block_params& params, const fc::time_point& deadline) const;

   // call from app() thread
   abi_resolver get_block_serializers( const chain::signed_block_ptr& block, const fc::microseconds& max_time ) const;

   // call from any thread
   fc::variant convert_block( const chain::signed_block_ptr& block,
                              abi_resolver& resolver ) const;

   struct get_block_header_params {
      string block_num_or_id;
      bool include_extensions = false; // include block extensions (requires reading entire block off disk)
   };

   struct get_block_header_result {
      chain::block_id_type  id;
      fc::variant           signed_block_header;
      std::optional<chain::extensions_type> block_extensions;
   };

   get_block_header_result get_block_header(const get_block_header_params& params, const fc::time_point& deadline) const;

   struct get_block_info_params {
      uint32_t block_num = 0;
   };

   fc::variant get_block_info(const get_block_info_params& params, const fc::time_point& deadline) const;

   struct get_block_header_state_params {
      string block_num_or_id;
   };

   fc::variant get_block_header_state(const get_block_header_state_params& params, const fc::time_point& deadline) const;

   struct get_table_rows_params {
      bool                 json = true;               ///< true = ABI-decode keys and values, false = return raw hex
      name                 code;                      ///< contract account
      string               table;                     ///< table name from ABI
      string               scope;                     ///< empty = unscoped query, non-empty = scope prefix (parsed via ABI key type)
      string               find;                      ///< exact key lookup (JSON obj or hex); errors if combined with lower/upper
      string               index_name;                ///< secondary index name (e.g. "byowner") or numeric position (e.g. "2")
      string               lower_bound;               ///< inclusive lower key (JSON obj when json=true, hex when json=false)
      string               upper_bound;               ///< exclusive upper key
      uint32_t             limit = 50;                ///< max rows to return
      std::optional<bool>  reverse;                   ///< iterate in reverse
      std::optional<bool>  show_payer;                ///< include RAM payer in each row
      std::optional<uint32_t> time_limit_ms;          ///< defaults to http-max-response-time-ms
   };

   struct get_table_rows_result {
      fc::variants         rows;                      ///< array of {key: {...}, value: {...}, payer?: "..."} objects
      bool                 more = false;
      string               next_key;                  ///< scope-stripped key for pagination
   };

   using get_table_rows_return_t = std::function<chain::t_or_exception<get_table_rows_result>()>;

   get_table_rows_return_t get_table_rows( const get_table_rows_params& params, const fc::time_point& deadline )const;

   struct get_table_by_scope_params {
      name                 code; // mandatory
      name                 table; // optional, act as filter
      string               lower_bound; // lower bound of scope, optional
      string               upper_bound; // upper bound of scope, optional
      uint32_t             limit = 10;
      std::optional<bool>  reverse;
      std::optional<uint32_t> time_limit_ms; // defaults to http-max-response-time-ms
   };
   struct get_table_by_scope_result_row {
      name        code;
      name        scope;
      string      table;
   };
   struct get_table_by_scope_result {
      vector<get_table_by_scope_result_row> rows;
      string      more; ///< fill lower_bound with this value to fetch more rows
   };

   get_table_by_scope_result get_table_by_scope( const get_table_by_scope_params& params, const fc::time_point& deadline )const;

   struct get_currency_balance_params {
      name                  code;
      name                  account;
      std::optional<string> symbol;
   };

   vector<asset> get_currency_balance( const get_currency_balance_params& params, const fc::time_point& deadline )const;

   struct get_currency_stats_params {
      name           code;
      string         symbol;
   };


   struct get_currency_stats_result {
      asset          supply;
      asset          max_supply;
      account_name   issuer;
   };

   fc::variant get_currency_stats( const get_currency_stats_params& params, const fc::time_point& deadline )const;

   struct get_finalizer_info_params {
   };

   struct get_finalizer_info_result {
      fc::variant                            active_finalizer_policy;  // current active policy
      fc::variant                            pending_finalizer_policy; // current pending policy. Empty if not existing

      // Last tracked vote information for each of the finalizers in
      // active_finalizer_policy and pending_finalizer_policy.
      // if a finalizer votes on both active_finalizer_policy and pending_finalizer_policy,
      // the vote information on pending_finalizer_policy is used.
      std::vector<tracked_votes::vote_info>  last_tracked_votes;
   };

   get_finalizer_info_result get_finalizer_info( const get_finalizer_info_params& params, const fc::time_point& deadline )const;

   struct get_producers_params {
      bool        json = false;
      string      lower_bound;
      uint32_t    limit = 50;
      std::optional<uint32_t> time_limit_ms; // defaults to http-max-response-time-ms
   };

   struct get_producers_result {
      fc::variants        rows; ///< one row per item, either encoded as hex string or JSON object
      double              total_producer_vote_weight;
      string              more; ///< fill lower_bound with this value to fetch more rows
   };

   get_producers_result get_producers( const get_producers_params& params, const fc::time_point& deadline )const;

   struct get_producer_schedule_params {
   };

   struct get_producer_schedule_result {
      fc::variant active;
      fc::variant pending;
   };

   get_producer_schedule_result get_producer_schedule( const get_producer_schedule_params& params, const fc::time_point& deadline )const;

   struct compute_transaction_results {
       chain::transaction_id_type  transaction_id;
       fc::variant                 processed; // "processed" is expected JSON for trxs in clio
    };

   struct compute_transaction_params {
      fc::variant transaction;
   };

   void compute_transaction(compute_transaction_params params, chain::plugin_interface::next_function<compute_transaction_results> next );

   struct send_read_only_transaction_results {
      chain::transaction_id_type  transaction_id;
      fc::variant                 processed;
   };
   struct send_read_only_transaction_params {
      fc::variant transaction;
   };
   void send_read_only_transaction(send_read_only_transaction_params params, chain::plugin_interface::next_function<send_read_only_transaction_results> next );

   template<typename Function>
   void walk_key_value_table(const name& code, const name& scope, const name& table, Function f) const
   {
      const auto& d = db.db();

      // KV storage: iterate [scope:8B BE] prefix within table_id partition
      const auto table_id = chain::compute_table_id(table.to_uint64_t());
      char scope_prefix[chain::kv_scope_prefix_size];
      chain::kv_encode_be64(scope_prefix, scope.to_uint64_t());
      std::string_view scope_sv(scope_prefix, chain::kv_scope_prefix_size);

      const auto& kv_idx = d.get_index<chain::kv_index, chain::by_code_key>();
      auto itr = kv_idx.lower_bound(boost::make_tuple(code, table_id, scope_sv));

      while (itr != kv_idx.end() && itr->code == code && itr->table_id == table_id) {
         auto kv = itr->key_view();
         if (kv.size() != chain::kv_scoped_key_size ||
             memcmp(kv.data(), scope_prefix, chain::kv_scope_prefix_size) != 0) break;

         // Create a temporary key_value_object-like view for the callback
         struct kv_row_view {
            uint64_t primary_key;
            chain::name payer;
            struct { const char* _data; size_t _size; const char* data() const { return _data; } size_t size() const { return _size; } } value;
         };

         kv_row_view row;
         row.primary_key = chain::kv_decode_be64(kv.data() + chain::kv_scope_prefix_size);
         row.payer = itr->payer;
         row.value._data = itr->value.data();
         row.value._size = itr->value.size();

         if (!f(row)) break;
         ++itr;
      }
   }

   using get_accounts_by_authorizers_result = account_query_db::get_accounts_by_authorizers_result;
   using get_accounts_by_authorizers_params = account_query_db::get_accounts_by_authorizers_params;
   get_accounts_by_authorizers_result get_accounts_by_authorizers( const get_accounts_by_authorizers_params& args, const fc::time_point& deadline) const;

   chain::symbol extract_core_symbol()const;

   using get_consensus_parameters_params = empty;
   struct get_consensus_parameters_results {
     fc::variant                       chain_config; //filled as chain_config_v0, v1, etc depending on activated features
     std::optional<chain::wasm_config> wasm_config;
   };
   get_consensus_parameters_results get_consensus_parameters(const get_consensus_parameters_params&, const fc::time_point& deadline) const;
};

class read_write : public api_base {
   controller& db;
   std::optional<trx_retry_db>& trx_retry;
   const fc::microseconds abi_serializer_max_time;
   const fc::microseconds http_max_response_time;
   const bool api_accept_transactions;
   friend class api_base;

public:
   read_write(controller& db, std::optional<trx_retry_db>& trx_retry,
              const fc::microseconds& abi_serializer_max_time, const fc::microseconds& http_max_response_time,
              bool api_accept_transactions);
   void validate() const;

   // return deadline for call
   fc::time_point start() const {
      validate();
      return http_max_response_time == fc::microseconds::maximum() ? fc::time_point::maximum()
                                                                   : fc::time_point::now() + http_max_response_time;
   }

   using push_transaction_params = fc::variant_object;
   struct push_transaction_results {
      chain::transaction_id_type  transaction_id;
      fc::variant                 processed; // "processed" is expected JSON for trxs in clio
   };
   void push_transaction(const push_transaction_params& params, chain::plugin_interface::next_function<push_transaction_results> next);


   using push_transactions_params  = vector<push_transaction_params>;
   using push_transactions_results = vector<push_transaction_results>;
   void push_transactions(const push_transactions_params& params, chain::plugin_interface::next_function<push_transactions_results> next);

   using send_transaction_params = push_transaction_params;
   using send_transaction_results = push_transaction_results;
   void send_transaction(send_transaction_params params, chain::plugin_interface::next_function<send_transaction_results> next);

   struct send_transaction2_params {
      bool return_failure_trace = true;
      bool retry_trx = false; ///< request transaction retry on validated transaction
      std::optional<uint16_t> retry_trx_num_blocks{}; ///< if retry_trx, report trace at specified blocks from executed or lib if not specified
      fc::variant transaction;
   };
   void send_transaction2(send_transaction2_params params, chain::plugin_interface::next_function<send_transaction_results> next);

};

 //support for --key_types [sha256,ripemd160] and --encoding [dec/hex]
 constexpr const char i64[]       = "i64";
 constexpr const char i128[]      = "i128";
 constexpr const char i256[]      = "i256";
 constexpr const char float64[]   = "float64";
 constexpr const char float128[]  = "float128";
 constexpr const char sha256[]    = "sha256";
 constexpr const char ripemd160[] = "ripemd160";
 constexpr const char dec[]       = "dec";
 constexpr const char hex[]       = "hex";

} // namespace chain_apis

class chain_plugin : public plugin<chain_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((signature_provider_manager_plugin))

   chain_plugin();
   virtual ~chain_plugin();

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();
   void handle_sighup() override;

   chain_apis::read_write get_read_write_api(const fc::microseconds& http_max_response_time);
   chain_apis::read_only get_read_only_api(const fc::microseconds& http_max_response_time) const;

   void accept_transaction(const chain::packed_transaction_ptr& trx, chain::plugin_interface::next_function<chain::transaction_trace_ptr> next);

   // Only call this after plugin_initialize()!
   controller& chain();
   // Only call this after plugin_initialize()!
   const controller& chain() const;

   chain::chain_id_type get_chain_id() const;
   fc::microseconds get_abi_serializer_max_time() const;
   bool api_accept_transactions() const;
   // set true by other plugins if any plugin allows transactions
   bool accept_transactions() const;
   void enable_accept_transactions();
   // true if vote processing is enabled
   bool accept_votes() const;

   static void handle_guard_exception(const chain::guard_exception& e);

   bool account_queries_enabled() const;
   bool transaction_finality_status_enabled() const;

   // return variant of trace for logging, trace is modified to minimize log output
   fc::variant get_log_trx_trace(const chain::transaction_trace_ptr& trx_trace) const;
   // return variant of trx for logging, trace is modified to minimize log output
   fc::variant get_log_trx(const transaction& trx) const;

   const controller::config& chain_config() const;

private:

   unique_ptr<class chain_plugin_impl> my;
};

} // namespace sysio

FC_REFLECT( sysio::chain_apis::linked_action, (account)(action) )
FC_REFLECT( sysio::chain_apis::permission, (perm_name)(parent)(required_auth)(linked_actions) )
FC_REFLECT(sysio::chain_apis::empty, )
FC_REFLECT(sysio::chain_apis::read_only::get_transaction_status_params, (id) )
FC_REFLECT(sysio::chain_apis::read_only::get_transaction_status_results, (state)(block_number)(block_id)(block_timestamp)(expiration)(head_number)(head_id)
           (head_timestamp)(irreversible_number)(irreversible_id)(irreversible_timestamp)(earliest_tracked_block_id)(earliest_tracked_block_number) )
FC_REFLECT(sysio::chain_apis::read_only::get_activated_protocol_features_params, (lower_bound)(upper_bound)(limit)(search_by_block_num)(reverse)(time_limit_ms) )
FC_REFLECT(sysio::chain_apis::read_only::get_activated_protocol_features_results, (activated_protocol_features)(more) )
FC_REFLECT(sysio::chain_apis::read_only::get_raw_block_params, (block_num_or_id))
FC_REFLECT(sysio::chain_apis::read_only::get_block_info_params, (block_num))
FC_REFLECT(sysio::chain_apis::read_only::get_block_header_state_params, (block_num_or_id))
FC_REFLECT(sysio::chain_apis::read_only::get_block_header_params, (block_num_or_id)(include_extensions))
FC_REFLECT(sysio::chain_apis::read_only::get_block_header_result, (id)(signed_block_header)(block_extensions))

FC_REFLECT( sysio::chain_apis::read_write::push_transaction_results, (transaction_id)(processed) )
FC_REFLECT( sysio::chain_apis::read_write::send_transaction2_params, (return_failure_trace)(retry_trx)(retry_trx_num_blocks)(transaction) )

FC_REFLECT( sysio::chain_apis::read_only::get_table_rows_params, (json)(code)(table)(scope)(find)(index_name)(lower_bound)(upper_bound)(limit)(reverse)(show_payer)(time_limit_ms) )
FC_REFLECT( sysio::chain_apis::read_only::get_table_rows_result, (rows)(more)(next_key) );

FC_REFLECT( sysio::chain_apis::read_only::get_table_by_scope_params, (code)(table)(lower_bound)(upper_bound)(limit)(reverse)(time_limit_ms) )
FC_REFLECT( sysio::chain_apis::read_only::get_table_by_scope_result_row, (code)(scope)(table));
FC_REFLECT( sysio::chain_apis::read_only::get_table_by_scope_result, (rows)(more) );

FC_REFLECT( sysio::chain_apis::read_only::get_currency_balance_params, (code)(account)(symbol));
FC_REFLECT( sysio::chain_apis::read_only::get_currency_stats_params, (code)(symbol));
FC_REFLECT( sysio::chain_apis::read_only::get_currency_stats_result, (supply)(max_supply)(issuer));

FC_REFLECT_EMPTY( sysio::chain_apis::read_only::get_finalizer_info_params )
FC_REFLECT( sysio::chain_apis::read_only::get_finalizer_info_result, (active_finalizer_policy)(pending_finalizer_policy)(last_tracked_votes) );

FC_REFLECT( sysio::chain_apis::read_only::get_producers_params, (json)(lower_bound)(limit)(time_limit_ms) )
FC_REFLECT( sysio::chain_apis::read_only::get_producers_result, (rows)(total_producer_vote_weight)(more) );

FC_REFLECT_EMPTY( sysio::chain_apis::read_only::get_producer_schedule_params )
FC_REFLECT( sysio::chain_apis::read_only::get_producer_schedule_result, (active)(pending) );

FC_REFLECT( sysio::chain_apis::read_only::account_resource_info, (used)(available)(max)(last_usage_update_time)(current_used) )
FC_REFLECT( sysio::chain_apis::read_only::get_account_results,
            (account_name)(head_block_num)(head_block_time)(privileged)(last_code_update)
            (core_liquid_balance)(ram_quota)(net_weight)(cpu_weight)(net_limit)(cpu_limit)(ram_usage)(permissions)
            (total_resources)
            (subjective_cpu_bill_limit) (sysio_any_linked_actions) )
// @swap code_hash
FC_REFLECT( sysio::chain_apis::read_only::get_code_results, (account_name)(code_hash)(wast)(wasm)(abi) )
FC_REFLECT( sysio::chain_apis::read_only::get_code_hash_results, (account_name)(code_hash) )
FC_REFLECT( sysio::chain_apis::read_only::get_abi_results, (account_name)(abi) )
FC_REFLECT( sysio::chain_apis::read_only::get_account_params, (account_name)(expected_core_symbol) )
FC_REFLECT( sysio::chain_apis::read_only::get_code_params, (account_name)(code_as_wasm) )
FC_REFLECT( sysio::chain_apis::read_only::get_code_hash_params, (account_name) )
FC_REFLECT( sysio::chain_apis::read_only::get_abi_params, (account_name) )
FC_REFLECT( sysio::chain_apis::read_only::get_raw_code_and_abi_params, (account_name) )
FC_REFLECT( sysio::chain_apis::read_only::get_raw_code_and_abi_results, (account_name)(wasm)(abi) )
FC_REFLECT( sysio::chain_apis::read_only::get_raw_abi_params, (account_name)(abi_hash) )
FC_REFLECT( sysio::chain_apis::read_only::get_raw_abi_results, (account_name)(code_hash)(abi_hash)(abi) )
FC_REFLECT( sysio::chain_apis::read_only::producer_info, (producer_name) )
FC_REFLECT( sysio::chain_apis::read_only::get_required_keys_params, (transaction)(available_keys) )
FC_REFLECT( sysio::chain_apis::read_only::get_required_keys_result, (required_keys) )
FC_REFLECT( sysio::chain_apis::read_only::compute_transaction_params, (transaction))
FC_REFLECT( sysio::chain_apis::read_only::compute_transaction_results, (transaction_id)(processed) )
FC_REFLECT( sysio::chain_apis::read_only::send_read_only_transaction_params, (transaction))
FC_REFLECT( sysio::chain_apis::read_only::send_read_only_transaction_results, (transaction_id)(processed) )
FC_REFLECT( sysio::chain_apis::read_only::get_consensus_parameters_results, (chain_config)(wasm_config))
