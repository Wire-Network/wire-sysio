#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <set>

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>

using namespace std::literals;

BOOST_AUTO_TEST_SUITE(batch_operator_plugin)

  BOOST_AUTO_TEST_CASE(batch_operator_plugin_dummy) try {
    BOOST_CHECK(1 > 0);
  } FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
