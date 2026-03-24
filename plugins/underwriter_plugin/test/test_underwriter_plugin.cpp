#include <boost/test/unit_test.hpp>

#include <set>

#include <sysio/underwriter_plugin/underwriter_plugin.hpp>

using namespace std::literals;

BOOST_AUTO_TEST_SUITE(underwriter_plugin_tests)

BOOST_AUTO_TEST_CASE(plugin_can_be_constructed) try {
   sysio::underwriter_plugin plugin;
   BOOST_CHECK(true);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(plugin_options_are_registered) try {
   sysio::underwriter_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   const auto& opts = cfg.options();
   std::set<std::string> option_names;
   for (const auto& opt : opts) {
      option_names.insert(opt->long_name());
   }
   BOOST_CHECK(option_names.count("underwriter-account") > 0);
   BOOST_CHECK(option_names.count("underwriter-scan-interval-ms") > 0);
   BOOST_CHECK(option_names.count("underwriter-verify-timeout-ms") > 0);
   BOOST_CHECK(option_names.count("underwriter-enabled") > 0);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(default_options_are_correct) try {
   sysio::underwriter_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   boost::program_options::variables_map vm;
   boost::program_options::store(
      boost::program_options::parse_command_line(0, static_cast<char**>(nullptr), cfg), vm);
   boost::program_options::notify(vm);

   BOOST_CHECK_EQUAL(vm["underwriter-scan-interval-ms"].as<uint32_t>(), 5000u);
   BOOST_CHECK_EQUAL(vm["underwriter-verify-timeout-ms"].as<uint32_t>(), 10000u);
   BOOST_CHECK_EQUAL(vm["underwriter-enabled"].as<bool>(), false);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
