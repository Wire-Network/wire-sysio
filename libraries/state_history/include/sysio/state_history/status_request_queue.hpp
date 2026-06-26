#pragma once

#include <cstddef>
#include <string_view>

namespace sysio::state_history {

/// Maximum status request queue depth before a SHiP session is closed.
inline constexpr std::size_t max_status_request_queue_depth = 1024;

/// Error text used when a SHiP client exceeds the per-session status request queue limit.
inline constexpr std::string_view status_request_queue_limit_exceeded =
      "state history status request queue limit exceeded";

/// SHiP status request protocol version.
enum class status_request_version {
   v0,
   v1,
};

/** Counts pending SHiP status requests by protocol version for one queue drain. */
struct pending_status_request_counts {
   std::size_t v0 = 0; ///< Number of pending v0 status requests.
   std::size_t v1 = 0; ///< Number of pending v1 status requests.

   /// Returns the total number of pending status requests.
   std::size_t size() const {
      return v0 + v1;
   }

   /// Returns true when no status requests are pending.
   bool empty() const {
      return size() == 0;
   }
};

/** Bounded counter queue for pending SHiP status requests. */
class status_request_queue {
public:
   /// Attempts to append one status request, returning false when the queue depth limit has been reached.
   [[nodiscard]] bool try_append(status_request_version version) {
      if(requests.size() >= max_status_request_queue_depth)
         return false;

      switch(version) {
      case status_request_version::v0:
         ++requests.v0;
         break;
      case status_request_version::v1:
         ++requests.v1;
         break;
      }

      return true;
   }

   /// Removes and returns all pending status requests while leaving the queue reusable.
   pending_status_request_counts pop_all() {
      const pending_status_request_counts pending = requests;
      requests = {};
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
   pending_status_request_counts requests; ///< Counted pending requests since the last drain.
};

} // namespace sysio::state_history
