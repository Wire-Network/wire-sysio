#include <fc/process.hpp>

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <string_view>

namespace {
constexpr std::string_view test_binary = "test_fc";
} // namespace

BOOST_AUTO_TEST_SUITE(process_suite)

// program_path() names the running test binary: an absolute path to an existing regular file.
BOOST_AUTO_TEST_CASE(program_path_is_the_running_executable) {
   const std::filesystem::path path = fc::program_path();
   BOOST_TEST(path.is_absolute());
   BOOST_TEST(std::filesystem::is_regular_file(path));
   BOOST_TEST(path.filename().string() == test_binary);
   BOOST_TEST(fc::program_name() == test_binary);
}

BOOST_AUTO_TEST_SUITE_END()
