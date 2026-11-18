#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>

#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>

#include <sysio/chain/exceptions.hpp>

#include <utility>

namespace sysio {

namespace {

constexpr auto OUTBOUND_RETRY_LABEL = "retry";
constexpr auto OUTBOUND_DELIVERED_LABEL = "delivered";

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
   const bool retry_pending = epoch == _last_outbound_epoch;
   // A recorded delivery cannot tip consensus again before the boundary. After
   // it elapses, the cron tick rate-limits retries until the round trip finishes.
   if (retry_pending && !_depot.is_epoch_boundary_past())
      return;

   try {
      // This is completion evidence, not the outbound signature. Solana's
      // inbound bytes are read at finalized commitment before depot delivery.
      // Re-read instead of latching so a WIRE-side rollback also permits recovery.
      if (_depot.has_delivered_envelope(_client->chain_code(), epoch))
         return;

      auto pending = _depot.read_pending_outbound(_client->chain_code(), epoch);
      if (!pending) {
         dlog("outpost_opp_job[{}]: no pending outbound for epoch {}",
              _client->to_string(), epoch);
         // No envelope yet: leave the next tick eligible.
         return;
      }

      // Emit before submission so failed attempts remain visible. A broken
      // debug sink must never prevent delivery.
      try {
         _depot.emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent{
            epoch,
            depot_outpost_direction_for(_client->chain_kind()),
            ::sysio::chain::name{}, // batch_op_name filled in by the depot
            pending->raw_envelope
         });
      } FC_LOG_AND_DROP("outpost_opp_job[{}]: emit_debug_envelope threw", _client->to_string());

      // An EMPTY tx id is not a failure: the relay had nothing to send. The
      // outpost already settled the epoch, or on Ethereum it tipped on
      // another relay's digest or this relay's delivery is recorded and waits
      // on the rest of the group. These returns remain provisional until the
      // inbound path establishes completion. An epoch the relay could NOT finish (outpost
      // behind, continuation bound reached, a call at the gas ceiling that
      // advanced nothing) arrives as `outpost_delivery_incomplete_exception`
      // and is retried next tick.
      auto tx_id = _client->deliver_outbound_envelope(epoch, pending->raw_envelope, _outpost_deadline);
      ilog("outpost_opp_job[{}]: {} outbound envelope ({} bytes) tx={}", _client->to_string(),
           retry_pending ? OUTBOUND_RETRY_LABEL : OUTBOUND_DELIVERED_LABEL, pending->raw_envelope.size(), tx_id);

      if (!retry_pending) {
         _last_outbound_epoch = epoch;
      }
   } catch (const chain::outpost_delivery_incomplete_exception& e) {
      // Incomplete deliveries remain eligible on the next tick. This may also
      // report an outpost that is behind, without establishing any progress.
      ilog("outpost_opp_job[{}]: outbound delivery for epoch {} continues next tick: {}",
           _client->to_string(), epoch, e.top_message());
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

   // The outpost's per-epoch cranks ride the first successful delivery of an
   // epoch -- never its consensus retry, never a failed delivery. Best-effort
   // and separate from the delivery result: the envelope landed regardless, and
   // a failed crank is retried by the next epoch's delivery.
   if (!retry_pending && _last_outbound_epoch == epoch) {
      try {
         _client->crank_outpost(epoch, _outpost_deadline);
      } catch (const fc::exception& e) {
         wlog("outpost_opp_job[{}]: outpost crank failed for epoch {}: {}",
              _client->to_string(), epoch, e.to_detail_string());
      } catch (const std::exception& e) {
         wlog("outpost_opp_job[{}]: outpost crank failed for epoch {}: {}",
              _client->to_string(), epoch, e.what());
      }
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
      if (_depot.has_delivered_envelope(_client->chain_code(), epoch)) {
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

      _depot.deliver_to_depot(_client->chain_code(), raw);
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
