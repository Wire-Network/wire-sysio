#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>

#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>

namespace sysio {

namespace {

/// Pick the debug-event endpoint enum for the direction WIRE -> outpost,
/// given the outpost's chain kind.
sysio::opp::debugging::DebugOutpostEndpointsType
depot_outpost_direction_for(sysio::opp::types::ChainKind kind) {
   switch (kind) {
      case sysio::opp::types::CHAIN_KIND_EVM:
         return sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM;
      case sysio::opp::types::CHAIN_KIND_SVM:
         return sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA;
      default:
         return sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_UNKNOWN;
   }
}

/// Pick the debug-event endpoint enum for the direction outpost -> WIRE,
/// given the outpost's chain kind.
sysio::opp::debugging::DebugOutpostEndpointsType
outpost_depot_direction_for(sysio::opp::types::ChainKind kind) {
   switch (kind) {
      case sysio::opp::types::CHAIN_KIND_EVM:
         return sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT;
      case sysio::opp::types::CHAIN_KIND_SVM:
         return sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT;
      default:
         return sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_UNKNOWN;
   }
}

} // namespace

outpost_opp_job::outpost_opp_job(outpost_client_ptr client,
                                 depot_ops&         depot,
                                 fc::microseconds   outpost_deadline)
   : _client(std::move(client))
   , _depot(depot)
   , _outpost_deadline(outpost_deadline) {}

void outpost_opp_job::run_outbound() {
   std::lock_guard<std::mutex> lock(_state_mx);

   if (!_depot.is_elected()) {
      dlog("outpost_opp_job[{}]: outbound skipped — not elected for current epoch",
           _client->to_string());
      return;
   }
   if (!_depot.within_epoch_window()) {
      dlog("outpost_opp_job[{}]: outbound skipped — outside epoch window",
           _client->to_string());
      return;
   }

   const auto epoch = _depot.current_epoch();
   if (epoch == 0) {
      // Genesis epoch is handled by the depot's bootstrap path, not here.
      return;
   }
   // Path-2 fallback consensus retry. When the initial delivery happened
   // but consensus didn't tip (only majority delivered, e.g. freshop is
   // in the current group with no cranker), the outpost waits for the
   // boundary to elapse before path-2 can fire. The contract's check
   // only re-runs when a tx hits it, so the cranker must re-deliver
   // post-boundary. Both ETH `OPPInbound::epochIn` and SOL
   // `opp_outpost::epoch_in` are idempotent on same-hash re-deliveries
   // from the same operator — they re-run the consensus tip without
   // re-recording. See .claude/rules/opp-consensus.md.
   //
   // Gating:
   //   - Only one retry per epoch (`_last_consensus_retry_epoch`)
   //   - Only after WIRE depot's boundary has elapsed
   //     (`_depot.is_epoch_boundary_past()` — proxies the outpost-side
   //      boundary check since both sides share `epoch_duration_sec`)
   //
   // Without the boundary gate, the cranker would re-deliver every tick
   // and burn gas on no-op consensus checks before path-2 is eligible.
   const bool retry_pending = epoch == _last_outbound_epoch
                            && epoch != _last_consensus_retry_epoch
                            && _depot.is_epoch_boundary_past();
   if (epoch == _last_outbound_epoch && !retry_pending) {
      // Already attempted this epoch and either the boundary hasn't
      // elapsed yet or a retry already fired for it.
      return;
   }

   auto pending = _depot.read_pending_outbound(_client->outpost_id(), epoch);
   if (!pending) {
      dlog("outpost_opp_job[{}]: no pending outbound for epoch {}",
           _client->to_string(), epoch);
      // No envelope this cycle — don't burn the epoch slot; next tick can retry
      // once one is queued.
      return;
   }

   // Emit the debug event BEFORE delivery so failed attempts are still
   // captured by the external_debugging sink. The captured `.data` file is
   // the only path to root-causing a Solana RPC reject when the validator's
   // error body is opaque (e.g., -32602 with empty message). Wrapped in its
   // own try/log-and-drop so a misbehaving signal slot can never break the
   // producer cluster.
   try {
      _depot.emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent{
         epoch,
         depot_outpost_direction_for(_client->chain_kind()),
         ::sysio::chain::name{}, // batch_op_name filled in by the depot
         pending->raw_envelope
      });
   } FC_LOG_AND_DROP("outpost_opp_job[{}]: emit_debug_envelope threw", _client->to_string());

