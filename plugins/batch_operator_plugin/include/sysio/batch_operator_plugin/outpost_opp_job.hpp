#pragma once

#include <fc/time.hpp>
#include <sysio/batch_operator_plugin/depot_ops.hpp>
#include <sysio/outpost_client/outpost_client.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

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
   /// Construct a job with a monotonic retry clock; tests may supply a manual clock.
   outpost_opp_job(outpost_client_ptr client, depot_ops& depot, fc::microseconds outpost_deadline,
                   std::function<std::chrono::steady_clock::time_point()> now = std::chrono::steady_clock::now);

   /// Read the WIRE-side pending outbound record for this outpost+epoch and,
   /// if present, call `client->deliver_outbound_envelope` with it. Emits the
   /// `DEPOT_OUTPOST_{ETHEREUM|SOLANA}` debug event when something was sent.
   /// No-op when the epoch window is closed or no envelope is pending.
   /// Successful submissions remain provisional: retry with 1..30 second capped
   /// exponential backoff until this operator's inbound delivery is recorded by
   /// the depot. Solana inbound reads use finalized commitment. The first observed
   /// epoch boundary resets the delay so fallback consensus is retriggered promptly.
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
   std::function<std::chrono::steady_clock::time_point()> _now;
   std::mutex         _state_mx;

   /// Last successful submission, used only to run the outpost's per-epoch cranks once.
   /// A signature, including a Solana processed success, never disables recovery.
   uint32_t _last_outbound_epoch = 0;

   /// Epoch owning the retry schedule; a new epoch can send immediately.
   uint32_t _outbound_retry_epoch = 0;
   /// Latch the first observed boundary so it resets backoff only once per epoch.
   bool _outbound_boundary_past = false;
   /// Nonblocking retry schedule, measured from the preceding attempt's completion.
   std::chrono::steady_clock::time_point _next_outbound_attempt{};
   /// Delay before the next retry; capped without limiting the number of attempts.
   std::chrono::steady_clock::duration _outbound_retry_backoff;
};

} // namespace sysio
