#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <set>

#include <sysio/outpost_client_plugin.hpp>

using namespace std::literals;
using namespace sysio;

namespace {

}

BOOST_AUTO_TEST_SUITE(outpost_client_plugin)

  BOOST_AUTO_TEST_CASE(init_plugin) try {
   BOOST_CHECK(1 > 0);
  } FC_LOG_AND_RETHROW();


BOOST_AUTO_TEST_SUITE_END()
