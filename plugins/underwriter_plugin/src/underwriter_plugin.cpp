#include <fc/log/logger.hpp>

#include <sysio/underwriter_plugin/underwriter_plugin.hpp>

namespace sysio {

// ---------------------------------------------------------------------------
//  Implementation
// ---------------------------------------------------------------------------
struct underwriter_plugin::impl {
   // Configuration
   chain::name              underwriter_account;
   bool                     enabled = false;
   uint32_t                 scan_interval_ms = 5000;
   uint32_t                 verify_timeout_ms = 10000;

   // Integration points (set during initialize)
   chain_plugin*            chain_plug = nullptr;
   cron_plugin*             cron_plug = nullptr;

   // Background worker
   std::unique_ptr<boost::asio::io_context> io_ctx;
   std::optional<boost::asio::io_context::work> io_work;
   std::thread              worker_thread;
   bool                     shutting_down = false;

   // ---- Core loop — does NOT crank, only reads and submits ----

   void on_irreversible_block(const chain::block_signal_params& bsp) {
      if (!enabled || shutting_down) return;

      // Post scanning work to background thread
      boost::asio::post(*io_ctx, [this]() {
         scan_pending_messages();
      });
   }

   void scan_pending_messages() {
      if (shutting_down) return;

      // TODO: Read PENDING messages from sysio.msgch via chain_plug
      //       Filter for ATTESTATION_TYPE_SWAP messages
      //       For each candidate swap:
      //         1. verify_on_external_chain(msg_id)
      //         2. If verified, add to candidate pool
      //       After scanning: select_optimal_underwriting()
   }

   void verify_on_external_chain(uint64_t msg_id) {
      // TODO: Read deposit details from the source chain (ETH/SOL)
      //       Confirm the deposit actually occurred on-chain
      //       This is the double-check between reading PENDING and committing
      //
      //       ETH: query eth RPC for transaction receipt
      //       SOL: query sol RPC for transaction confirmation
   }

   void select_optimal_underwriting() {
      // TODO: Given verified candidates and current collateral state,
      //       select the set of swaps that maximizes utilization of
      //       deposited collateral across all chains.
      //
      //       Greedy knapsack approach:
      //       1. Sort by fee-to-collateral ratio (highest first)
      //       2. Greedily select until collateral exhausted on any chain
      //       3. For each selected: submit_intent(msg_id)
   }

   void submit_intent(uint64_t msg_id) {
      // TODO: Push sysio.uwrit::submituw action
      //       with underwriter_account, msg_id, source_sig, target_sig
      ilog("underwriter: submitting intent for message ${m}", ("m", msg_id));
   }

   void check_confirmations() {
      // TODO: Monitor sysio.uwrit::uwledger for entries with status INTENT_SUBMITTED
      //       belonging to this underwriter. Check if BOTH outpost confirmations
      //       have arrived (via inbound message chain attestations).
   }
};

// ---------------------------------------------------------------------------
//  Plugin lifecycle
// ---------------------------------------------------------------------------
underwriter_plugin::underwriter_plugin()
   : _impl(std::make_unique<impl>()) {}

underwriter_plugin::~underwriter_plugin() = default;

void underwriter_plugin::set_program_options(options_description& cli,
                                              options_description& cfg) {
   auto opts = cfg.add_options();
   opts("underwriter-account", bpo::value<std::string>(),
        "WIRE account name for this underwriter");
   opts("underwriter-scan-interval-ms", bpo::value<uint32_t>()->default_value(5000),
        "How often to scan for pending messages (ms)");
   opts("underwriter-verify-timeout-ms", bpo::value<uint32_t>()->default_value(10000),
        "Timeout for external chain verification (ms)");
   opts("underwriter-enabled", bpo::value<bool>()->default_value(false),
        "Enable underwriter functionality");
}

void underwriter_plugin::plugin_initialize(const variables_map& options) {
   if (options.count("underwriter-account")) {
      _impl->underwriter_account = chain::name(options["underwriter-account"].as<std::string>());
   }
   _impl->scan_interval_ms = options["underwriter-scan-interval-ms"].as<uint32_t>();
   _impl->verify_timeout_ms = options["underwriter-verify-timeout-ms"].as<uint32_t>();
   _impl->enabled = options["underwriter-enabled"].as<bool>();

   _impl->chain_plug = &app().get_plugin<chain_plugin>();
   _impl->cron_plug = &app().get_plugin<cron_plugin>();
}

void underwriter_plugin::plugin_startup() {
   if (!_impl->enabled) {
      ilog("underwriter_plugin: disabled, skipping startup");
      return;
   }

   ilog("Starting underwriter plugin for account ${a}",
        ("a", _impl->underwriter_account.to_string()));

   // Subscribe to irreversible block events
   auto& cron = app().get_plugin<cron_plugin>();
   cron.events.irreversible_block.subscribe(
      [this](const chain::block_signal_params& bsp) {
         _impl->on_irreversible_block(bsp);
      }
   );

   // Start background worker thread
   _impl->io_ctx = std::make_unique<boost::asio::io_context>();
   _impl->io_work.emplace(*_impl->io_ctx);
   _impl->worker_thread = std::thread([this]() {
      fc::set_thread_name("underwriter");
      _impl->io_ctx->run();
   });
}

void underwriter_plugin::plugin_shutdown() {
   _impl->shutting_down = true;
   if (_impl->io_work) {
      _impl->io_work.reset();
   }
   if (_impl->io_ctx) {
      _impl->io_ctx->stop();
   }
   if (_impl->worker_thread.joinable()) {
      _impl->worker_thread.join();
   }
   ilog("underwriter_plugin: shutdown complete");
}

} // namespace sysio
