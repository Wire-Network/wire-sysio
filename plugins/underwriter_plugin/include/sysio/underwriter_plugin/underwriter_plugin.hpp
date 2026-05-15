#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

namespace sysio {

   /// Underwriter plugin — polls for underwrite requests and submits intents.
   ///
   /// The underwriter does NOT relay OPP envelopes (that's the batch operator's job).
   /// Instead it:
   /// 1. Polls sysio.uwrit::uwreqs for PENDING underwrite requests
   /// 2. Reads its own credit lines from sysio.opreg::operators (aggregate stakes per chain)
   /// 3. Selects requests where credit covers 100% of SWAP on both send and receive chains
   /// 4. Submits underwrite intent to the relevant outpost contract (ETH/SOL)
   ///    — the outpost locks capital and emits UNDERWRITE_INTENT via OPP
   /// 5. Monitors uwreqs for status changes (assigned, confirmed, rejected)
   namespace underwriter_defaults {
      constexpr uint32_t scan_interval_ms    = 5000;
      constexpr uint32_t action_timeout_ms   = 15000;
      constexpr bool     enabled             = false;
      constexpr const char* eth_client_id    = "eth-default";
      constexpr const char* sol_client_id    = "sol-default";
   }

   class underwriter_plugin : public appbase::plugin<underwriter_plugin> {
   public:
      APPBASE_PLUGIN_REQUIRES(
         (chain_plugin)
         (cron_plugin)
         (outpost_ethereum_client_plugin)
         (outpost_solana_client_plugin)
         (signature_provider_manager_plugin)
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
