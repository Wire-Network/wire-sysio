#include <boost/test/unit_test.hpp>

#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/reflect/variant.hpp>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

BOOST_AUTO_TEST_SUITE(logger_tests)

// A logging config names a console sink's stream "stderr" or "stdout" on every platform. macOS defines both as macros,
// and a reflected name taken from the macro would make nodeop reject the config it ships with.
BOOST_AUTO_TEST_CASE(console_sink_output_type_spelling) {
   using fc::sink::output_t;
   const auto read = [](const std::string& json) {
      return fc::json::from_string(json).as<fc::sink::console_sink_config>().output_type;
   };
   BOOST_CHECK(read(R"({"output_type": "stderr"})") == output_t::output_stderr);
   BOOST_CHECK(read(R"({"output_type": "stdout"})") == output_t::output_stdout);
   BOOST_CHECK_THROW(read(R"({"output_type": "stdin"})"), fc::bad_cast_exception);
   BOOST_CHECK_EQUAL(fc::variant(output_t::output_stderr).as_string(), "stderr");
   BOOST_CHECK_EQUAL(fc::variant(output_t::output_stdout).as_string(), "stdout");
}

// A logger that logging.json does not configure keeps the sink it was built with, and every thread logging through it
// shares that sink, so the sink must lock.
BOOST_AUTO_TEST_CASE(unconfigured_logger_sink_is_thread_safe) {
   fc::logger log;
   const auto& sinks = log.get_agent_logger()->sinks();
   BOOST_REQUIRE_EQUAL(sinks.size(), 1u);
   BOOST_CHECK(std::dynamic_pointer_cast<spdlog::sinks::stderr_color_sink_mt>(sinks.front()));
}

BOOST_AUTO_TEST_SUITE_END()
