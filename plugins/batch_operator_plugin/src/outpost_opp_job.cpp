#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>

#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>

namespace sysio {

namespace {

/// Pick the debug-event endpoint enum for the direction WIRE -> outpost,
/// given the outpost's chain kind.
sysio::opp::debugging::DebugOutpostEndpointsType
depot_outpost_direction_for(sysio::opp::types::ChainKind kind) {
   switch (kind) {
      case sysio::opp::types::CHAIN_KIND_ETHEREUM:
         return sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM;
      case sysio::opp::types::CHAIN_KIND_SOLANA:
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
      case sysio::opp::types::CHAIN_KIND_ETHEREUM:
         return sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT;
      case sysio::opp::types::CHAIN_KIND_SOLANA:
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
   if (epoch == _last_outbound_epoch) {
      // Already attempted this epoch; avoid re-delivering on every cron tick.
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

   try {
      auto tx_id = _client->deliver_outbound_envelope(epoch, pending->raw_envelope, _outpost_deadline);
      ilog("outpost_opp_job[{}]: delivered outbound envelope ({} bytes) tx={}",
           _client->to_string(), pending->raw_envelope.size(), tx_id);

      _depot.emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent{
         epoch,
         depot_outpost_direction_for(_client->chain_kind()),
         ::sysio::chain::name{}, // batch_op_name filled in by the depot
         pending->raw_envelope
      });

      _last_outbound_epoch = epoch;
   } catch (const fc::exception& e) {
      wlog("outpost_opp_job[{}]: outbound delivery failed: {}",
           _client->to_string(), e.to_string());
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

      _depot.deliver_to_depot(_client->outpost_id(), raw);
      ilog("outpost_opp_job[{}]: delivered {} inbound bytes to depot for epoch {}",
           _client->to_string(), raw.size(), epoch);

      _depot.emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent{
         epoch,
         outpost_depot_direction_for(_client->chain_kind()),
         ::sysio::chain::name{}, // batch_op_name filled in by the depot
         raw
      });
   } catch (const fc::exception& e) {
      wlog("outpost_opp_job[{}]: inbound cycle failed: {}",
           _client->to_string(), e.to_string());
   } catch (const std::exception& e) {
      wlog("outpost_opp_job[{}]: inbound cycle failed: {}",
           _client->to_string(), e.what());
   }
}

} // namespace sysio
