#include <sysio/chain/webassembly/sys-vm-oc/chained_signal_handler.hpp>

#include <boost/test/unit_test.hpp>

namespace {

/// Captures arguments from a direct handler invocation without installing process-wide signal actions.
struct handler_observation {
   int signal = 0;
   siginfo_t* info = nullptr;
   void* context = nullptr;
};
handler_observation observed;

/// Model a one-argument POSIX handler.
void legacy_handler(int sig) {
   observed.signal = sig;
}

/// Model a three-argument SA_SIGINFO handler.
void siginfo_handler(int sig, siginfo_t* info, void* context) {
   observed = {sig, info, context};
}

} // namespace

BOOST_AUTO_TEST_SUITE(sysvmoc_signal_handler_tests)

/// A legacy handler receives the signal through its declared one-argument ABI.
BOOST_AUTO_TEST_CASE(legacy_action) {
   struct sigaction action{};
   action.sa_handler = legacy_handler;
   observed = {};
   const sysio::chain::sysvmoc::chained_signal_handler handler(action);
   BOOST_CHECK(handler(SIGSEGV, nullptr, nullptr));
   BOOST_CHECK_EQUAL(observed.signal, SIGSEGV);
   BOOST_CHECK(observed.info == nullptr);
   BOOST_CHECK(observed.context == nullptr);
}

/// A SA_SIGINFO handler retains all three arguments, including their original addresses.
BOOST_AUTO_TEST_CASE(siginfo_action) {
   struct sigaction action{};
   action.sa_flags = SA_SIGINFO;
   action.sa_sigaction = siginfo_handler;
   siginfo_t info{};
   int context = 0;
   observed = {};
   const sysio::chain::sysvmoc::chained_signal_handler handler(action);
   BOOST_CHECK(handler(SIGSEGV, &info, &context));
   BOOST_CHECK_EQUAL(observed.signal, SIGSEGV);
   BOOST_CHECK(observed.info == &info);
   BOOST_CHECK(observed.context == &context);
}

/// Default, ignored, and absent predecessor actions leave the executor's existing fallback in control.
BOOST_AUTO_TEST_CASE(non_callable_actions) {
   using sysio::chain::sysvmoc::chained_signal_handler;
   observed = {};
   BOOST_CHECK(!chained_signal_handler{}(SIGSEGV, nullptr, nullptr));
   struct sigaction action{};
   action.sa_handler = SIG_DFL;
   BOOST_CHECK(!chained_signal_handler{action}(SIGSEGV, nullptr, nullptr));
   action.sa_handler = SIG_IGN;
   BOOST_CHECK(!chained_signal_handler{action}(SIGSEGV, nullptr, nullptr));
   action.sa_flags = SA_SIGINFO;
   action.sa_handler = SIG_DFL;
   BOOST_CHECK(!chained_signal_handler{action}(SIGSEGV, nullptr, nullptr));
   action.sa_handler = SIG_IGN;
   BOOST_CHECK(!chained_signal_handler{action}(SIGSEGV, nullptr, nullptr));
   action = {};
   action.sa_flags = SA_SIGINFO;
   BOOST_CHECK(!chained_signal_handler{action}(SIGSEGV, nullptr, nullptr));
   BOOST_CHECK_EQUAL(observed.signal, 0);
}

BOOST_AUTO_TEST_SUITE_END()
