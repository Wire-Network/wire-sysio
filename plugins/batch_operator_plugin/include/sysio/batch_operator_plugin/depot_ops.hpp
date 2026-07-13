#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <sysio/batch_operator_plugin/debug_envelope_event.hpp>

namespace sysio {

/**
 * @brief A pending outbound envelope read from `sysio.msgch::outenvelopes`.
 *
 * Populated by `depot_ops::read_pending_outbound` and handed to
 * `outpost_opp_job::run_outbound`, which forwards the raw bytes to the
 * concrete `outpost_client::deliver_outbound_envelope`.
 */
struct outbound_envelope_record {
   uint64_t          chain_code        = 0;
   uint32_t          epoch_index       = 0;
   /// Canonical epoch digest of the envelope (hex): keccak256 of `raw_envelope`, matching the
   /// receiving outpost's consensus digest and the next emit's `previous_envelope_hash`.
   std::string       envelope_hash_hex;
   std::vector<char> raw_envelope;
};

/**
 * @brief WIRE-side orchestration contract consumed by `outpost_opp_job`.
 *
 * Every method here reads or writes WIRE chain state — they belong to
 * `batch_operator_plugin`, not the cross-chain `outpost_client` SPI. The
 * plugin's internal `impl` struct supplies the canonical implementation;
 * tests supply a recording mock (`mock_depot_ops`).
 *
 * Every method is intended to be called on a cron worker thread. Concrete
 * implementations are responsible for their own thread-safety when needed.
 */
struct depot_ops {
   virtual ~depot_ops() = default;

   /**
    * Look up the pending outbound envelope that this outpost should deliver
    * in the given epoch. Returns std::nullopt when `sysio.msgch::outenvelopes`
    * holds no matching row (no outbound traffic this cycle).
    */
   virtual std::optional<outbound_envelope_record>
   read_pending_outbound(uint64_t chain_code, uint32_t epoch_index) = 0;

   /**
    * Ask `sysio.msgch::envelopes` whether we already pushed a
    * `sysio.msgch::deliver` for this outpost in this epoch. Used to guard the
    * inbound path so a retrying cron job does not double-deliver.
    */
   virtual bool has_delivered_envelope(uint64_t chain_code, uint32_t epoch_index) = 0;

   /**
    * Push `sysio.msgch::deliver` with the concatenated inbound envelope bytes.
    * Synchronous — blocks on the action's future until the configured
    * delivery timeout elapses.
    */
   virtual void deliver_to_depot(uint64_t                 chain_code,
                                 const std::vector<char>& raw_messages) = 0;

   /**
    * Fire the batch-operator debug-envelope signal with the given event.
    * A no-op when no slots are connected, so the job never has to check.
    */
   virtual void emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent event) = 0;

   /**
    * True when the current wall-clock time is in the safe operating window for
    * the current epoch (far enough from both edges). The inbound and outbound
    * jobs gate themselves on this.
    */
   virtual bool within_epoch_window() const = 0;

   /**
    * True when this batch operator is the one elected to drive OPP deliveries
    * for the current epoch. Only elected operators should call into the
    * outpost clients or the WIRE-side depot actions; non-elected jobs bail
    * early to avoid racing / wasted RPCs / rejected chain transactions.
    */
   virtual bool is_elected() const = 0;

   /**
    * The most recent `current_epoch_index` read from `sysio.epoch::epochstate`.
    * Cached by the depot implementation; refreshed on the `epoch_tick` job.
    */
   virtual uint32_t current_epoch() const = 0;

   /**
    * True when wall-clock has advanced past the depot's `next_epoch_start` for
    * the currently-cached epoch. Used by the outpost cranker to decide when
    * a consensus-retry re-delivery is warranted on a stuck path-2 fallback
    * case: once the WIRE depot's boundary has elapsed, the outpost-side
    * boundary (same `epoch_duration_sec`) is also definitively past, so a
    * re-delivery from an already-delivered operator can re-trigger the
    * outpost's consensus evaluation and tip path-2 fallback. Without this
    * gate the cranker would have to either re-deliver every tick (gas
    * waste) or never re-deliver (chain stalls when initial deliveries fall
    * short of unanimous). Mirrors the WIRE-side `sysio.msgch::chkcons`
    * "time gate passed" check.
    *
    * Returns false when the cache is empty (pre-bootstrap) or when the
    * boundary has not yet elapsed for the current epoch.
    */
   virtual bool is_epoch_boundary_past() const = 0;
};

} // namespace sysio
