#include <fc/log/logger.hpp>

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>

namespace sysio {

// ---------------------------------------------------------------------------
//  Implementation
// ---------------------------------------------------------------------------
struct batch_operator_plugin::impl {
   // Configuration
   chain::name              operator_account;
   bool                     enabled = false;
   uint32_t                 epoch_poll_ms = 5000;
   uint32_t                 outpost_poll_ms = 3000;
   uint32_t                 delivery_timeout_ms = 30000;

   // State — ALL 21 operators track this, not just elected 7
   uint32_t                 current_epoch = 0;
   uint8_t                  my_group = 255;       // unassigned until read from chain
   bool                     is_elected = false;
   std::vector<chain::name> current_group_members;

   // Integration points (set during initialize)
   chain_plugin*            chain_plug = nullptr;
   cron_plugin*             cron_plug = nullptr;

   // Background worker
   std::unique_ptr<boost::asio::io_context> io_ctx;
   std::optional<boost::asio::io_context::work> io_work;
   std::thread              worker_thread;
   bool                     shutting_down = false;

   // ---- Epoch cycle orchestration ----

   void on_irreversible_block(const chain::block_signal_params& bsp) {
      if (!enabled || shutting_down) return;

      // TODO: Read sysio.epoch::epochstate table via chain_plug->get_read_only_api()
      //       Compare epoch index; if advanced:
      //       1. Update current_epoch, current_group_members
      //       2. is_elected = (my_group == epoch_state.current_group)
      //       3. If elected: post run_epoch_cycle() to io_ctx
      //       4. If NOT elected: monitor only
   }

   void run_epoch_cycle() {
      if (shutting_down) return;
      ilog("batch_operator: running epoch cycle for epoch ${e}", ("e", current_epoch));

      // PHASE 1 — OUTBOUND (WIRE → Outposts)
      // For each registered outpost:
      //   crank_depot_outbound(outpost_id);
      //   read_outbound_chain(outpost_id);
      //   deliver_to_outpost(outpost_id);
      //   verify_outbound_delivery(outpost_id);

      // PHASE 2 — INBOUND (Outposts → WIRE)
      // For each registered outpost:
      //   crank_outpost_epoch(outpost_id);
      //   read_inbound_chain(outpost_id);
      //   deliver_to_depot(outpost_id);
      //   verify_inbound_delivery(outpost_id);
   }

   // ---- Phase 1: Outbound ----

   void crank_depot_outbound(uint64_t outpost_id) {
      // Push sysio.msgch::crank action to produce outbound OPP Message Chain
   }

   void read_outbound_chain(uint64_t outpost_id) {
      // Read the produced chain from sysio.msgch tables
   }

   void deliver_to_outpost(uint64_t outpost_id) {
      // ETH: call OPPInbound.epochIn() + messagesIn() via eth RPC
      // SOL: call opp_inbound::epoch_in() + messages_in() via sol RPC
   }

   void verify_outbound_delivery(uint64_t outpost_id) {
      // All 7 independently verify the delivered chain
   }

   // ---- Phase 2: Inbound ----

   void crank_outpost_epoch(uint64_t outpost_id) {
      // ETH: call OPP.finalizeEpoch()
      // SOL: call finalize_epoch
   }

   void read_inbound_chain(uint64_t outpost_id) {
      // ETH: read OPPMessage/OPPEpoch events from eth_logs
      // SOL: read OPP events from Solana transaction logs
   }

   void deliver_to_depot(uint64_t outpost_id) {
      // Push sysio.msgch::deliver action with chain_hash + raw_messages
   }

   void verify_inbound_delivery(uint64_t outpost_id) {
      // All 7 independently verify delivered chain hashes match
   }
};

// ---------------------------------------------------------------------------
//  Plugin lifecycle
// ---------------------------------------------------------------------------
batch_operator_plugin::batch_operator_plugin()
   : _impl(std::make_unique<impl>()) {}

batch_operator_plugin::~batch_operator_plugin() = default;

void batch_operator_plugin::set_program_options(options_description& cli,
                                                 options_description& cfg) {
   auto opts = cfg.add_options();
   opts("batch-operator-account", bpo::value<std::string>(),
        "WIRE account name for this batch operator");
   opts("batch-epoch-poll-ms", bpo::value<uint32_t>()->default_value(5000),
        "How often to check epoch state (ms)");
   opts("batch-outpost-poll-ms", bpo::value<uint32_t>()->default_value(3000),
        "How often to poll outpost for new messages (ms)");
   opts("batch-delivery-timeout-ms", bpo::value<uint32_t>()->default_value(30000),
        "Max time to wait for chain delivery confirmation (ms)");
   opts("batch-enabled", bpo::value<bool>()->default_value(false),
        "Enable batch operator functionality");
}

void batch_operator_plugin::plugin_initialize(const variables_map& options) {
   if (options.count("batch-operator-account")) {
      _impl->operator_account = chain::name(options["batch-operator-account"].as<std::string>());
   }
   _impl->epoch_poll_ms = options["batch-epoch-poll-ms"].as<uint32_t>();
   _impl->outpost_poll_ms = options["batch-outpost-poll-ms"].as<uint32_t>();
   _impl->delivery_timeout_ms = options["batch-delivery-timeout-ms"].as<uint32_t>();
   _impl->enabled = options["batch-enabled"].as<bool>();

   _impl->chain_plug = &app().get_plugin<chain_plugin>();
   _impl->cron_plug = &app().get_plugin<cron_plugin>();
}

void batch_operator_plugin::plugin_startup() {
   if (!_impl->enabled) {
      ilog("batch_operator_plugin: disabled, skipping startup");
      return;
   }

   ilog("Starting batch operator plugin for account ${a}",
        ("a", _impl->operator_account.to_string()));

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
      fc::set_thread_name("batch-op");
      _impl->io_ctx->run();
   });
}

void batch_operator_plugin::plugin_shutdown() {
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
   ilog("batch_operator_plugin: shutdown complete");
}

} // namespace sysio
