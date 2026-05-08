#include <format>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/dll.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/spawn.hpp>
#include <boost/test/unit_test.hpp>

#include <sysio/opp/opp.hpp>

using namespace sysio::opp;

namespace {

namespace bp = boost::process;
namespace bfs = boost::filesystem;

}

BOOST_AUTO_TEST_SUITE(opp_specs)

BOOST_AUTO_TEST_CASE(opp_test1) {
   BOOST_CHECK_EQUAL(1,1);
}

BOOST_AUTO_TEST_SUITE_END()
