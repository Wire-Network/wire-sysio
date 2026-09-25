#include <boost/test/unit_test.hpp>

#include <fc/log/logger.hpp>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

BOOST_AUTO_TEST_SUITE(logger_tests)

// A logger that logging.json does not configure keeps the sink it was built with, and every thread logging through it
// shares that sink, so the sink must lock.
BOOST_AUTO_TEST_CASE(unconfigured_logger_sink_is_thread_safe) {
   fc::logger log;
   const auto& sinks = log.get_agent_logger()->sinks();
   BOOST_REQUIRE_EQUAL(sinks.size(), 1u);
   BOOST_CHECK(std::dynamic_pointer_cast<spdlog::sinks::stderr_color_sink_mt>(sinks.front()));
}

BOOST_AUTO_TEST_SUITE_END()
