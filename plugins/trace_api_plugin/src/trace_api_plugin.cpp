#include <sysio/trace_api/trace_api_plugin.hpp>

#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/trace_api/chain_extraction.hpp>
#include <sysio/trace_api/logging.hpp>
#include <sysio/trace_api/request_handler.hpp>
#include <sysio/trace_api/store_provider.hpp>

#include <sysio/trace_api/configuration_utils.hpp>

#include <sysio/resource_monitor_plugin/resource_monitor_plugin.hpp>

#include <boost/signals2/connection.hpp>

#include <algorithm>

using namespace sysio::trace_api;
using namespace sysio::trace_api::configuration_utils;
using namespace sysio::chain::literals;
using boost::signals2::scoped_connection;

namespace {

   std::string to_detail_string(const std::exception_ptr& e) {
      try {
         std::rethrow_exception(e);
      } catch (fc::exception& er) {
         return er.to_detail_string();
      } catch (const std::exception& e) {
         fc::exception fce(
               FC_LOG_MESSAGE(warn, "std::exception: {}: ", e.what()),
               fc::std_exception_code,
               BOOST_CORE_TYPEID(e).name(),
               e.what());
         return fce.to_detail_string();
      } catch (...) {
         fc::unhandled_exception ue(
               FC_LOG_MESSAGE(warn, "unknown: ",),
               std::current_exception());
         return ue.to_detail_string();
      }
   }

   void log_exception( const exception_with_context& e, fc::log_level level ) {
      if( _log.is_enabled( level ) ) {
         const auto&[ex_ptr, file, line, func] = e;
         auto detail_string = to_detail_string(ex_ptr);
         auto& logger = _log.get_agent_logger();
         logger->log(spdlog::source_loc{file, static_cast<int>(line), func}, logger->level(), FC_FMT( "{}", detail_string ));
      }
   }

   /**
    * The exception_handler provided to the extraction sub-system throws `yield_exception` as a signal that
    * Something has gone wrong and the extraction process needs to terminate immediately
    *
    * This templated method is used to wrap signal handlers for `chain_controller` so that the plugin-internal
    * `yield_exception` can be translated to a `chain::controller_emit_signal_exception`.
    *
    * The goal is that the currently applied block will be rolled-back before the shutdown takes effect leaving
    * the system in a better state for restart.
    */
   template<typename F>
   void emit_killer(F&& f) {
      try {
         f();
      } catch (const yield_exception& ) {
         SYS_THROW(chain::controller_emit_signal_exception, "Trace API encountered an Error which it cannot recover from.  Please resolve the error and relaunch the process")
      }
   }

   template<typename Store>
   struct shared_store_provider {
      explicit shared_store_provider(const std::shared_ptr<Store>& store)
      :store(store)
      {}

      template <typename BlockTrace>
      void append( const BlockTrace& trace ) {
         store->append(trace);
      }

      void append_lib( uint32_t new_lib ) {
         store->append_lib(new_lib);
      }

      get_block_t get_block(uint32_t height) {
         return store->get_block(height);
      }

      void append_trx_ids(block_trxs_entry tt){
         store->append_trx_ids(std::move(tt));
      }

      std::optional<std::pair<uint32_t,uint32_t>> first_and_last_recorded_blocks() const {
         return store->first_and_last_recorded_blocks();
      }

      void append_abi(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes) {
         store->append_abi(account, global_seq, std::move(abi_bytes));
      }

      std::optional<abi_log::lookup_result> lookup_abi(chain::name account, uint64_t global_seq) const {
         return store->lookup_abi(account, global_seq);
      }

      bool has_abi_entry(chain::name account) const {
         return store->has_abi_entry(account);
      }

      uint32_t slice_stride() const noexcept {
         return store->slice_stride();
      }

      uint32_t slice_number(uint32_t block_height) const noexcept {
         return store->slice_number(block_height);
      }

      bloom_reader get_bloom(uint32_t slice_number) const {
         return store->get_bloom(slice_number);
      }

      std::shared_ptr<Store> store;
   };
}

