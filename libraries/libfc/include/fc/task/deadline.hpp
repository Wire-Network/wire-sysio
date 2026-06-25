#pragma once

#include <fc/time.hpp>

#include <optional>

namespace fc::task {

namespace detail {
   inline thread_local std::optional<fc::time_point> active_deadline;
} // namespace detail

/**
 * Thread-local absolute deadline inherited by nested retry and transport calls.
 *
 * The scope stores the earliest active deadline so nested helpers cannot extend
 * a caller's budget by installing a later one. This is intentionally
 * thread-local: OPP relay workers execute one synchronous chain call stack per
 * worker, while the shared chain clients may be referenced by other workers.
 */
class deadline_scope {
public:
   /**
    * Begin a scoped deadline.
    *
    * @param deadline_abs Absolute wall-clock deadline that nested helpers must
    *                     not exceed.
    */
   explicit deadline_scope(fc::time_point deadline_abs)
      : _previous(detail::active_deadline) {
      if (_previous && *_previous < deadline_abs) {
         detail::active_deadline = *_previous;
      } else {
         detail::active_deadline = deadline_abs;
      }
   }

   deadline_scope(const deadline_scope&) = delete;
   deadline_scope& operator=(const deadline_scope&) = delete;

   /**
    * Restore the previously active deadline, if any.
    */
   ~deadline_scope() {
      detail::active_deadline = _previous;
   }

private:
   std::optional<fc::time_point> _previous;
};

/**
 * Return the currently active thread-local deadline, if a caller installed one.
 */
inline std::optional<fc::time_point> current_deadline() {
   return detail::active_deadline;
}

/**
 * Clamp `deadline_abs` to the currently active scoped deadline.
 *
 * @param deadline_abs Local helper deadline.
 * @return The earliest of `deadline_abs` and the active scoped deadline.
 */
inline fc::time_point clamp_to_current_deadline(fc::time_point deadline_abs) {
   if (detail::active_deadline && *detail::active_deadline < deadline_abs) {
      return *detail::active_deadline;
   }
   return deadline_abs;
}

} // namespace fc::task
