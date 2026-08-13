#pragma once

#include <fc/mock_time.hpp>

#include <boost/asio/basic_deadline_timer.hpp>
#include <boost/date_time/gregorian/gregorian_types.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>

namespace fc {

/**
 * Boost.Asio TimeTraits that follow fc's mock-time latch.
 *
 * Until a test calls fc::mock_time_traits::set_now() these are ordinary wall clock traits, and a
 * timer built on them behaves exactly like a real one: to_posix_duration() hands Asio the true
 * remaining delay, so it sleeps once and wakes at the deadline. Once the latch is engaged the clock
 * becomes the virtual one fc::time_point::now() already reports, and to_posix_duration() collapses
 * to a short poll so each wake re-reads the virtual clock and the timer fires as soon as a test
 * advances time past its expiry.
 *
 * This lets production and test share a single timer type. There is no build variant, no test only
 * option and no runtime polymorphism, only the same latch fc::time_point::now() already consults, so
 * production timing is unchanged and a test engages the virtual clock simply by calling set_now().
 */
struct mockable_time_traits {
   typedef boost::posix_time::ptime         time_type;
   typedef boost::posix_time::time_duration duration_type;

   /// Poll interval used only while mock time is engaged. mock_time_traits::set_now() sleeps for
   /// longer than this, so a caller that advances the clock can rely on the io_context having had
   /// the opportunity to notice before set_now() returns.
   static constexpr int mock_poll_interval_ms = 1;

   static time_type now() {
      return mock_time_traits::is_set() ? mock_time_traits::now()
                                        : boost::posix_time::microsec_clock::universal_time();
   }

   static time_type     add( const time_type& t, const duration_type& d ) { return t + d; }
   static duration_type subtract( const time_type& t1, const time_type& t2 ) { return t1 - t2; }
   static bool          less_than( const time_type& t1, const time_type& t2 ) { return t1 < t2; }

   /// How long Asio should wait before re-evaluating now(). Under the real clock that is the whole
   /// remaining delay; under mock time it has to be a short poll, because virtual time only moves
   /// when a test moves it and Asio would otherwise sleep out the delay in real time.
   static duration_type to_posix_duration( const duration_type& d ) {
      return mock_time_traits::is_set() ? boost::posix_time::milliseconds( mock_poll_interval_ms ) : d;
   }

   /// Convert an fc::time_point for use with expires_at(). Mirrors the unix epoch that
   /// mock_time_traits counts from, so mocked and real deadlines share one frame of reference.
   static time_type to_time_type( const fc::time_point& t ) {
      static const time_type unix_epoch{ boost::gregorian::date( 1970, 1, 1 ) };
      return unix_epoch + boost::posix_time::microseconds( t.time_since_epoch().count() );
   }
};

/// Deadline timer that runs on the real clock in production and on fc's mock clock under test.
using mockable_deadline_timer = boost::asio::basic_deadline_timer<boost::posix_time::ptime, mockable_time_traits>;

} // namespace fc