namespace sysio {

/**
 * A common source for information shared between the extraction process and the RPC process
 */
struct trace_api_common_impl {
   static void set_program_options(appbase::options_description& cli, appbase::options_description& cfg) {
      auto cfg_options = cfg.add_options();
      cfg_options("trace-dir", bpo::value<std::filesystem::path>()->default_value("traces"),
                  "the location of the trace directory (absolute path or relative to application data dir)");
      cfg_options("trace-slice-stride", bpo::value<uint32_t>()->default_value(10'000),
                  "Number of blocks each \"slice\" of trace data will contain on the filesystem.\n"
                  "Must be in the range [1, 1000000].  Larger values reduce file count but bloat the\n"
                  "block-offset sidecar pre-allocation and stress the per-slice trx_id hash index.");
      cfg_options("trace-minimum-irreversible-history-blocks", boost::program_options::value<int32_t>()->default_value(-1),
                  "Number of blocks to ensure are kept past LIB for retrieval before \"slice\" files can be automatically removed.\n"
                  "A value of -1 indicates that automatic removal of \"slice\" files will be turned off.");
      cfg_options("trace-minimum-uncompressed-irreversible-history-blocks", boost::program_options::value<int32_t>()->default_value(-1),
                  "Number of blocks to ensure are uncompressed past LIB. Compressed \"slice\" files are still accessible but may carry a performance loss on retrieval\n"
                  "A value of -1 indicates that automatic compression of \"slice\" files will be turned off.");
      cfg_options("trace-max-block-range", bpo::value<uint32_t>()->default_value(1000),
                  "Maximum number of blocks scanned by a single get_actions or get_token_transfers request.\n"
                  "Must be in [1, 10000]. block_num_end is silently clamped to block_num_start + this - 1.\n"
                  "Clients paginate by advancing block_num_start by this amount each call. The response\n"
                  "envelope reports the actual range scanned.");
   }

   void plugin_initialize(const appbase::variables_map& options) {
      auto dir_option = options.at("trace-dir").as<std::filesystem::path>();
      if (dir_option.is_relative())
         trace_dir = app().data_dir() / dir_option;
      else
         trace_dir = dir_option;
      if (auto resmon_plugin = app().find_plugin<resource_monitor_plugin>())
        resmon_plugin->monitor_directory(trace_dir);

      slice_stride = options.at("trace-slice-stride").as<uint32_t>();
      SYS_ASSERT(slice_stride >= 1 && slice_stride <= 1'000'000, chain::plugin_config_exception,
                 "\"trace-slice-stride\" must be in [1, 1000000]; got {}", slice_stride);

      const int32_t blocks = options.at("trace-minimum-irreversible-history-blocks").as<int32_t>();
      SYS_ASSERT(blocks >= -1, chain::plugin_config_exception,
                 "\"trace-minimum-irreversible-history-blocks\" must be greater to or equal to -1.");
      if (blocks > manual_slice_file_value) {
         minimum_irreversible_history_blocks = blocks;
      }

      const int32_t uncompressed_blocks = options.at("trace-minimum-uncompressed-irreversible-history-blocks").as<int32_t>();
      SYS_ASSERT(uncompressed_blocks >= -1, chain::plugin_config_exception,
                 "\"trace-minimum-uncompressed-irreversible-history-blocks\" must be greater to or equal to -1.");

      if (uncompressed_blocks > manual_slice_file_value) {
         minimum_uncompressed_irreversible_history_blocks = uncompressed_blocks;
      }

      const uint32_t block_range = options.at("trace-max-block-range").as<uint32_t>();
      SYS_ASSERT(block_range >= 1 && block_range <= 10'000, chain::plugin_config_exception,
                 "\"trace-max-block-range\" must be in [1, 10000]; got {}", block_range);
      max_block_range = block_range;

      store = std::make_shared<store_provider>(
         trace_dir,
         slice_stride,
         minimum_irreversible_history_blocks,
         minimum_uncompressed_irreversible_history_blocks,
         compression_seek_point_stride
      );
   }

   void plugin_startup() {
      store->start_maintenance_thread([](const std::string& msg ){
         fc_dlog( _log, "{}", msg );
      });
   }

   void plugin_shutdown() {
      store->stop_maintenance_thread();
   }

   // common configuration paramters
   std::filesystem::path trace_dir;
   uint32_t slice_stride = 0;

   std::optional<uint32_t> minimum_irreversible_history_blocks;
   std::optional<uint32_t> minimum_uncompressed_irreversible_history_blocks;

   static constexpr int32_t manual_slice_file_value = -1;
   static constexpr uint32_t compression_seek_point_stride = 6 * 1024 * 1024; // 6 MiB strides for clog seek points

   uint32_t max_block_range = 100;
   std::shared_ptr<store_provider> store;
};

/**
 * Interface with the RPC process
 */
struct trace_api_rpc_plugin_impl : public std::enable_shared_from_this<trace_api_rpc_plugin_impl>
{
   explicit trace_api_rpc_plugin_impl( const std::shared_ptr<trace_api_common_impl>& common )
   :common(common) {}

