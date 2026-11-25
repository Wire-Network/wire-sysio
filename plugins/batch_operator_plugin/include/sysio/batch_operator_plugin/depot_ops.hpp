#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fc/time.hpp>

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
    * inbound path so a retrying cron job does not double-deliver, and as the
    * outbound path's completion evidence.
    */
   virtual bool has_delivered_envelope(uint64_t chain_code, uint32_t epoch_index) = 0;

   /**
    * Push `sysio.msgch::deliver` with the concatenated inbound envelope bytes.
    * Synchronous — blocks on the action's future for at most the chain's
    * delivery timeout. True when the depot accepted the push; a refusal is
    * logged by the push.
    */
   virtual bool deliver_to_depot(uint64_t                 chain_code,
                                 const std::vector<char>& raw_messages) = 0;

   /**
    * Push `sysio.msgch::chkcons` for outpost `chain_code` when its last-read
    * epoch state shows it due (`batch_operator_detail::pushes_chkcons_after_delivery`).
    * Called right after this operator's own delivery of the chain's current
    * epoch lands, which may have decided it, so a due chain advances without
    * waiting for the next epoch tick.
    */
   virtual void push_chkcons_if_due(uint64_t chain_code) = 0;

   /**
    * Whether this operator already reported outpost `chain_code`'s outbound
    * backlog in epoch `epoch_index`: its `sysio.msgch::qreports` row for the
    * chain was made in that epoch. False when the read failed.
    */
   virtual bool has_reported_backlog(uint64_t chain_code, uint32_t epoch_index) = 0;

   /**
    * Push `sysio.msgch::reportqueue` for outpost `chain_code`'s epoch
    * `epoch_index`: `pending_count` messages queued on the outpost since
    * `oldest_pending_ms` (milliseconds since the Unix epoch; zero for unknown).
    * True when the depot accepted it. A refusal is expected when the chain
    * moved on since the epoch state was read, and is logged at info.
    */
   virtual bool report_backlog(uint64_t chain_code, uint32_t epoch_index, uint32_t pending_count,
                               uint64_t oldest_pending_ms) = 0;

   /**
    * Fire the batch-operator debug-envelope signal with the given event.
    * A no-op when no slots are connected, so the job never has to check.
    */
   virtual void emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent event) = 0;

   /**
    * True when this batch operator is elected to drive OPP deliveries for
    * outpost `chain_code`'s current epoch: it sits in the group on duty in that
    * chain's own schedule window and the chain is not paused. Only elected
    * operators should call into the outpost client or the WIRE-side depot
    * actions; non-elected jobs bail early to avoid racing / wasted RPCs /
    * rejected chain transactions.
    */
   virtual bool is_elected(uint64_t chain_code) const = 0;

   /**
    * The most recent `current_epoch_index` read from outpost `chain_code`'s
    * `sysio.epoch::epochstate` row, or 0 while it has not been read. Cached by
    * the depot implementation; refreshed by the epoch tick.
    */
   virtual uint32_t current_epoch(uint64_t chain_code) const = 0;

   /**
    * True when outpost `chain_code`'s current epoch has its accepted winner
    * (`sysio.msgch::outpcons` records it). False while the chain's epoch state
    * has not been read.
    */
   virtual bool is_decided(uint64_t chain_code) const = 0;

   /**
    * True when outpost `chain_code` has traffic recorded, so it is or will be
    * due (`epochstate.due_at` is set). False while it is idle or its epoch
    * state has not been read.
    */
   virtual bool has_traffic(uint64_t chain_code) const = 0;

   /**
    * Outpost `chain_code`'s minimum duration between two epochs
    * (`epochcfg.epoch_duration_sec`), or zero while its epoch state has not
    * been read.
    */
   virtual fc::microseconds minimum_duration(uint64_t chain_code) const = 0;

   /**
    * How long each call to outpost `chain_code`, and its push of
    * `sysio.msgch::deliver`, may take: derived from the chain's on-chain epoch
    * duration (`batch_operator_detail::derive_cadence`). The longest delivery
    * timeout while the chain's epoch state has not been read.
    */
   virtual fc::microseconds delivery_timeout(uint64_t chain_code) const = 0;
};

} // namespace sysio
