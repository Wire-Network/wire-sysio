#pragma once

#include <atomic>
#include <future>
#include <utility>

namespace sysio::batch_operator_detail {

/// Owns completion notification for an asynchronous batch-operator action.
///
/// The state is intentionally shared with the completion callback so a caller
/// may time out and destroy its future without leaving the callback with a
/// dangling promise or permitting a second completion to throw.
class async_action_completion {
public:
   /// Returns the single future that waits for this action's callback.
   std::future<void> get_future() {
      return done.get_future();
   }

   /// Invokes `on_complete` once, suppresses callback exceptions, and signals
   /// the waiter without allowing duplicate asynchronous completions to throw.
   template <typename CompletionHandler>
   bool complete(CompletionHandler&& on_complete) noexcept {
      if (completed.exchange(true, std::memory_order_acq_rel)) {
         return false;
      }

      try {
         std::forward<CompletionHandler>(on_complete)();
      } catch (...) {
         // A logging or formatting failure must not escape the relay callback.
      }

      try {
         done.set_value();
      } catch (...) {
         // The one-shot guard prevents future_error in normal operation.
      }
      return true;
   }

private:
   std::atomic<bool> completed{false};
   std::promise<void> done;
};

} // namespace sysio::batch_operator_detail
