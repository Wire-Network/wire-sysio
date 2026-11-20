#pragma once

#include "async_action_completion.hpp"
#include "underwriter.hpp"

namespace sysio::batch_operator_detail::underwriter {

/**
 * Crank on a new depot epoch and continue on the next poll after a productive trace. The contract owns queue
 * traversal, refill and freeze rules. A crank that only advances an internal cursor waits for the next epoch.
 * All scheduling state belongs to the underwriter job; its retained completion is safe to read after a timeout.
 */
struct synd_crank_state {
   std::optional<uint32_t> epoch;
   fc::time_point attempted_at;
   std::shared_ptr<async_action_completion> completion;

   /// Missing epoch observations disable only that trigger. The cord does not suppress safe contract bookkeeping.
   bool due(const plan& actions, const std::vector<request>& requests, std::optional<uint32_t> current_epoch,
            fc::time_point now, fc::microseconds idle_interval) const {
      const bool sync = std::ranges::any_of(requests, [](const request& row) {
         return is_envelope_request(row) && is_terminal(row.state) && !row.outcome_acknowledged;
      });
      return !actions.accepts.empty() || !actions.approves.empty() || sync ||
             (current_epoch && epoch != current_epoch) ||
             (completion && completion->has_inline_actions()) || now - attempted_at >= idle_interval;
   }

   /// Record the decision's clock/epoch even on an uncertain push; each new attempt consumes prior trace progress.
   void note(std::optional<uint32_t> current_epoch, fc::time_point now) {
      if (current_epoch) epoch = current_epoch;
      attempted_at = now;
      completion.reset();
   }
};

} // namespace sysio::batch_operator_detail::underwriter
