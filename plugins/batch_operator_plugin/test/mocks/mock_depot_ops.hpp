#pragma once

#include <functional>
#include <map>
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
      fc::microseconds  timeout;
   };
   struct report_call {
      uint64_t chain_code        = 0;
      uint32_t epoch_index       = 0;
      uint32_t pending_count     = 0;
      uint64_t oldest_pending_ms = 0;
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

   sysio::push_outcome deliver_to_depot(uint64_t                 chain_code,
                                        const std::vector<char>& raw_messages,
                                        fc::microseconds         timeout) override {
      // `deliver_thrower` runs before recording so a test can simulate the
      // WIRE-side push_action failing without losing the call evidence —
      // any throw from the hook propagates to the caller, which is what
      // the production failure path looks like.
      if (deliver_thrower) deliver_thrower();
      std::lock_guard<std::mutex> lock(_mx);
      deliver_calls.push_back({chain_code, raw_messages, timeout});
      return deliver_outcome;
   }

   void push_chkcons_if_due(uint64_t chain_code) override {
      std::lock_guard<std::mutex> lock(_mx);
      chkcons_if_due_calls.push_back(chain_code);
   }

   bool has_reported_backlog(uint64_t chain_code, uint32_t epoch_index) override {
      std::lock_guard<std::mutex> lock(_mx);
      has_reported_calls.push_back({chain_code, epoch_index});
      return has_reported_response ? has_reported_response(chain_code, epoch_index) : false;
   }

   bool report_backlog(uint64_t chain_code, uint32_t epoch_index, uint32_t pending_count,
                       uint64_t oldest_pending_ms) override {
      std::lock_guard<std::mutex> lock(_mx);
      report_calls.push_back({chain_code, epoch_index, pending_count, oldest_pending_ms});
      return report_accepted;
   }

   void emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent event) override {
      // `emit_thrower` simulates a misbehaving signal slot. Ordered before
      // the recording step so a thrown event isn't logged as emitted.
      if (emit_thrower) emit_thrower();
      std::lock_guard<std::mutex> lock(_mx);
      emitted_events.push_back(std::move(event));
   }

   bool is_elected(uint64_t chain_code) const override { return knobs_of(chain_code).elected; }
   uint32_t current_epoch(uint64_t chain_code) const override { return knobs_of(chain_code).epoch; }
   /// The chain's `late_window_remaining` while it owes a late delivery, else zero.
   fc::microseconds late_window_left(uint64_t chain_code) const override {
      const chain_knobs knobs = knobs_of(chain_code);
      return knobs.owes_late_delivery ? knobs.late_window_remaining : fc::microseconds();
   }
   /// Elected, or reporting for the chain's next group.
   bool reports_backlog(uint64_t chain_code) const override {
      const chain_knobs knobs = knobs_of(chain_code);
      return knobs.elected || knobs.next_group_reports;
   }
   bool is_decided(uint64_t chain_code) const override { return knobs_of(chain_code).decided; }
   fc::microseconds minimum_duration(uint64_t chain_code) const override {
      return knobs_of(chain_code).chain_minimum;
   }
   /// The chain's own knob, else `default_delivery_timeout`. Not an epoch query, so not recorded in `epoch_queries`.
   fc::microseconds delivery_timeout(uint64_t chain_code) const override {
      std::lock_guard<std::mutex> lock(_mx);
      const auto it = chains.find(chain_code);
      return it == chains.end() ? default_delivery_timeout : it->second.delivery_timeout;
   }

   /// The delivery timeout of every chain without its own knobs.
   static constexpr fc::microseconds default_delivery_timeout = fc::seconds(2);
   /// The minimum duration of every chain without its own knobs.
   static constexpr fc::microseconds default_minimum_duration = fc::seconds(60);
   /// What is left of the late-delivery window of every chain without its own knobs while it owes one: longer than
   /// the delivery timeout, so the timeout bounds the late delivery.
   static constexpr fc::microseconds default_late_window_remaining = fc::seconds(10);

   /// One chain's epoch and election, as the epoch queries answer them.
   struct chain_knobs {
      bool     elected               = true;
      uint32_t epoch                 = 1;
      bool     owes_late_delivery    = false;   ///< whether `late_window_left` answers `late_window_remaining`
      bool     decided               = false;
      fc::microseconds chain_minimum = default_minimum_duration;   ///< what `minimum_duration` answers
      /// What `delivery_timeout` answers: the deadline the job hands every outpost call.
      fc::microseconds delivery_timeout = default_delivery_timeout;
      /// What `late_window_left` answers while `owes_late_delivery`.
      fc::microseconds late_window_remaining = default_late_window_remaining;
      /// Whether `reports_backlog` answers true without `elected`: the operator reports for the chain's next group.
      bool             next_group_reports    = false;
   };

   // Knobs callers can set before driving the job under test. They answer for
   // every chain without its own entry in `chains`.
   bool             elected               = true;
   uint32_t         epoch                 = 1;
   bool             owes_late_delivery    = false;
   bool             decided               = false;
   fc::microseconds chain_minimum         = default_minimum_duration;
   fc::microseconds late_window_remaining = default_late_window_remaining;
   bool             next_group_reports    = false;
   /// What `deliver_to_depot` answers once it records the call.
   sysio::push_outcome deliver_outcome = sysio::push_outcome::accepted;
   /// What `report_backlog` answers once it records the call.
   bool             report_accepted   = true;

   /// Per-chain knobs, overriding the ones above for their chain.
   std::map<uint64_t, chain_knobs> chains;

   std::function<std::optional<sysio::outbound_envelope_record>(uint64_t, uint32_t)> pending_response;
   std::function<bool(uint64_t, uint32_t)>                                           has_delivered_response;
   std::function<bool(uint64_t, uint32_t)>                                           has_reported_response;

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
   std::vector<key>          has_reported_calls;
   std::vector<deliver_call> deliver_calls;
   std::vector<report_call>  report_calls;
   /// The chain of every `push_chkcons_if_due` call, in call order.
   std::vector<uint64_t>     chkcons_if_due_calls;
   std::vector<sysio::opp::debugging::DebugEnvelopeEvent> emitted_events;
   /// The chain each epoch or election query asked about, in call order.
   mutable std::vector<uint64_t> epoch_queries;

private:
   /// `chain_code`'s knobs, recording the query.
   chain_knobs knobs_of(uint64_t chain_code) const {
      std::lock_guard<std::mutex> lock(_mx);
      epoch_queries.push_back(chain_code);
      const auto it = chains.find(chain_code);
      if (it != chains.end()) return it->second;
      return chain_knobs{.elected               = elected,
                         .epoch                 = epoch,
                         .owes_late_delivery    = owes_late_delivery,
                         .decided               = decided,
                         .chain_minimum         = chain_minimum,
                         .late_window_remaining = late_window_remaining,
                         .next_group_reports    = next_group_reports};
   }

   mutable std::mutex _mx;
};

} // namespace sysio::test
