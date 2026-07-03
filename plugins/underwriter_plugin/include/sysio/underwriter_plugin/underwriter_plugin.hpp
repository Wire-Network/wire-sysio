#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
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
      /// Behind-now gap (ms) under which the node counts as synced. Measured
      /// against the LAST IRREVERSIBLE block's time — the state the plugin's
      /// table reads actually serve (operator daemons run read-mode =
      /// irreversible). Gates the deferred startup (preflight → cron) on
      /// cold-booting operator nodes; see `sync_detail.hpp::head_is_recent`.
      /// Must exceed steady-state finality lag (a few blocks) and stay well
      /// under one epoch.
      constexpr uint32_t sync_recency_ms     = 5000;
      /// How long after the sync gate arms a failing preflight keeps
      /// RETRYING before the failure is terminal. Rows the harness confirmed
      /// final on the producer can land in the LOCAL irreversible state a
      /// beat after the gate arms (observed live: a registration in block N
      /// readable only 50ms after a preflight read at LIB N−3); the grace
      /// absorbs that boundary race while a genuinely incomplete bootstrap
      /// still fails loudly once it expires.
      constexpr uint32_t preflight_retry_grace_ms    = 15000;
      /// Cadence (ms) of preflight retries within the grace window.
      constexpr uint32_t preflight_retry_interval_ms = 500;
      // SEC-13/WSA-027: the single eth/sol client-id defaults were removed with
      // the single-client config — outpost wiring is now per-chain and required
      // (--underwriter-{eth,sol}-outpost), with no single fallback.
   }

   class underwriter_plugin : public appbase::plugin<underwriter_plugin> {
   public:
      APPBASE_PLUGIN_REQUIRES(
         (chain_plugin)
         (cron_plugin)
         (http_plugin)
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