   void plugin_initialize(const appbase::variables_map&) {
      fc_ilog(_log, "trace_api: initializing trace api rpc plugin");
      max_block_range = common->max_block_range;
      auto store = common->store;
      auto data_handler = std::make_shared<abi_data_handler>(
         [](const exception_with_context& e) {
            // Log at debug and fall back to raw hex -- do not rethrow, since
            // ABI capture is automatic and decoding failures should be soft.
            log_exception(e, fc::log_level::debug);
         },
         [store](chain::name account, uint64_t global_seq) -> std::optional<abi_data_handler::lookup_entry> {
            std::optional<abi_data_handler::lookup_entry> out;
            if (auto r = store->lookup_abi(account, global_seq)) {
               out.emplace(abi_data_handler::lookup_entry{r->effective_global_seq, std::move(r->abi_bytes)});
            }
            return out;
         }
      );

      req_handler = std::make_shared<request_handler_t>(
         shared_store_provider<store_provider>(common->store),
         abi_data_handler::shared_provider(data_handler),
         [](const std::string& msg ) {
            fc_dlog( _log, "{}", msg );
         }
      );
   }

   void plugin_startup() {
      auto& http = app().get_plugin<http_plugin>();

      http.add_async_handler({"/v1/trace_api/get_block",
            api_category::trace_api,
            [this](std::string, std::string body, url_response_callback cb)
      {
         auto block_number = ([&body]() -> std::optional<uint32_t> {
            if (body.empty()) {
               return {};
            }

            try {
               auto input = fc::json::from_string(body);
               auto block_num = input.get_object()["block_num"].as_uint64();
               if (block_num > std::numeric_limits<uint32_t>::max()) {
                  return {};
               }
               return block_num;
            } catch (...) {
               return {};
            }
         })();

         if (!block_number) {
            error_results results{400, "Bad or missing block_num"};
            cb( 400, fc::variant( results ));
            return;
         }

         try {

            auto resp = req_handler->get_block_trace(*block_number);
            if (resp.is_null()) {
               error_results results{404, "Trace API: block trace missing"};
               cb( 404, fc::variant( results ));
            } else {
               cb( 200, std::move(resp) );
            }
         } catch (...) {
            http_plugin::handle_exception("trace_api", "get_block", body, cb);
         }
      }});


      http.add_async_handler({"/v1/trace_api/get_transaction_trace",
            api_category::trace_api,
            [this](std::string, std::string body, url_response_callback cb)
      {
         auto trx_id = ([&body]() -> std::optional<transaction_id_type> {
            if (body.empty()) {
               return {};
            }
            try {
               auto input = fc::json::from_string(body);
               auto trxid = input.get_object()["id"].as_string();
               if (trxid.size() < 8 || trxid.size() > 64) {
                  return {};
               }
               return transaction_id_type(trxid);
            } catch (...) {
               return {};
            }
         })();

         if (!trx_id) {
            error_results results{400, "Bad or missing transaction ID"};
            cb( 400, fc::variant( results ));
            return;
         }

         try {
            // search for the block that contains the transaction
            get_block_n blk_num = common->store->get_trx_block_number(*trx_id);
            if (!blk_num.has_value()){
               error_results results{404, "Trace API: transaction id missing in the transaction id log files"};
               cb( 404, fc::variant( results ));
            } else {
               auto resp = req_handler->get_transaction_trace(*trx_id, *blk_num);
               if (resp.is_null()) {
                  error_results results{404, "Trace API: transaction trace missing"};
                  cb( 404, fc::variant( results ));
               } else {
                  cb( 200, std::move(resp) );
               }
            }
          } catch (...) {
             http_plugin::handle_exception("trace_api", "get_transaction", body, cb);
          }
      }});

      http.add_async_handler({"/v1/trace_api/get_actions",
            api_category::trace_api,
            [this](std::string, std::string body, url_response_callback cb)
      {
         action_query query;
         bool include_notifications = false;
         if (!body.empty()) {
            try {
               auto input = fc::json::from_string(body);
               const auto& obj = input.get_object();
               if (obj.contains("receiver"))
                  query.receiver = chain::name(obj["receiver"].as_string());
               if (obj.contains("account"))
                  query.account = chain::name(obj["account"].as_string());
               if (obj.contains("action"))
                  query.action = chain::name(obj["action"].as_string());
               if (obj.contains("block_num_start"))
                  query.block_num_start = obj["block_num_start"].as<uint32_t>();
               if (obj.contains("block_num_end"))
                  query.block_num_end = obj["block_num_end"].as<uint32_t>();
               if (obj.contains("include_notifications"))
                  include_notifications = obj["include_notifications"].as_bool();
            } catch (...) {
               error_results results{400, "Bad request body"};
               cb( 400, fc::variant( results ));
               return;
            }
         }

         // Default = canonical actions only (receiver == account).  When the caller
         // specifies exactly one of receiver/account and does not opt in to
         // notifications, mirror the specified value onto the missing one.
         if (!include_notifications) {
            if (query.account && !query.receiver)
               query.receiver = query.account;
            else if (query.receiver && !query.account)
               query.account = query.receiver;
         }

         if (query.block_num_start > query.block_num_end) {
            error_results results{400, "block_num_start must be <= block_num_end"};
            cb( 400, fc::variant( results ));
            return;
         }

         clamp_block_end(query);

         try {
            auto result = req_handler->get_actions(query);
            cb( 200, fc::mutable_variant_object()
                        ("block_num_start", query.block_num_start)
                        ("block_num_end",   query.block_num_end)
                        ("actions",         result.actions) );
         } catch (...) {
            http_plugin::handle_exception("trace_api", "get_actions", body, cb);
         }
      }});

      http.add_async_handler({"/v1/trace_api/get_token_transfers",
            api_category::trace_api,
            [this](std::string, std::string body, url_response_callback cb)
      {
         // Convenience wrapper: receiver==account==token_contract, action="transfer"
         // gives exactly one result per transfer (inline notifications excluded).
         static constexpr chain::name default_token_contract = "sysio.token"_n;
         static constexpr chain::name transfer_action_name   = "transfer"_n;
         action_query query;
         query.action   = transfer_action_name;
         query.receiver = default_token_contract;
         query.account  = default_token_contract;

         if (!body.empty()) {
            try {
               auto input = fc::json::from_string(body);
               const auto& obj = input.get_object();
               if (obj.contains("token_contract")) {
                  chain::name tc = chain::name(obj["token_contract"].as_string());
                  query.receiver = tc;
                  query.account  = tc;
               }
               if (obj.contains("block_num_start"))
                  query.block_num_start = obj["block_num_start"].as<uint32_t>();
               if (obj.contains("block_num_end"))
                  query.block_num_end = obj["block_num_end"].as<uint32_t>();
            } catch (...) {
               error_results results{400, "Bad request body"};
               cb( 400, fc::variant( results ));
               return;
            }
         }

         if (query.block_num_start > query.block_num_end) {
            error_results results{400, "block_num_start must be <= block_num_end"};
            cb( 400, fc::variant( results ));
            return;
         }

         clamp_block_end(query);

         try {
            auto result = req_handler->get_token_transfer_actions(query);
            cb( 200, fc::mutable_variant_object()
                        ("block_num_start", query.block_num_start)
                        ("block_num_end",   query.block_num_end)
                        ("transfers",       result.actions) );
         } catch (...) {
            http_plugin::handle_exception("trace_api", "get_token_transfers", body, cb);
         }
      }});
   }

