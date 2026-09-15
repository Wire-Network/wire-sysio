#pragma once

#include <memory>
#include <sysio/chain/application.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>

namespace sysio {

/// Writes one /v1/chain/get_info snapshot per irreversible block straight to an OpenSearch/Elasticsearch
/// `_bulk` endpoint, rendered through the operator's JSON document template (the snapshot is the
/// `status_monitor` member of the object the template's `${data}` token renders), feeding dashboards
/// without involving the logging framework. Opt-in (`plugin = sysio::status_monitor_plugin`) and disabled
/// until `--status-monitor-target-url` is set; every knob is a `status-monitor-*` option in config.ini or on
/// the command line, fixed at startup (SIGHUP refreshes only the diagnostic logger). The template file is a
/// JSON object whose string values carry `${token}` placeholders; every other member is emitted as written
/// (a JSON file has no comment syntax, so a "_comment" member would land in every document), and no field
/// is required. Emits only for irreversible blocks within status_monitor::max_current_block_age of
/// wall-clock time -- nothing while syncing or replaying, and nothing on a host whose clock runs more than
/// that ahead of the chain (the pause is warned about once a minute and the resumption logged once). The
/// block-application thread only copies the snapshot and queues it; a render worker builds the document and
/// a delivery worker batches up to --status-monitor-max-items-per-task documents per bulk request, which the
/// es client sends from its own io thread (three threads in all). Delivery problems are counted on the workers
/// and reported from the application thread, where the failure line and the recovery line share one rate limit
/// of at most one line a minute between them -- so an endpoint alternating between failed and acknowledged
/// batches cannot log a line per block. Steady-state rate: one document per irreversible block, about
/// two per second at the 500 ms block interval.
///
/// config.ini excerpt:
///   plugin = sysio::status_monitor_plugin
///   status-monitor-target-url = https://opensearch.example.com
///   status-monitor-target-index = <your index or write alias>
///   status-monitor-target-template-file = /etc/wire/status-monitor-template.json
class status_monitor_plugin : public appbase::plugin<status_monitor_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin))

   /// Allocates the private state; nothing is parsed, started, or connected until plugin_initialize.
   status_monitor_plugin();
   /// Releases the private state; plugin_shutdown has already stopped the workers.
   virtual ~status_monitor_plugin();

   /// Registers the status-monitor-* options (all `cfg` options: config.ini and command line).
   virtual void set_program_options(options_description& cli, options_description& cfg) override;
   /// Parses and validates the options, loading and compiling the template -- a bad value fails startup with
   /// chain::plugin_config_exception -- and binds the diagnostic logger. An absent target URL leaves the
   /// plugin disabled.
   void plugin_initialize(const variables_map& options);
   /// When enabled: acquires chain_plugin's read-only API, starts the es client and the two workers, and
   /// connects the irreversible_block slot.
   void plugin_startup();
   /// Disconnects the slot, cancels an in-flight request, stops the workers, and reports the final counters.
   void plugin_shutdown();
   /// Refreshes the diagnostic logger from the reloaded logging config; options are never re-read.
   void handle_sighup() override;

private:
   struct impl;
   std::unique_ptr<impl> _impl;
};

} // namespace sysio
