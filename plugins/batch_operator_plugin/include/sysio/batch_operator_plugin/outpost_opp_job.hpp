#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

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
   /// @param clock the time `run_backlog` paces its reads and re-reports by: the wall clock unless a test injects
   ///              its own.
   outpost_opp_job(outpost_client_ptr              client,
                   depot_ops&                      depot,
                   std::function<fc::time_point()> clock = &fc::time_point::now);

   /// While this operator is elected on the chain's current epoch, deliver the
   /// depot's pending outbound envelope for it to the outpost
   /// (`client->deliver_outbound_envelope`), every cycle until the depot
   /// records this operator's inbound delivery of the epoch
   /// (`has_delivered_envelope`). The client sends nothing for a delivery the
   /// outpost already recorded, which awaits the rest of the group. The first
   /// call of an epoch that returns runs the outpost's per-epoch crank. Emits
   /// the `DEPOT_OUTPOST_{ETHEREUM|SOLANA}` debug event before each call. No-op
   /// when no envelope is pending.
   void run_outbound();

   /// While this operator is elected on the chain's current epoch, read the
   /// outpost's envelope for it and deliver it to the depot
   /// (`sysio.msgch::deliver`). Right after its delivery lands, asks the depot
   /// to push `sysio.msgch::chkcons` if the chain is due. Emits the
   /// `OUTPOST_{ETHEREUM|SOLANA}_DEPOT` debug event before each push. Skips an
   /// epoch already delivered.
   void run_inbound();

   /// Report the outpost's outbound backlog to the depot
   /// (`sysio.msgch::reportqueue`) while this operator is elected on the
   /// chain's current epoch N, the depot has decided N, and the outpost's
   /// latest emitted epoch is N with messages still queued, so no message that
   /// left in N's emit counts. The count and its age come from one outpost read.
   /// Reported once per epoch, and again while this operator's report for N
   /// stands and the chain stays idle a chain minimum after the last one, since
   /// a report re-runs the depot's tally over the eligible group. While nothing
   /// is reported for N, the outpost is read at most a quarter of the chain
   /// minimum apart rather than every cycle.
   void run_backlog();

   /// For logging / job labels.
   const outpost_client& client() const { return *_client; }

private:
   /// This operator's last backlog report on the chain: its epoch, and when it
   /// was made or, after a restart, first seen standing.
   struct backlog_report {
      uint32_t       epoch_index = 0;
      fc::time_point reported_at;
   };

   outpost_client_ptr              _client;
   depot_ops&                      _depot;
   std::function<fc::time_point()> _clock;
   /// Serializes `run_outbound` and `run_inbound`, and guards `_last_outbound_epoch`.
   std::mutex                      _state_mx;
   /// Guards `run_backlog`'s state below.
   std::mutex                      _backlog_mx;

   /// The epoch whose outpost crank ran, after its first outbound call that
   /// returned. Never gates a delivery.
   uint32_t _last_outbound_epoch = 0;
   /// See `backlog_report`; empty until the first report on the chain.
   std::optional<backlog_report> _backlog_report;
   /// When `run_backlog` last read the outpost's backlog; zero before its first read.
   fc::time_point _last_backlog_read;
};

} // namespace sysio