   void plugin_shutdown() {
   }

   // Silently clamp block_num_end so the scan spans at most max_block_range blocks.
   // No 400 returned -- wide-range requests are a normal pagination pattern.
   // The response envelope reports the actual range so clients can detect the clamp.
   //
   // Always reduce block_num_end to min(requested end, start + range - 1), computing the span
   // limit in 64-bit.  The earlier "clamp only when max_end < end" form left block_num_end at
   // UINT32_MAX when block_num_start sat within max_block_range-1 of the top (max_end then exceeds
   // any uint32_t end), relying on the near-the-top range being naturally small.  Reducing
   // unconditionally keeps the range bound robust regardless of max_block_range.
   void clamp_block_end(action_query& query) const {
      const uint64_t max_end = uint64_t{query.block_num_start} + max_block_range - 1;
      query.block_num_end = static_cast<uint32_t>(std::min<uint64_t>(query.block_num_end, max_end));
   }

   std::shared_ptr<trace_api_common_impl> common;
   uint32_t max_block_range = 100;

   using request_handler_t = request_handler<shared_store_provider<store_provider>, abi_data_handler::shared_provider>;
   std::shared_ptr<request_handler_t> req_handler;
};

struct trace_api_plugin_impl {
   explicit trace_api_plugin_impl( const std::shared_ptr<trace_api_common_impl>& common )
   :common(common) {}

