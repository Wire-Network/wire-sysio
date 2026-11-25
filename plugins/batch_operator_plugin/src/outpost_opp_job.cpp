#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>

#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>

#include <sysio/chain/exceptions.hpp>

#include <utility>

#include "chain_epochs.hpp"

namespace sysio {

namespace {

/// Outpost backlog reads per chain minimum, at most, while this operator has nothing reported for the epoch.
constexpr int64_t BACKLOG_READS_PER_MINIMUM = 4;

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

outpost_opp_job::outpost_opp_job(outpost_client_ptr              client,
                                 depot_ops&                      depot,
                                 std::function<fc::time_point()> clock)
   : _client(std::move(client))
   , _depot(depot)
   , _clock(std::move(clock)) {}

void outpost_opp_job::run_outbound() {
   std::lock_guard<std::mutex> lock(_state_mx);

   // Every epoch query names this job's own chain: each outpost chain runs its own consensus epoch.
   const uint64_t chain_code = _client->chain_code();
   if (!_depot.is_elected(chain_code)) {
      dlog("outpost_opp_job[{}]: outbound skipped — not elected for the chain's current epoch",
           _client->to_string());
      return;
   }

   const uint32_t epoch = _depot.current_epoch(chain_code);
   if (epoch == 0) {
      // Genesis epoch is handled by the depot's bootstrap path, not here.
      return;
   }

   bool returned = false;
   try {
      // Completion evidence, re-read every cycle rather than latched: until the depot records this operator's
      // inbound delivery of the epoch, the outpost may still need this relay's part of it.
      if (_depot.has_delivered_envelope(chain_code, epoch))
         return;

      std::optional<outbound_envelope_record> pending = _depot.read_pending_outbound(chain_code, epoch);
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
      // outpost already settled the epoch, it tipped on another relay's
      // digest, or this relay's delivery is recorded and awaits the rest of
      // the group. An epoch the relay could NOT finish (outpost behind,
      // continuation bound reached, a call at the gas ceiling that advanced
      // nothing) arrives as `outpost_delivery_incomplete_exception`. Either way
      // the next tick calls again until the depot records the inbound reply.
      const std::string tx_id = _client->deliver_outbound_envelope(epoch, pending->raw_envelope,
                                                                   _depot.delivery_timeout(chain_code));
      if (tx_id.empty()) {
         dlog("outpost_opp_job[{}]: outbound envelope for epoch {} has nothing to send", _client->to_string(),
              epoch);
      } else {
         ilog("outpost_opp_job[{}]: delivered outbound envelope for epoch {} ({} bytes) tx={}", _client->to_string(),
              epoch, pending->raw_envelope.size(), tx_id);
      }
      returned = true;
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

   // The outpost's per-epoch cranks ride the first outbound call of an epoch
   // that returned -- never a failed one, never a repeat. Best-effort and
   // separate from the delivery result: a failed crank is retried by the next
   // epoch's delivery.
   if (returned && _last_outbound_epoch != epoch) {
      _last_outbound_epoch = epoch;
      try {
         _client->crank_outpost(epoch, _depot.delivery_timeout(chain_code));
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

   const uint64_t chain_code = _client->chain_code();
   const uint32_t epoch      = _depot.current_epoch(chain_code);
   if (epoch == 0) return;

   // Bytes for an epoch older than the previous one are owed no more: the depot refuses its delivery.
   std::erase_if(_inbound_envelopes,
                 [epoch](const std::pair<const uint32_t, std::vector<char>>& kept) { return kept.first + 1 < epoch; });

   if (_depot.is_elected(chain_code)) {
      deliver_inbound(chain_code, epoch, /*late=*/false);
   } else {
      dlog("outpost_opp_job[{}]: inbound skipped for epoch {} — not elected for it", _client->to_string(), epoch);
   }
   // A member of the previous epoch's group may still deliver it within the chain's late-delivery window, credited
   // when the current epoch closes. Once the window has closed the depot takes it no more, so its bytes go.
   if (epoch > 1) {
      if (_depot.late_delivery_open(chain_code)) {
         deliver_inbound(chain_code, epoch - 1, /*late=*/true);
      } else {
         _inbound_envelopes.erase(epoch - 1);
      }
   }
}

void outpost_opp_job::deliver_inbound(uint64_t chain_code, uint32_t epoch_index, bool late) {
   try {
      if (_depot.has_delivered_envelope(chain_code, epoch_index)) {
         dlog("outpost_opp_job[{}]: already delivered for epoch {}",
              _client->to_string(), epoch_index);
         return;
      }

      // The outpost overwrites its one latest-envelope slot at its next emit, so a late delivery uses the bytes kept
      // from an earlier read when there are any. The current epoch is read afresh.
      std::vector<char> raw;
      const auto        kept = _inbound_envelopes.find(epoch_index);
      if (late && kept != _inbound_envelopes.end()) {
         raw = kept->second;
      } else {
         raw = _client->read_inbound_envelope(epoch_index, _depot.delivery_timeout(chain_code));
         if (raw.empty()) {
            dlog("outpost_opp_job[{}]: no inbound envelope for epoch {}",
                 _client->to_string(), epoch_index);
            return;
         }
         _inbound_envelopes[epoch_index] = raw;
      }

      // Emit BEFORE deliver_to_depot for the same reason as run_outbound:
      // a transient WIRE-side push_action failure (e.g., mempool full,
      // ABI-serializer hiccup) must not eat the bytes we just pulled off
      // the outpost — those bytes are the diagnostic record. Wrapped so a
      // throwing slot can't break the inbound path.
      try {
         _depot.emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent{
            epoch_index,
            outpost_depot_direction_for(_client->chain_kind()),
            ::sysio::chain::name{}, // batch_op_name filled in by the depot
            raw
         });
      } FC_LOG_AND_DROP("outpost_opp_job[{}]: emit_debug_envelope threw", _client->to_string());

      const push_outcome delivered = _depot.deliver_to_depot(chain_code, raw);
      if (delivered != push_outcome::accepted) {
         // The push logged the depot's reason. Kept bytes the depot refused are not offered again: the next cycle
         // reads the outpost afresh, which has moved on once it emitted the following epoch. A push the depot never
         // answered, timed out or not built, keeps them for the next cycle while the window is open.
         if (late && delivered == push_outcome::refused) _inbound_envelopes.erase(epoch_index);
         ilog("outpost_opp_job[{}]: the depot did not accept the {}delivery for epoch {}; the next cycle decides "
              "again",
              _client->to_string(), late ? "late " : "", epoch_index);
         return;
      }
      ilog("outpost_opp_job[{}]: delivered {} inbound bytes to depot for {}epoch {}",
           _client->to_string(), raw.size(), late ? "previous " : "", epoch_index);
      // This delivery may have decided the epoch; a due chain then advances without waiting for the epoch tick.
      if (!late) _depot.push_chkcons_if_due(chain_code);
   } catch (const fc::exception& e) {
      wlog("outpost_opp_job[{}]: inbound cycle for epoch {} failed: {}",
           _client->to_string(), epoch_index, e.to_string());
   } catch (const std::exception& e) {
      wlog("outpost_opp_job[{}]: inbound cycle for epoch {} failed: {}",
           _client->to_string(), epoch_index, e.what());
   }
}

void outpost_opp_job::run_backlog() {
   std::lock_guard<std::mutex> lock(_backlog_mx);

   const uint64_t chain_code = _client->chain_code();
   if (!_depot.is_elected(chain_code)) {
      dlog("outpost_opp_job[{}]: backlog skipped — not elected for the chain's current epoch", _client->to_string());
      return;
   }
   const uint32_t epoch = _depot.current_epoch(chain_code);
   if (epoch == 0) return;
   // The depot takes a report only for a decided epoch: its accepted envelope proves the outpost emitted it.
   if (!_depot.is_decided(chain_code)) {
      dlog("outpost_opp_job[{}]: backlog skipped — epoch {} is not decided", _client->to_string(), epoch);
      return;
   }

   try {
      // One report per epoch: each eligible member's first report runs the depot's tally.
      if (_depot.has_reported_backlog(chain_code, epoch)) return;
      const fc::time_point   now     = _clock();
      const fc::microseconds minimum = _depot.minimum_duration(chain_code);
      if (now < _last_backlog_read + fc::microseconds(minimum.count() / BACKLOG_READS_PER_MINIMUM)) return;

      _last_backlog_read = now;
      const outbound_backlog backlog = _client->read_outbound_backlog(_depot.delivery_timeout(chain_code));
      if (backlog.outbound_epoch != epoch) {
         dlog("outpost_opp_job[{}]: backlog skipped — the outpost's latest emit is for epoch {}, not {}",
              _client->to_string(), backlog.outbound_epoch, epoch);
         return;
      }
      if (backlog.pending_count == 0) {
         dlog("outpost_opp_job[{}]: no outbound backlog in epoch {}", _client->to_string(), epoch);
         return;
      }
      if (!_depot.report_backlog(chain_code, epoch, backlog.pending_count, backlog.oldest_pending_ms)) {
         ilog("outpost_opp_job[{}]: the depot did not accept the backlog report for epoch {}; the next cycle "
              "decides again",
              _client->to_string(), epoch);
         return;
      }
      ilog("outpost_opp_job[{}]: reported an outbound backlog of {} pending since {} ms for epoch {}",
           _client->to_string(), backlog.pending_count, backlog.oldest_pending_ms, epoch);
   } catch (const fc::exception& e) {
      wlog("outpost_opp_job[{}]: backlog cycle failed: {}", _client->to_string(), e.to_string());
   } catch (const std::exception& e) {
      wlog("outpost_opp_job[{}]: backlog cycle failed: {}", _client->to_string(), e.what());
   }
}

} // namespace sysio
