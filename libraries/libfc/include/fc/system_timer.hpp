#pragma once

#include <fc/mock_time.hpp>
#include <fc/time.hpp>

#include <boost/asio/basic_waitable_timer.hpp>

#include <chrono>

namespace fc {

/**
 * A chrono clock that reports whatever fc::time_point::now() reports.
 *
 * That is the wall clock ordinarily, and the virtual clock once a test has engaged fc's mock time
 * latch with mock_time_traits::set_now(), so a timer built on this clock follows a test's notion of
 * "now" without any of its callers knowing.
 *
 * Its epoch is fc::time_point's, which is the unix epoch, matching std::chrono::system_clock.
 */
struct system_clock {
   using duration                  = std::chrono::system_clock::duration;
   using rep                       = duration::rep;
   using period                    = duration::period;
   using time_point                = std::chrono::time_point<system_clock, duration>;
   static constexpr bool is_steady = false;

   static time_point now() noexcept { return from_time_point( fc::time_point::now() ); }

   /// Convert an fc::time_point for use with the timer's expires_at().
   ///
   /// Goes through fc::time_point::to_system_clock() rather than converting the microsecond count
   /// directly, because that conversion has to be clamped: this clock's duration is finer than a
   /// microsecond on some platforms, nanoseconds on Linux, so scaling fc::time_point::maximum() up
   /// overflows and wraps to a moment in the past. A deadline meant to be unreachable would then be
   /// one that has already passed, which turns a dormant timer into one that fires at once.
   static time_point from_time_point( const fc::time_point& t ) noexcept {
      return time_point{ t.to_system_clock().time_since_epoch() };
   }
};

/**
 * Wait traits that keep a timer responsive to virtual time.
 *
 * Asio asks how long it may sleep before re-evaluating the clock. Under the real clock the honest
 * answer is the whole remaining delay, so the timer sleeps once and wakes at its deadline exactly as
 * an ordinary steady_timer or system_timer does, and production timing is unaffected.
 *
 * Under mock time that answer would be wrong: virtual time only moves when a test moves it, so a
 * timer told to sleep out the delay would sleep it out in real time instead. Returning a short poll
 * makes Asio re-read the clock frequently, and the timer then fires as soon as a test advances
 * virtual time past its expiry. mock_time_traits::set_now() sleeps for longer than this interval, so
 * a caller that advances the clock can rely on the io_context having had the chance to notice.
 *
 * Production and test therefore share one timer type: no build variant, no test only option and no
 * runtime polymorphism, only the same latch fc::time_point::now() already consults.
 */
struct system_timer_wait_traits {
   /// Poll interval used only while mock time is engaged.
   static constexpr std::chrono::milliseconds mock_poll_interval{1};

   static system_clock::duration to_wait_duration( const system_clock::duration& d ) {
      return mock_time_traits::is_set() ? std::chrono::duration_cast<system_clock::duration>( mock_poll_interval ) : d;
   }

   static system_clock::duration to_wait_duration( const system_clock::time_point& t ) {
      return to_wait_duration( t - system_clock::now() );
   }
};

/// Drop-in for boost::asio::system_timer that runs on fc's clock: the wall clock in production and
/// the virtual clock once a test has engaged fc's mock time latch.
using system_timer = boost::asio::basic_waitable_timer<system_clock, system_timer_wait_traits>;

} // namespace fc
