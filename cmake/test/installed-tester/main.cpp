#define BOOST_TEST_MODULE installed_tester_probe
#include <boost/test/included/unit_test.hpp>
#include <fc/io/json.hpp>
#include <sysio/testing/tester.hpp>
#include <version>

static_assert(_LIBCPP_VERSION == 230103, "Installed consumers must use libc++ 23.1.3");

/// Exercise C++ objects across installed library boundaries and start a chain.
BOOST_AUTO_TEST_CASE(installed_host_abi) {
   const std::string message = "installed-libcxx-consumer";
   const auto json = fc::json::to_string(fc::variant(message), fc::time_point::maximum());
   BOOST_CHECK_EQUAL(fc::json::from_string(json).as_string(), message);
   sysio::testing::tester chain;
   chain.produce_block();
   BOOST_CHECK_GT(chain.control->head().block_num(), 0u);
}
