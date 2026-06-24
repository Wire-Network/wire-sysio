#pragma once

#include <cstddef>
#include <deque>
#include <string_view>

namespace sysio::state_history {

/// Maximum status request queue depth before a SHiP session is closed.
inline constexpr std::size_t max_status_request_queue_depth = 1024;

/// Error text used when a SHiP client exceeds the per-session status request queue limit.
inline constexpr std::string_view status_request_queue_limit_exceeded =
      "state history status request queue limit exceeded";

/** Bounded FIFO for pending SHiP status requests. */
class status_request_queue {
public:
   /// Attempts to append one status request, returning false when the queue depth limit has been reached.
   [[nodiscard]] bool try_append(bool is_v1_request) {
      if(requests.size() >= max_status_request_queue_depth)
         return false;
      requests.emplace_back(is_v1_request);
      return true;
   }

   /// Removes and returns all pending status requests while leaving the queue reusable.
   std::deque<bool> pop_all() {
      std::deque<bool> pending;
      pending.swap(requests);
      return pending;
   }

   /// Returns the number of queued status requests.
   std::size_t size() const {
      return requests.size();
   }

   /// Returns true when no status requests are queued.
   bool empty() const {
      return requests.empty();
   }

private:
   std::deque<bool> requests; ///< false for v0 requests, true for v1 requests.
};

} // namespace sysio::state_history
