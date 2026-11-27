#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

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
 * constructs one per outpost chain the operator relays and schedules three cron
 * jobs against it, each at that chain's poll interval:
 *
 *   - `outpost_opp_outbound_<id>` calls `run_outbound()` every cycle.
 *   - `outpost_opp_inbound_<id>`  calls `run_inbound()`  every cycle.
 *   - `outpost_opp_backlog_<id>`  calls `run_backlog()`  every cycle.
 *
 * The jobs can fire on different cron worker threads; the internal
 * `_state_mx` serializes the outbound and inbound jobs. The backlog job keeps
 * its state under its own `_backlog_mx`, so its outpost read and depot push
 * never hold up a delivery. `one_at_a_time` on each cron job prevents a single
 * job from re-entering itself.
 *
 * Each outpost chain runs its own consensus epoch, so every epoch and election
 * query the job makes of `depot_ops` names the job's own chain
 * (`client->chain_code()`). A chain's epoch advances once its traffic makes it
 * due, so the jobs hold nothing back within an epoch: they act whenever the
 * chain's state calls for it.
 *
 * Every `outpost_client` call gets the chain's delivery timeout
 * (`depot_ops::delivery_timeout`, from its on-chain epoch duration) as its
 * deadline, read each cycle. Concrete clients are required to enforce it
 * internally so a hung remote chain cannot starve the pool.
 */
class outpost_opp_job {
public:
   /// @param clock the time the job paces its outpost cranks and backlog reads by: the wall clock unless a test injects
   ///              its own.
   outpost_opp_job(outpost_client_ptr              client,
                   depot_ops&                      depot,
                   std::function<fc::time_point()> clock = &fc::time_point::now);

   /// While this operator is elected on the chain's current epoch, deliver the
   /// depot's pending outbound envelope for it to the outpost
   /// (`client->deliver_outbound_envelope`), every cycle until the depot
   /// records this operator's inbound delivery of the epoch
   /// (`has_delivered_envelope`). The client sends nothing for a delivery the
   /// outpost already recorded, which awaits the rest of the group. Emits the
   /// `DEPOT_OUTPOST_{ETHEREUM|SOLANA}` debug event before each call; nothing is
   /// delivered when no envelope is pending. Then, at most once per chain
   /// minimum whether or not the chain advances, runs the outpost's crank
   /// (`client->crank_outpost`), so what the outpost accrues on an idle chain is
   /// still queued.
   void run_outbound();

   /// Deliver the outpost's envelope to the depot (`sysio.msgch::deliver`): for
   /// the previous epoch while this operator sat in that epoch's group, the
   /// chain's late-delivery window is open (`depot_ops::late_window_left`) and
   /// the depot holds no delivery of its for it, which the depot credits as a
   /// late delivery; then for the chain's current epoch while this operator is
   /// elected on it. The late delivery goes first, its outpost read and depot
   /// push bounded by what is left of the window. The outpost keeps only its
   /// latest emitted envelope, so the bytes read for an epoch are kept while
   /// the epoch is current, then while its late-delivery window is open,
   /// unless the depot refuses their late delivery.
   /// Right after its own current-epoch delivery lands, asks the depot to push
   /// `sysio.msgch::chkcons` if the chain is due. Emits the
   /// `OUTPOST_{ETHEREUM|SOLANA}_DEPOT` debug event before each push. Skips an
   /// epoch already delivered.
   void run_inbound();

   /// Report the outpost's outbound backlog to the depot
   /// (`sysio.msgch::reportqueue`) while this operator reports for the chain
   /// (`depot_ops::reports_backlog`: elected on its current epoch N, or in its
   /// next group while the chain has stayed not due), the depot has decided N,
   /// and the outpost's latest emitted epoch is N with messages still queued,
   /// so no message that left in N's emit counts. The count and its age come
   /// from one outpost read.
   /// Reported once per epoch. While nothing is reported for N, the outpost is
   /// read at most a quarter of the chain minimum apart rather than every cycle.
   void run_backlog();

   /// For logging / job labels.
   const outpost_client& client() const { return *_client; }

private:
   /// Deliver the depot's pending outbound envelope for `epoch` to the outpost, unless the depot already holds this
   /// operator's inbound delivery of it. A failure is logged and the next cycle tries again.
   void deliver_outbound(uint64_t chain_code, uint32_t epoch);

   /// Run the outpost's crank (`client->crank_outpost`) for `epoch` unless this job ran it less than a chain minimum
   /// ago. A failure is logged and spends the chain minimum all the same.
   void crank_outpost(uint64_t chain_code, uint32_t epoch);

   /// Deliver the outpost's envelope for `epoch_index` to the depot unless the
   /// depot already holds this operator's delivery of it, the outpost read and
   /// the depot push each bounded by `timeout`. `late` marks the previous
   /// epoch's delivery: its bytes are the kept ones when there are any, and no
   /// `chkcons` follows it.
   void deliver_inbound(uint64_t chain_code, uint32_t epoch_index, bool late, fc::microseconds timeout);

   outpost_client_ptr              _client;
   depot_ops&                      _depot;
   std::function<fc::time_point()> _clock;
   /// Serializes `run_outbound` and `run_inbound`, and guards `_last_crank` and `_inbound_envelopes`.
   std::mutex                      _state_mx;
   /// Guards `run_backlog`'s state below.
   std::mutex                      _backlog_mx;

   /// When `run_outbound` last ran the outpost's crank; zero before its first. Never gates a delivery.
   fc::time_point _last_crank;
   /// The envelope bytes read from the outpost for each epoch this operator may
   /// still owe the depot: the current epoch, and the previous one while its
   /// late-delivery window is open and the depot has not refused them.
   std::map<uint32_t, std::vector<char>> _inbound_envelopes;
   /// When `run_backlog` last read the outpost's backlog; zero before its first read.
   fc::time_point _last_backlog_read;
};

} // namespace sysio
