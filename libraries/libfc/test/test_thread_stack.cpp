#include <fc/scoped_exit.hpp>
#include <fc/thread_stack.hpp>

#include <boost/test/unit_test.hpp>

#include <pthread.h>

namespace {

/// Stack the tests require of the main thread.
constexpr rlim_t required = 8 * 1024 * 1024;
/// Stack given to the thread whose size is read back.
constexpr std::size_t sized_thread_stack = 3 * 1024 * 1024;

/// Records the stack size the thread it runs on reports.
void* record_stack_size(void* result) {
   *static_cast<std::size_t*>(result) = fc::current_thread_stack_size();
   return nullptr;
}

} // namespace

BOOST_AUTO_TEST_SUITE(thread_stack_test_suite)

/// A thread created with a stack size reports at least that size.
BOOST_AUTO_TEST_CASE(current_thread_stack_size_reports_created_size) {
   pthread_attr_t attributes;
   BOOST_REQUIRE(pthread_attr_init(&attributes) == 0);
   auto destroy = fc::make_scoped_exit([&attributes]() { pthread_attr_destroy(&attributes); });
   BOOST_REQUIRE(pthread_attr_setstacksize(&attributes, sized_thread_stack) == 0);

   std::size_t reported = 0;
   pthread_t thread;
   BOOST_REQUIRE(pthread_create(&thread, &attributes, record_stack_size, &reported) == 0);
   BOOST_REQUIRE(pthread_join(thread, nullptr) == 0);
   BOOST_TEST(reported >= sized_thread_stack);
}

/// The soft limit is raised only when it is below the requirement, and only as far as the requirement.
BOOST_AUTO_TEST_CASE(soft_stack_limit_raised_to_requirement) {
   constexpr rlim_t smaller = required / 4;
   constexpr rlim_t larger = required * 2;

   // 0 stands for "no change" so the results print
   auto raised = [](rlim_t soft, rlim_t hard) {
      return fc::raised_soft_stack_limit({.rlim_cur = soft, .rlim_max = hard}, required).value_or(0);
   };
   BOOST_TEST(raised(RLIM_INFINITY, RLIM_INFINITY) == 0u);
   BOOST_TEST(raised(required, required) == 0u);
   BOOST_TEST(raised(larger, RLIM_INFINITY) == 0u);
   BOOST_TEST(raised(smaller, RLIM_INFINITY) == required);
   BOOST_TEST(raised(smaller, required) == required);
   BOOST_TEST(raised(smaller, larger) == required);
}

/// A hard limit below the requirement cannot be worked around, so it is reported.
BOOST_AUTO_TEST_CASE(stack_hard_limit_below_requirement_throws) {
   constexpr rlim_t smaller = required / 4;
   BOOST_CHECK_EXCEPTION(fc::raised_soft_stack_limit({.rlim_cur = smaller, .rlim_max = smaller}, required),
                         fc::assert_exception, [](const fc::assert_exception& e) {
                            return e.top_message().find("The stack hard limit (ulimit -Hs)") != std::string::npos;
                         });
}

#if defined(__linux__)
/// raise_main_thread_stack_limit sets the process's soft limit to the requirement.
BOOST_AUTO_TEST_CASE(main_thread_stack_limit_raised) {
   rlimit original{};
   BOOST_REQUIRE(getrlimit(RLIMIT_STACK, &original) == 0);
   if (original.rlim_max != RLIM_INFINITY && original.rlim_max < required) {
      BOOST_TEST_MESSAGE("stack hard limit below the requirement; nothing to raise");
      return;
   }
   auto restore = fc::make_scoped_exit([&original]() { setrlimit(RLIMIT_STACK, &original); });

   rlimit lowered = original;
   lowered.rlim_cur = required / 2;
   BOOST_REQUIRE(setrlimit(RLIMIT_STACK, &lowered) == 0);
   fc::raise_main_thread_stack_limit(required);

   rlimit raised{};
   BOOST_REQUIRE(getrlimit(RLIMIT_STACK, &raised) == 0);
   BOOST_TEST(raised.rlim_cur == required);
   BOOST_TEST(raised.rlim_max == original.rlim_max);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
