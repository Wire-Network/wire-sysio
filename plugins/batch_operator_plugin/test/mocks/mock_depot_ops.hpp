#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include <sysio/batch_operator_plugin/depot_ops.hpp>

namespace sysio::test {

/**
 * @brief Recording/scripted implementation of depot_ops for unit tests.
 */
class mock_depot_ops : public sysio::depot_ops {
public:
   struct deliver_call {
      uint64_t          chain_code = 0;
      std::vector<char> raw_messages;
   };

   std::optional<sysio::outbound_envelope_record>
   read_pending_outbound(uint64_t chain_code, uint32_t epoch_index) override {
      std::lock_guard<std::mutex> lock(_mx);
      read_pending_calls.push_back({chain_code, epoch_index});
      if (pending_response) {
         return pending_response(chain_code, epoch_index);
      }
      return std::nullopt;
   }

   bool has_delivered_envelope(uint64_t chain_code, uint32_t epoch_index) override {
      std::lock_guard<std::mutex> lock(_mx);
      has_delivered_calls.push_back({chain_code, epoch_index});
      if (has_delivered_response) {
         return has_delivered_response(chain_code, epoch_index);
      }
      return false;
   }

   void deliver_to_depot(uint64_t                 chain_code,
                         const std::vector<char>& raw_messages) override {
      // `deliver_thrower` runs before recording so a test can simulate the
      // WIRE-side push_action failing without losing the call evidence —
      // any throw from the hook propagates to the caller, which is what
      // the production failure path looks like.
      if (deliver_thrower) deliver_thrower();
      std::lock_guard<std::mutex> lock(_mx);
      deliver_calls.push_back({chain_code, raw_messages});
   }

   void emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent event) override {
      // `emit_thrower` simulates a misbehaving signal slot. Ordered before
      // the recording step so a thrown event isn't logged as emitted.
      if (emit_thrower) emit_thrower();
      std::lock_guard<std::mutex> lock(_mx);
      emitted_events.push_back(std::move(event));
   }

   bool     within_epoch_window() const override { return window_open; }
   bool     is_elected()         const override { return elected; }
   uint32_t current_epoch()      const override { return epoch; }
   bool     is_epoch_boundary_past() const override { return epoch_boundary_past; }

   // Knobs callers can set before driving the job under test.
   bool     window_open = true;
   bool     elected     = true;
   uint32_t epoch       = 1;
   /// Drives the consensus-retry gate in `outpost_opp_job::run_outbound`.
   /// Default `false` keeps the existing tests' "no retry" expectation
   /// intact; tests targeting the retry path flip this to `true` to
   /// simulate wall-clock past `next_epoch_start`.
   bool     epoch_boundary_past = false;

   std::function<std::optional<sysio::outbound_envelope_record>(uint64_t, uint32_t)> pending_response;
   std::function<bool(uint64_t, uint32_t)>                                           has_delivered_response;

   /// Test-only seam: invoked at the top of `emit_debug_envelope` before
   /// any recording. If it throws, simulates a misbehaving signal slot —
   /// production code wraps the emit in FC_LOG_AND_DROP, so this lets us
   /// prove that a slot throw never breaks the surrounding work.
   std::function<void()> emit_thrower;

   /// Test-only seam: invoked at the top of `deliver_to_depot`. If it
   /// throws, simulates a transient WIRE-side push_action failure — used
   /// to drive the inbound run-through without recording the failed call.
   std::function<void()> deliver_thrower;

   // Call recorders
   struct key { uint64_t chain_code; uint32_t epoch_index; };
   std::vector<key>          read_pending_calls;
   std::vector<key>          has_delivered_calls;
   std::vector<deliver_call> deliver_calls;
   std::vector<sysio::opp::debugging::DebugEnvelopeEvent> emitted_events;

private:
   mutable std::mutex _mx;
};

} // namespace sysio::test
