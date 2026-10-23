#define BOOST_TEST_MODULE test_resmom_plugin
#include <boost/test/included/unit_test.hpp>

#include <fc/variant_object.hpp>
#include <fc/filesystem.hpp>
#include <sysio/chain/exceptions.hpp>

#include <sys/stat.h>
#include <filesystem>
#include <sstream>
#include <spdlog/sinks/ostream_sink.h>
#include <sysio/resource_monitor_plugin/resource_monitor_plugin.hpp>
#include <sysio/resource_monitor_plugin/file_space_handler.hpp>

#include <thread>

using namespace sysio;
using namespace boost::system;


// For program options
namespace bpo = boost::program_options;
using bpo::options_description;
using bpo::variables_map;

struct resmon_fixture {
   void set_program_options() {
      options_description dummy;
      _my.set_program_options(dummy, _cfg);
   }

   void initialize(const std::vector<std::string>& args){
      // We only have at most 3 arguments. OK to hardcodied in test
      // programs.
      const char* argv[10];
      SYS_ASSERT(args.size() < 10, chain::plugin_exception, "number of arguments ({}) must be less than 10", args.size());

      // argv[0] is program name, no need to fill in
      for (size_t i=0; i<args.size(); ++i) {
         argv[i+1] = args[i].c_str();
      }

      bpo::variables_map options;
      bpo::store(bpo::parse_command_line(args.size()+1, argv, _cfg), options);
      bpo::notify(options);

      _my.plugin_initialize(options);
   }

   void set_options(const std::vector<std::string>& arg) {
      set_program_options();
      initialize(arg);
   }

   void plugin_startup(const std::vector<std::filesystem::path>& dirs, int runTimeSecs=3) {
      set_options({"--resource-monitor-interval-seconds=1"});

      for (auto& dir: dirs) {
         _my.monitor_directory(dir);
      }

      _my.plugin_startup();
      std::this_thread::sleep_for( std::chrono::milliseconds(runTimeSecs*1000) );
      _my.plugin_shutdown();
   }

   resource_monitor_plugin _my;
   options_description _cfg;
};

namespace {
struct fixed_space_provider {
   uintmax_t available;
   int get_stat(const char*, struct stat* result) {
      result->st_dev = 1;
      return 0;
   }
   std::filesystem::space_info get_space(const std::filesystem::path&, std::error_code& error) {
      error.clear();
      return {100, available, available};
   }
};

struct captured_monitor_logger {
   std::ostringstream output;
   fc::logger logger = fc::logger::default_logger();
   fc::log_level saved_level = logger.get_log_level();
   bool saved_enabled = logger.is_enabled();
   std::unique_ptr<spdlog::logger> saved_logger = std::move(logger.get_agent_logger());

   captured_monitor_logger() {
      auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
      sink->set_pattern("%v");
      logger.update_agent_logger(std::make_unique<spdlog::logger>("monitor-cadence", sink));
      logger.set_log_level(fc::log_level::warn);
      logger.set_enabled(true);
   }
   ~captured_monitor_logger() {
      logger.update_agent_logger(std::move(saved_logger));
      logger.set_log_level(saved_level);
      logger.set_enabled(saved_enabled);
   }
   size_t warning_count() const {
      const auto text = output.str();
      const std::string warning = "Space usage warning";
      size_t count = 0;
      for (size_t pos = 0; (pos = text.find(warning, pos)) != std::string::npos; pos += warning.size()) ++count;
      return count;
   }
};
}

