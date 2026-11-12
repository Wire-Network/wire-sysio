#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <sysio/account_watch_plugin/account_watch.hpp>
#include <sysio/account_watch_plugin/account_watch_plugin.hpp>
#include <sysio/chain/controller.hpp>

#include <boost/signals2/connection.hpp>

#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace sysio {

/// Every member is touched only on the application thread: plugin_initialize/startup/shutdown run there, and so do the
/// controller signals the watcher is driven by.
struct account_watch_plugin::impl {
   fc::logger log;                                ///< the `account_watch` logger
   std::optional<account_watch::config> cfg;      ///< set by plugin_initialize
   std::unique_ptr<account_watch::watcher> watch; ///< set at startup when enabled
   /// controller::block_start, feeding watcher::on_block_start
   std::optional<boost::signals2::scoped_connection> block_start_connection;
   /// controller::applied_transaction, feeding watcher::on_applied_transaction
   std::optional<boost::signals2::scoped_connection> applied_transaction_connection;
   /// controller::accepted_block, feeding watcher::on_accepted_block
   std::optional<boost::signals2::scoped_connection> accepted_block_connection;

   /** Write @p record at the level its @p kind deserves: a match is a warning, everything else information. */
   void write(account_watch::event kind, const fc::variant_object& record);
   /** Debug check of the single-thread invariant stated on this struct. */
   void assert_application_thread() const;
};

void account_watch_plugin::impl::write(account_watch::event kind, const fc::variant_object& record) {
   if (kind == account_watch::event::match)
      fc_wlog(log, "{}", account_watch::to_json_line(record));
   else
      fc_ilog(log, "{}", account_watch::to_json_line(record));
}

void account_watch_plugin::impl::assert_application_thread() const {
   assert(std::this_thread::get_id() == appbase::app().executor().get_main_thread_id());
}

account_watch_plugin::account_watch_plugin()
   : _impl(std::make_unique<impl>()) {}

account_watch_plugin::~account_watch_plugin() = default;

void account_watch_plugin::set_program_options(options_description&, options_description& cfg) {
   account_watch::add_options(cfg);
}

void account_watch_plugin::plugin_initialize(const variables_map& options) {
   try {
      _impl->cfg = account_watch::parse_config(options);
      handle_sighup(); // bind the logger, the convention every plugin follows at the end of initialize
   }
   FC_LOG_AND_RETHROW()
}

void account_watch_plugin::plugin_startup() {
   try {
      chain_plugin& chain_plug = app().get_plugin<chain_plugin>();
      chain::controller& chain = chain_plug.chain();
      _impl->watch = std::make_unique<account_watch::watcher>(
         *_impl->cfg, chain, chain_plug.get_abi_serializer_max_time(),
         [impl = _impl.get()](account_watch::event kind, const fc::variant_object& record) {
            impl->write(kind, record);
         },
         [] { return fc::time_point::now(); });
      _impl->watch->report_start();

      // A failure in a handler is logged and dropped: watching never affects block processing.
      _impl->block_start_connection.emplace(chain.block_start().connect([impl = _impl.get()](uint32_t block_num) {
         impl->assert_application_thread();
         try {
            impl->watch->on_block_start(block_num);
         }
         FC_LOG_AND_DROP("{} block_start", account_watch::logger_name);
      }));
      _impl->applied_transaction_connection.emplace(chain.applied_transaction().connect(
         [impl = _impl.get()](std::tuple<const chain::transaction_trace_ptr&, const chain::packed_transaction_ptr&> t) {
            impl->assert_application_thread();
            try {
               impl->watch->on_applied_transaction(std::get<0>(t));
            }
            FC_LOG_AND_DROP("{} applied_transaction", account_watch::logger_name);
         }));
      _impl->accepted_block_connection.emplace(
         chain.accepted_block().connect([impl = _impl.get()](const chain::block_signal_params& t) {
            impl->assert_application_thread();
            try {
               const auto& [block, id] = t;
               impl->watch->on_accepted_block(block, id);
            }
            FC_LOG_AND_DROP("{} accepted_block", account_watch::logger_name);
         }));
   }
   FC_LOG_AND_RETHROW()
}

void account_watch_plugin::plugin_shutdown() {
   _impl->accepted_block_connection.reset();
   _impl->applied_transaction_connection.reset();
   _impl->block_start_connection.reset();
   if (_impl->watch)
      _impl->watch->report_stop();
}

void account_watch_plugin::handle_sighup() {
   fc::logger::update(std::string{account_watch::logger_name}, _impl->log);
}

} // namespace sysio
