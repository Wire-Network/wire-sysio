#pragma once

#include <sysio/chain/application.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>

#include <memory>

namespace sysio {

/// Reports every action in an accepted block that a watched authorization signed, or that runs at a watched account,
/// as one JSON record per action on the `account_watch` logger, so a watchdog reading the log can alert on a key being
/// used. Opt-in (`plugin = sysio::account_watch_plugin`), and once enabled it needs at least one
/// `--account-watch-include` or `--account-watch-receiver` rule or startup fails; the rules and limits are
/// `account-watch-*` options, fixed at startup. An include or exclude rule is
/// `<actor>[@<permission>],<contract>::<action>`, a receiver rule `<receiver>,<contract>::<action>`, each field a name
/// or `*`; an action is reported when one of its authorizations matches an include rule and no exclude rule, or when
/// it runs at an account a receiver rule names. Records are reported as blocks are accepted, so the
/// read mode decides their latency: at head in head mode, once irreversible in irreversible mode. All work runs on
/// the application thread, in the controller's signal handlers; there are no threads of its own.
///
/// config.ini excerpt:
///   plugin = sysio::account_watch_plugin
///   account-watch-include = prod.a,*::*
///   account-watch-include = snap.a,*::*
///   account-watch-exclude = snap.a@active,sysio::votesnaphash
class account_watch_plugin : public appbase::plugin<account_watch_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin))

   /// Allocates the private state; nothing is parsed or connected until plugin_initialize.
   account_watch_plugin();
   /// Releases the private state; plugin_shutdown has already disconnected from the controller.
   virtual ~account_watch_plugin();

   /// Registers the account-watch-* options (all `cfg` options: config.ini and command line).
   virtual void set_program_options(options_description& cli, options_description& cfg) override;
   /// Parses the rules -- a bad one fails startup with chain::plugin_config_exception -- and binds the logger.
   void plugin_initialize(const variables_map& options);
   /// When enabled: writes the start record and connects to the controller's block_start, applied_transaction and
   /// accepted_block signals. Connecting here, after chain_plugin has replayed its block log, keeps a restart from
   /// reporting replayed blocks again.
   void plugin_startup();
   /// Disconnects from the controller and writes the stop record.
   void plugin_shutdown();
   /// Re-binds the logger from the reloaded logging config; options are never re-read.
   void handle_sighup() override;

private:
   struct impl;
   std::unique_ptr<impl> _impl;
};

} // namespace sysio
