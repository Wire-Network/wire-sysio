#include <boost/test/unit_test.hpp>

#include <fc/exception/exception.hpp>

#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

BOOST_AUTO_TEST_SUITE(exception)

/**
 * Verify fc::exception::rethrow() preserves the dynamic exception type.
 * A plain `throw *exception_ptr;` slices to fc::exception; rethrow() must throw the
 * original derived type (assert_exception here) so typed catch sites still match,
 * and appended log context must survive dynamic_copy_exception().
 */
BOOST_AUTO_TEST_CASE(rethrow) try {
   fc::exception_ptr exp;
   try {
      try {
         FC_ASSERT(false, "test {}", 42);
      } FC_RETHROW_EXCEPTIONS(info, "extra stuff")
   } catch (fc::exception& e) {
      exp = e.dynamic_copy_exception();
   }

   BOOST_TEST(exp->to_detail_string().find("test 42") != std::string::npos);
   BOOST_TEST(exp->to_detail_string().find("extra stuff") != std::string::npos);

   try {
      exp->rethrow();
   } catch (fc::assert_exception& e) {
      // success
   } catch (fc::exception& e) {
      BOOST_FAIL("should be: assert_exception");
   } catch (...) {
      BOOST_FAIL("should be: assert_exception");
   }

} FC_LOG_AND_RETHROW();

/// Preserve the existing policy: cleanup drops ordinary failures and the general macro propagates interprocess OOM.
BOOST_AUTO_TEST_CASE(cleanup_exception_policy) {
   const auto cleanup = [](auto failure) noexcept {
      try {
         failure();
      } FC_LOG_AND_DROP_NOEXCEPT("cleanup regression")
   };
   BOOST_CHECK_NO_THROW(cleanup([] { FC_THROW_EXCEPTION(fc::invalid_arg_exception, "expected cleanup failure"); }));
   BOOST_CHECK_NO_THROW(cleanup([] { throw std::runtime_error("expected cleanup failure"); }));
   BOOST_CHECK_NO_THROW(cleanup([] { throw 0; }));
   const auto propagate = [] {
      try {
         throw boost::interprocess::bad_alloc();
      } FC_LOG_AND_DROP("allocation regression")
   };
   BOOST_CHECK_THROW(propagate(), boost::interprocess::bad_alloc);
}

/// Preserve the existing noexcept cleanup policy, which terminates on interprocess OOM.
BOOST_AUTO_TEST_CASE(cleanup_allocation_failure_terminates) {
   constexpr int terminated = 73;
   constexpr int returned = 74;
   const auto child = ::fork();
   BOOST_REQUIRE(child >= 0);
   if (child == 0) {
      std::set_terminate([] { ::_exit(terminated); });
      const auto cleanup = []() noexcept {
         try {
            throw boost::interprocess::bad_alloc();
         } FC_LOG_AND_DROP_NOEXCEPT("allocation regression")
      };
      cleanup();
      ::_exit(returned);
   }
   int status = 0;
   pid_t waited;
   do {
      waited = ::waitpid(child, &status, 0);
   } while (waited == -1 && errno == EINTR);
   BOOST_REQUIRE_EQUAL(waited, child);
   BOOST_REQUIRE(WIFEXITED(status));
   BOOST_CHECK_EQUAL(WEXITSTATUS(status), terminated);
}

BOOST_AUTO_TEST_SUITE_END()
