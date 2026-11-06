/**
 * Berkeley SoftFloat keeps the rounding mode and exception flags in per-thread state (softfloat 3.0.0#3): a
 * worker thread starts from the defaults and leaks nothing back to the thread that spawned it. With the
 * process-global state of 3.0.0#2 every check below fails.
 */
extern "C" {
#include <softfloat/softfloat.h>
}
#include <softfloat/softfloat.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <thread>

namespace {

const softfloat32_t one = {0x3f800000u};
const softfloat32_t zero = {0u};
const softfloat32_t minus_one = {0xbf800000u};

/// Other suites may leave the calling thread's state behind; start from the defaults and put them back.
struct softfloat_state_guard {
   softfloat_state_guard() { reset(); }
   ~softfloat_state_guard() { reset(); }
   static void reset() {
      softfloat_roundingMode = softfloat_round_near_even;
      softfloat_exceptionFlags = 0;
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(softfloat_thread_local_tests)

BOOST_AUTO_TEST_CASE(rounding_mode_and_exception_flags_are_per_thread) {
   softfloat_state_guard guard;

   softfloat_roundingMode = softfloat_round_max;
   f32_div(one, zero);
   BOOST_CHECK_EQUAL(unsigned(softfloat_exceptionFlags), unsigned(softfloat_flag_infinite));

   unsigned worker_mode = ~0u;
   unsigned worker_flags_before = ~0u;
   unsigned worker_flags_after = 0;
   std::thread worker([&] {
      worker_mode = softfloat_roundingMode;
      worker_flags_before = softfloat_exceptionFlags;
      softfloat_roundingMode = softfloat_round_minMag;
      f32_sqrt(minus_one);
      worker_flags_after = softfloat_exceptionFlags;
   });
   worker.join();

   BOOST_CHECK_EQUAL(worker_mode, unsigned(softfloat_round_near_even));
   BOOST_CHECK_EQUAL(worker_flags_before, 0u);
   BOOST_CHECK_EQUAL(worker_flags_after, unsigned(softfloat_flag_invalid));
   BOOST_CHECK_EQUAL(unsigned(softfloat_roundingMode), unsigned(softfloat_round_max));
   BOOST_CHECK_EQUAL(unsigned(softfloat_exceptionFlags), unsigned(softfloat_flag_infinite));
}

BOOST_AUTO_TEST_SUITE_END()
