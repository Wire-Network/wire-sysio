#pragma once

#include <cstdint>
#include <mutex>

#include <fc/time.hpp>

#include <sysio/batch_operator_plugin/depot_ops.hpp>
#include <sysio/outpost_client/outpost_client.hpp>

namespace sysio {

/**
 * @brief Chain-agnostic OPP delivery state-machine for a single outpost.
 *
 * Holds an `outpost_client_ptr` (the chain-specific SPI, owned by
 * `outpost_client_plugin`) and a reference to a `depot_ops` (the WIRE-side
 * contract, owned by `batch_operator_plugin`). The batch operator plugin
 * constructs one per registered outpost at startup and schedules two cron
 * jobs against it:
 *
 *   - `outpost_opp_outbound_<id>` calls `run_outbound()` every cycle.
 *   - `outpost_opp_inbound_<id>`  calls `run_inbound()`  every cycle.
 *
 * Both jobs can fire on different cron worker threads; the internal
 * `_state_mx` serializes mutable state access between them. `one_at_a_time`
 * on each cron job prevents a single job from re-entering itself.
 *
 * The `outpost_deadline` passed to every `outpost_client` RPC call is the
 * per-call deadline, not the cycle deadline. Concrete clients are required
 * to enforce it internally so a hung remote chain cannot starve the pool.
 */
class outpost_opp_job {
public:
   outpost_opp_job(outpost_client_ptr client,
                   depot_ops&         depot,
                   fc::microseconds   outpost_deadline);

   /// Read the WIRE-side pending outbound record for this outpost+epoch and,
   /// if present, call `client->deliver_outbound_envelope` with it. Emits the
   /// `DEPOT_OUTPOST_{ETHEREUM|SOLANA}` debug event when something was sent.
   /// No-op when the epoch window is closed or no envelope is pending.
   void run_outbound();

   /// Read inbound envelopes from the remote chain for the current epoch and,
   /// if any matched, push `sysio.msgch::deliver` via the depot. Emits the
   /// `OUTPOST_{ETHEREUM|SOLANA}_DEPOT` debug event when something was
   /// delivered. Skips when already delivered for this epoch or when the
   /// epoch window is closed.
   void run_inbound();

   /// For logging / job labels.
   const outpost_client& client() const { return *_client; }

private:
   outpost_client_ptr _client;
   depot_ops&         _depot;
   fc::microseconds   _outpost_deadline;
   std::mutex         _state_mx;

   /// Last epoch for which `run_outbound` attempted a delivery — used to
   /// avoid re-delivering the same envelope inside a single epoch across
   /// cron re-fires.
   uint32_t _last_outbound_epoch = 0;

   /// Last epoch for which a path-2 consensus-retry re-delivery was issued.
   /// Bounds the retry at one tx per epoch even when cron fires repeatedly
   /// after the boundary elapses but before consensus tips. The retry
   /// itself is idempotent on both the ETH and SOL outpost contracts:
   /// re-submitting the same envelope from the same operator re-runs the
   /// dual-path consensus check without re-recording the delivery. See
   /// .claude/rules/opp-consensus.md.
   uint32_t _last_consensus_retry_epoch = 0;
};

} // namespace sysio
