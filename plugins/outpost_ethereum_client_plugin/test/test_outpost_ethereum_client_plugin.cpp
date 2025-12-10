#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <set>

#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/outpost_client/ethereum/ethereum_client.hpp>

using namespace std::literals;
using namespace sysio::outpost_client;
using namespace sysio::outpost_client::ethereum;

namespace {

}

BOOST_AUTO_TEST_SUITE(outpost_ethereum_client_plugin)

  BOOST_AUTO_TEST_CASE(init_plugin) try {
    std::println(std::cerr, "This works");
    BOOST_CHECK(1 > 0);
  } FC_LOG_AND_RETHROW();


BOOST_AUTO_TEST_SUITE_END()
