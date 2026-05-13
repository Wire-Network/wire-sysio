#pragma once

#include <fc/time.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>

namespace fc {

/// mock out fc::time_point::now() so test code can advance "now" deterministically
/// without sleeping. Used by fc::time_point::now() when is_set() is true.
class mock_time_traits {
public:
   typedef boost::posix_time::ptime         time_type;
   typedef boost::posix_time::time_duration duration_type;

   // Requires set_now() to be called first on main thread before any calls to fc::time_point::now()
   static time_type now() noexcept;

   // First call should be on one thread before any calls to fc::time_point::now()
   static void set_now( time_type t );
   static void set_now( const fc::time_point& now );

   // Thread safe only if first call to set_now is before any threads are spawned or memory barrier introduced
   static bool is_set() { return mock_enabled_; }

   // return now as fc::time_point, used by fc::time_point::now() if mock_time_traits is_set()
   static fc::time_point fc_now();

private:
   static bool mock_enabled_;
   static const boost::posix_time::ptime epoch_;
   static std::atomic<int64_t> now_;
};

} // namespace fc
