#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin.hpp>

namespace sysio {

   /// Underwriter plugin — a separate daemon from the batch operator.
   ///
   /// Does NOT crank any contracts. Instead it:
   /// 1. Reads PENDING messages from sysio.msgch filtered by ATTESTATION_TYPE_SWAP
   /// 2. Independently verifies deposits on external chains (ETH/SOL)
   /// 3. Selects swaps that maximize utilization of deposited collateral (greedy knapsack)
   /// 4. Submits underwriting intent to sysio.uwrit
   /// 5. Monitors uwledger for dual-outpost confirmation
   class underwriter_plugin : public appbase::plugin<underwriter_plugin> {
   public:
      APPBASE_PLUGIN_REQUIRES(
         (chain_plugin)
         (cron_plugin)
         (outpost_ethereum_client_plugin)
         (outpost_solana_client_plugin)
      )

      underwriter_plugin();
      virtual ~underwriter_plugin();

      virtual void set_program_options(options_description& cli, options_description& cfg) override;
      void plugin_initialize(const variables_map& options);
      void plugin_startup();
      void plugin_shutdown();

   private:
      struct impl;
      std::unique_ptr<impl> _impl;
   };

} // namespace sysio