BOOST_AUTO_TEST_SUITE(resmon_plugin_tests)
   BOOST_FIXTURE_TEST_CASE(intervalTooBig, resmon_fixture)
   {
      BOOST_REQUIRE_THROW(set_options({"--resource-monitor-interval-seconds=301"}), chain::plugin_config_exception);
   }

   BOOST_FIXTURE_TEST_CASE(intervalTooSmall, resmon_fixture)
   {
      BOOST_REQUIRE_THROW(set_options({"--resource-monitor-interval-seconds=0"}), chain::plugin_config_exception);
   }

   BOOST_FIXTURE_TEST_CASE(intervalLowBound, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-interval-seconds=1"}));
   }

   BOOST_FIXTURE_TEST_CASE(intervalMiddle, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-interval-seconds=150"}));
   }

   BOOST_FIXTURE_TEST_CASE(intervalHighBound, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-interval-seconds=300"}));
   }

   BOOST_FIXTURE_TEST_CASE(thresholdTooBig, resmon_fixture)
   {
      BOOST_REQUIRE_THROW(set_options({"--resource-monitor-space-threshold=100"}), chain::plugin_config_exception);
   }

   BOOST_FIXTURE_TEST_CASE(thresholdTooSmall, resmon_fixture)
   {
      BOOST_REQUIRE_THROW(set_options({"--resource-monitor-space-threshold=5"}), chain::plugin_config_exception);
   }

   BOOST_FIXTURE_TEST_CASE(thresholdLowBound, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-space-threshold=6"}));
   }

   BOOST_FIXTURE_TEST_CASE(thresholdMiddle, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-space-threshold=60"}));
   }

   BOOST_FIXTURE_TEST_CASE(thresholdHighBound, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-space-threshold=99"}));
   }

   BOOST_FIXTURE_TEST_CASE(absoluteTooBig, resmon_fixture)
   {
      BOOST_REQUIRE_THROW(set_options({"--resource-monitor-space-absolute-gb=17179869183"}), chain::plugin_config_exception);
   }

   BOOST_FIXTURE_TEST_CASE(absoluteTooSmall, resmon_fixture)
   {
      BOOST_REQUIRE_THROW(set_options({"--resource-monitor-space-absolute-gb=0"}), chain::plugin_config_exception);
   }

   BOOST_FIXTURE_TEST_CASE(absoluteLowBound, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-space-absolute-gb=1"}));
   }

   BOOST_FIXTURE_TEST_CASE(absoluteMiddle, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-space-absolute-gb=1024"}));
   }

   BOOST_FIXTURE_TEST_CASE(absoluteHighBound, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-space-absolute-gb=17179869182"}));
   }

   BOOST_FIXTURE_TEST_CASE(noShutdown, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-not-shutdown-on-threshold-exceeded"}));
   }

   BOOST_FIXTURE_TEST_CASE(startupNormal, resmon_fixture)
   {
      // do not use native "/tmp", as subdirectories in /tmp on test machine
      // can be removed during a test run, causing file_space_handler::add_file_system
      // to assert when doing get_stat on a removed directory
      fc::temp_directory temp_dir;
      BOOST_REQUIRE_NO_THROW(plugin_startup({temp_dir.path()}));
   }

   BOOST_FIXTURE_TEST_CASE(startupDuplicateDirs, resmon_fixture)
   {
      fc::temp_directory temp_dir;
      BOOST_REQUIRE_NO_THROW(plugin_startup({temp_dir.path(), temp_dir.path()}));
   }

   BOOST_FIXTURE_TEST_CASE(startupMultDirs, resmon_fixture)
   {
      fc::temp_directory temp_dir_1;
      fc::temp_directory temp_dir_2;
      BOOST_REQUIRE_NO_THROW(plugin_startup({temp_dir_1.path(), temp_dir_2.path()}));
   }

   BOOST_FIXTURE_TEST_CASE(startupNoExistingDirs, resmon_fixture)
   {
      fc::temp_directory temp_dir;
      // temp_dir/hsdfgd983 does not exist in a just created temp directory
      BOOST_REQUIRE_THROW(plugin_startup({temp_dir.path(), temp_dir.path() / "hsdfgd983"}), chain::plugin_config_exception);
   }

   BOOST_FIXTURE_TEST_CASE(warningIntervalTooBig, resmon_fixture)
   {
      BOOST_REQUIRE_THROW(set_options({"--resource-monitor-warning-interval=451"}), chain::plugin_config_exception);
   }

   BOOST_FIXTURE_TEST_CASE(warningIntervalTooSmall, resmon_fixture)
   {
      BOOST_REQUIRE_THROW(set_options({"--resource-monitor-warning-interval=0"}), chain::plugin_config_exception);
   }

   BOOST_FIXTURE_TEST_CASE(warningIntervalLowBound, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-warning-interval=1"}));
   }

   BOOST_FIXTURE_TEST_CASE(warningIntervalMiddle, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-warning-interval=225"}));
   }

   BOOST_FIXTURE_TEST_CASE(warningIntervalHighBound, resmon_fixture)
   {
      BOOST_REQUIRE_NO_THROW(set_options({"--resource-monitor-warning-interval=450"}));
   }

   BOOST_AUTO_TEST_CASE(warnings_follow_monitor_ticks)
   {
      // Drives space_monitor_loop() directly (no timer), so tick counts are exact for both warning kinds.
      for (uintmax_t available : {5u, 12u}) {
         for (uint32_t interval : {1u, 5u, 30u}) {
            BOOST_TEST_CONTEXT("available=" << available << ", warning interval=" << interval) {
               captured_monitor_logger capture;
               resource_monitor::file_space_handler<fixed_space_provider> monitor(fixed_space_provider{available});
               monitor.set_threshold(90, 85);
               monitor.set_shutdown_on_exceeded(false);
               monitor.set_warning_interval(interval);
               monitor.add_file_system("mock-filesystem");
               BOOST_REQUIRE_EQUAL(capture.warning_count(), 0u);
               for (uint32_t tick = 1; tick <= 3 * interval; ++tick) {
                  monitor.space_monitor_loop();
                  BOOST_CHECK_EQUAL(capture.warning_count(), 1u + (tick - 1) / interval);
               }
            }
         }
      }
   }

BOOST_AUTO_TEST_SUITE_END()
