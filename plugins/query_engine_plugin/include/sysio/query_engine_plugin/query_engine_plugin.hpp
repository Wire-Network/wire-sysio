#pragma once

#include <sysio/chain/application.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/query_engine_plugin/query_service.hpp>

#include <memory>

namespace sysio {
/// Opt-in current-state SQL queries over this node's controller, with bounded off-chain evaluation.
class query_engine_plugin : public appbase::plugin<query_engine_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin))
   /// Construct private lifecycle state without accessing the controller.
   query_engine_plugin();
   /// Stop owned workers before destroying private state.
   ~query_engine_plugin();
   /// Register immutable query-* limits for config.ini and CLI.
   void set_program_options(options_description&, options_description&) override;
   /// Validate options and reject configured block producers.
   void plugin_initialize(const variables_map&);
   /// Start the query service; register a route only if HTTP and its chain_ro category are enabled.
   void plugin_startup();
   /// Return the shared C++ API, or nullptr before startup/after shutdown. Retained services reject calls after stop.
   std::shared_ptr<query_engine::query_service> get_query_service() const;
   /// Cancel pending work and release controller access before dependency shutdown.
   void plugin_shutdown();
   /// Rebind only the diagnostic logger.
   void handle_sighup() override;

private:
   struct impl;
   std::unique_ptr<impl> _impl;
};
} // namespace sysio
