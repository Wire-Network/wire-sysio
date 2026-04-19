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
      uint64_t          outpost_id = 0;
      std::vector<char> raw_messages;
   };

   std::optional<sysio::outbound_envelope_record>
   read_pending_outbound(uint64_t outpost_id, uint32_t epoch_index) override {
      std::lock_guard<std::mutex> lock(_mx);
      read_pending_calls.push_back({outpost_id, epoch_index});
      if (pending_response) {
         return pending_response(outpost_id, epoch_index);
      }
      return std::nullopt;
   }

   bool has_delivered_envelope(uint64_t outpost_id, uint32_t epoch_index) override {
      std::lock_guard<std::mutex> lock(_mx);
      has_delivered_calls.push_back({outpost_id, epoch_index});
      if (has_delivered_response) {
         return has_delivered_response(outpost_id, epoch_index);
      }
      return false;
   }

   void deliver_to_depot(uint64_t                 outpost_id,
                         const std::vector<char>& raw_messages) override {
      std::lock_guard<std::mutex> lock(_mx);
      deliver_calls.push_back({outpost_id, raw_messages});
   }

   void emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent event) override {
      std::lock_guard<std::mutex> lock(_mx);
      emitted_events.push_back(std::move(event));
   }

   bool     within_epoch_window() const override { return window_open; }
   bool     is_elected()         const override { return elected; }
   uint32_t current_epoch()      const override { return epoch; }

   // Knobs callers can set before driving the job under test.
   bool     window_open = true;
   bool     elected     = true;
   uint32_t epoch       = 1;

   std::function<std::optional<sysio::outbound_envelope_record>(uint64_t, uint32_t)> pending_response;
   std::function<bool(uint64_t, uint32_t)>                                           has_delivered_response;

   // Call recorders
   struct key { uint64_t outpost_id; uint32_t epoch_index; };
   std::vector<key>          read_pending_calls;
   std::vector<key>          has_delivered_calls;
   std::vector<deliver_call> deliver_calls;
   std::vector<sysio::opp::debugging::DebugEnvelopeEvent> emitted_events;

private:
   mutable std::mutex _mx;
};

} // namespace sysio::test