   try {
      auto tx_id = _client->deliver_outbound_envelope(epoch, pending->raw_envelope, _outpost_deadline);
      ilog("outpost_opp_job[{}]: {} outbound envelope ({} bytes) tx={}",
           _client->to_string(),
           retry_pending ? "consensus-retry"
                         : "delivered",
           pending->raw_envelope.size(),
           tx_id);

      if (retry_pending) {
         _last_consensus_retry_epoch = epoch;
      } else {
         _last_outbound_epoch = epoch;
      }
   } catch (const fc::network::json_rpc::json_rpc_error& e) {
      wlog("outpost_opp_job[{}]: outbound delivery failed: code={} message='{}' data={} detail='{}'",
           _client->to_string(), e.code, e.top_message(),
           e.data.is_null() ? std::string("<none>") : fc::json::to_string(e.data, fc::json::yield_function_t{}),
           e.to_detail_string());
   } catch (const fc::exception& e) {
      wlog("outpost_opp_job[{}]: outbound delivery failed: {}",
           _client->to_string(), e.to_detail_string());
   } catch (const std::exception& e) {
      wlog("outpost_opp_job[{}]: outbound delivery failed: {}",
           _client->to_string(), e.what());
   }
}

void outpost_opp_job::run_inbound() {
   std::lock_guard<std::mutex> lock(_state_mx);

   if (!_depot.is_elected()) {
      dlog("outpost_opp_job[{}]: inbound skipped — not elected for current epoch",
           _client->to_string());
      return;
   }
   if (!_depot.within_epoch_window()) {
      dlog("outpost_opp_job[{}]: inbound skipped — outside epoch window",
           _client->to_string());
      return;
   }

   const auto epoch = _depot.current_epoch();
   if (epoch == 0) return;

   try {
      if (_depot.has_delivered_envelope(_client->outpost_id(), epoch)) {
         dlog("outpost_opp_job[{}]: already delivered for epoch {}",
              _client->to_string(), epoch);
         return;
      }

      auto raw = _client->read_inbound_envelope(epoch, _outpost_deadline);
      if (raw.empty()) {
         dlog("outpost_opp_job[{}]: no inbound envelope for epoch {}",
              _client->to_string(), epoch);
         return;
      }

      // Emit BEFORE deliver_to_depot for the same reason as run_outbound:
      // a transient WIRE-side push_action failure (e.g., mempool full,
      // ABI-serializer hiccup) must not eat the bytes we just pulled off
      // the outpost — those bytes are the diagnostic record. Wrapped so a
      // throwing slot can't break the inbound path.
      try {
         _depot.emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent{
            epoch,
            outpost_depot_direction_for(_client->chain_kind()),
            ::sysio::chain::name{}, // batch_op_name filled in by the depot
            raw
         });
      } FC_LOG_AND_DROP("outpost_opp_job[{}]: emit_debug_envelope threw", _client->to_string());

      _depot.deliver_to_depot(_client->outpost_id(), raw);
      ilog("outpost_opp_job[{}]: delivered {} inbound bytes to depot for epoch {}",
           _client->to_string(), raw.size(), epoch);
   } catch (const fc::exception& e) {
      wlog("outpost_opp_job[{}]: inbound cycle failed: {}",
           _client->to_string(), e.to_string());
   } catch (const std::exception& e) {
      wlog("outpost_opp_job[{}]: inbound cycle failed: {}",
           _client->to_string(), e.what());
   }
}

} // namespace sysio