   void plugin_initialize(const appbase::variables_map& options) {
      fc_ilog(_log, "trace_api: initializing trace api plugin");
      auto log_exceptions_and_shutdown = [](const exception_with_context& e) {
         log_exception(e, fc::log_level::error);
         app().quit();
         throw yield_exception("shutting down");
      };
      auto& chain = app().find_plugin<chain_plugin>()->chain();

      // Lazy ABI fetcher: called from applied_transaction (chain write thread) on first
      // encounter of each account. Captures current ABI from the chain DB at that point.
      chain_extraction_t::abi_fetcher_t abi_fetcher = [&chain](chain::name account)
            -> std::optional<std::vector<char>> {
         std::optional<std::vector<char>> result;
         try {
            const auto* meta = chain.find_account_metadata(account);
            if (meta && meta->abi.size() > 0)
               result.emplace(meta->abi.data(), meta->abi.data() + meta->abi.size());
         } catch (const std::exception& e) {
            fc_dlog(_log, "trace_api: lazy ABI fetch for {} failed: {}", account, e.what());
         } catch (...) {
            fc_dlog(_log, "trace_api: lazy ABI fetch for {} failed: unknown", account);
         }
         return result;
      };

      extraction = std::make_shared<chain_extraction_t>(
         shared_store_provider<store_provider>(common->store),
         log_exceptions_and_shutdown,
         std::move(abi_fetcher));

      applied_transaction_connection.emplace(
         chain.applied_transaction().connect([this](std::tuple<const chain::transaction_trace_ptr&, const chain::packed_transaction_ptr&> t) {
            emit_killer([&](){
               extraction->signal_applied_transaction(std::get<0>(t), std::get<1>(t));
            });
         }));

      block_start_connection.emplace(
            chain.block_start().connect([this](uint32_t block_num) {
               emit_killer([&](){
                  extraction->signal_block_start(block_num);
               });
            }));

      accepted_block_connection.emplace(
         chain.accepted_block().connect([this](const chain::block_signal_params& t) {
            emit_killer([&](){
               const auto& [ block, id ] = t;
               extraction->signal_accepted_block(block, id);
            });
         }));

      irreversible_block_connection.emplace(
         chain.irreversible_block().connect([this](const chain::block_signal_params& t) {
            const auto& [ block, id ] = t;
            emit_killer([&](){
               extraction->signal_irreversible_block(block->block_num());
            });
         }));

   }

   void plugin_startup() {
      common->plugin_startup();
   }

   void plugin_shutdown() {
      common->plugin_shutdown();
   }

   std::shared_ptr<trace_api_common_impl> common;

   using chain_extraction_t = chain_extraction_impl_type<shared_store_provider<store_provider>>;
   std::shared_ptr<chain_extraction_t> extraction;

   std::optional<scoped_connection>                            applied_transaction_connection;
   std::optional<scoped_connection>                            block_start_connection;
   std::optional<scoped_connection>                            accepted_block_connection;
   std::optional<scoped_connection>                            irreversible_block_connection;
};

trace_api_plugin::trace_api_plugin() = default;

trace_api_plugin::~trace_api_plugin() = default;

void trace_api_plugin::set_program_options(appbase::options_description& cli, appbase::options_description& cfg) {
   trace_api_common_impl::set_program_options(cli, cfg);
}

void trace_api_plugin::plugin_initialize(const appbase::variables_map& options) {
   handle_sighup(); // setup logging

   auto common = std::make_shared<trace_api_common_impl>();
   common->plugin_initialize(options);

   my = std::make_shared<trace_api_plugin_impl>(common);
   my->plugin_initialize(options);

   rpc = std::make_shared<trace_api_rpc_plugin_impl>(common);
   rpc->plugin_initialize(options);
}

void trace_api_plugin::plugin_startup() {
   my->plugin_startup();
   rpc->plugin_startup();
}

void trace_api_plugin::plugin_shutdown() {
   my->plugin_shutdown();
   rpc->plugin_shutdown();
   fc_dlog( _log, "exit shutdown");
}

void trace_api_plugin::handle_sighup() {
   fc::logger::update( logger_name, _log );
}

trace_api_rpc_plugin::trace_api_rpc_plugin() = default;

trace_api_rpc_plugin::~trace_api_rpc_plugin() = default;

void trace_api_rpc_plugin::set_program_options(appbase::options_description& cli, appbase::options_description& cfg) {
   trace_api_common_impl::set_program_options(cli, cfg);
}

void trace_api_rpc_plugin::plugin_initialize(const appbase::variables_map& options) {
   auto common = std::make_shared<trace_api_common_impl>();
   common->plugin_initialize(options);

   rpc = std::make_shared<trace_api_rpc_plugin_impl>(common);
   rpc->plugin_initialize(options);
}

void trace_api_rpc_plugin::plugin_startup() {
   rpc->plugin_startup();
}

void trace_api_rpc_plugin::plugin_shutdown() {
   rpc->plugin_shutdown();
}

void trace_api_rpc_plugin::handle_sighup() {
   fc::logger::update( logger_name, _log );
}

}
