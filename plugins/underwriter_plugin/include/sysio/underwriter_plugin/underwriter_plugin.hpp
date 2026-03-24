#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <thread>

namespace sysio {

   /// Underwriter plugin — a separate daemon from the batch operator.
   ///
   /// Does NOT crank any contracts. Instead it:
   /// 1. Reads PENDING messages from sysio.msgch
   /// 2. Independently verifies deposits on external chains (ETH/SOL)
   /// 3. Selects swaps that maximize utilization of deposited collateral
   /// 4. Submits underwriting intent to sysio.uwrit
   /// 5. Monitors for dual-outpost confirmation
   class underwriter_plugin : public appbase::plugin<underwriter_plugin> {
   public:
      APPBASE_PLUGIN_REQUIRES(
         (chain_plugin)
         (cron_plugin)
         (signature_provider_manager_plugin)
      )

      underwriter_plugin();
      virtual ~underwriter_plugin();

      virtual void set_program_options(options_description& cli, options_description& cfg);
      virtual void plugin_initialize(const variables_map& options);
      virtual void plugin_startup();
      virtual void plugin_shutdown();

   private:
      struct impl;
      std::unique_ptr<impl> _impl;
   };

} // namespace sysio
