#include <boost/test/unit_test.hpp>

#include <fc/io/json.hpp>
#include <fc/log/dmlog_formatter.hpp>
#include <fc/log/json_formatter.hpp>
#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/variant_object.hpp>

#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(const std::string& prefix) {
   static std::atomic<int> counter{0};
   auto uniq = std::hash<std::thread::id>{}(std::this_thread::get_id());
   fs::path p = fs::temp_directory_path()
      / ("fc_json_fmt_" + prefix + "_" + std::to_string(uniq) + "_" + std::to_string(counter++));
   fs::create_directories(p);
   return p;
}

std::vector<std::string> read_lines(const fs::path& file) {
   std::vector<std::string> lines;
   std::ifstream in(file);
   std::string line;
   while (std::getline(in, line)) {
      lines.push_back(line);
   }
   return lines;
}

fc::variant parse_json(const std::string& s) {
   return fc::json::from_string(s);
}

// Build a standard rotating file sink with fc::log::json_formatter attached.
std::shared_ptr<spdlog::sinks::sink> make_json_rotating_sink(
   const std::string& path,
   std::size_t max_size,
   std::size_t max_files,
   std::map<std::string, std::string> extras = {})
{
   auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, max_size, max_files);
   sink->set_formatter(std::make_unique<fc::log::json_formatter>(std::move(extras)));
   return sink;
}

// Create a temp dir, spin up a rotating file sink + json_formatter, run
// log_fn against it, tear everything down, and return the JSONL lines.
std::vector<std::string> sink_write_read(
   const std::string& prefix,
   std::map<std::string, std::string> extras,
   const std::function<void(spdlog::logger&)>& log_fn)
{
   auto dir     = make_temp_dir(prefix);
   auto cleanup = fc::make_scoped_exit([&]{ fs::remove_all(dir); });
   auto file    = dir / "out.jsonl";
   {
      auto sink = make_json_rotating_sink(file.string(), 100*1024*1024, 3, std::move(extras));
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      log_fn(lgr);
   }
   return read_lines(file);
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(json_formatter_tests)

BOOST_AUTO_TEST_CASE(writes_one_json_object_per_line) try {
   auto lines = sink_write_read("basic", {}, [](spdlog::logger& lgr) {
      for (int i = 0; i < 5; ++i) {
         SPDLOG_LOGGER_INFO(&lgr, "message {}", i);
      }
   });
   BOOST_REQUIRE_EQUAL(lines.size(), 5u);
   for (size_t i = 0; i < lines.size(); ++i) {
      auto v = parse_json(lines[i]);
      const auto& obj = v.get_object();
      BOOST_CHECK(obj.contains("ts"));
      BOOST_CHECK(obj.contains("lvl"));
      BOOST_CHECK(obj.contains("thread"));
      BOOST_CHECK(obj.contains("logger"));
      BOOST_CHECK(obj.contains("file"));
      BOOST_CHECK(obj.contains("line"));
      BOOST_CHECK(obj.contains("func"));
      BOOST_CHECK(obj.contains("msg"));
      BOOST_CHECK_EQUAL(obj["msg"].as_string(), std::string("message ") + std::to_string(i));
      BOOST_CHECK_EQUAL(obj["logger"].as_string(), "test_logger");
      BOOST_CHECK_EQUAL(obj["lvl"].as_string(), "info");
   }
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(timestamp_iso8601_microseconds) try {
   auto lines = sink_write_read("ts", {}, [](spdlog::logger& lgr) {
      SPDLOG_LOGGER_INFO(&lgr, "ts check");
   });
   BOOST_REQUIRE_EQUAL(lines.size(), 1u);
   auto v = parse_json(lines[0]);
   std::string ts = v.get_object()["ts"].as_string();
   std::regex iso_re{R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z$)"};
   BOOST_CHECK_MESSAGE(std::regex_match(ts, iso_re),
      "timestamp did not match ISO-8601 microsecond pattern: " + ts);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(levels_match_spdlog) try {
   auto lines = sink_write_read("levels", {}, [](spdlog::logger& lgr) {
      SPDLOG_LOGGER_TRACE(&lgr, "t");
      SPDLOG_LOGGER_DEBUG(&lgr, "d");
      SPDLOG_LOGGER_INFO (&lgr, "i");
      SPDLOG_LOGGER_WARN (&lgr, "w");
      SPDLOG_LOGGER_ERROR(&lgr, "e");
   });
   BOOST_REQUIRE_EQUAL(lines.size(), 5u);
   BOOST_CHECK_EQUAL(parse_json(lines[0]).get_object()["lvl"].as_string(), "trace");
   BOOST_CHECK_EQUAL(parse_json(lines[1]).get_object()["lvl"].as_string(), "debug");
   BOOST_CHECK_EQUAL(parse_json(lines[2]).get_object()["lvl"].as_string(), "info");
   BOOST_CHECK_EQUAL(parse_json(lines[3]).get_object()["lvl"].as_string(), "warn");
   BOOST_CHECK_EQUAL(parse_json(lines[4]).get_object()["lvl"].as_string(), "error");
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(escapes_special_and_control_chars) try {
   const std::string payload = "a\"b\\c\nd\te\rf\x01g\x1fh";
   auto lines = sink_write_read("escape", {}, [&](spdlog::logger& lgr) {
      SPDLOG_LOGGER_INFO(&lgr, "{}", payload);
   });
   BOOST_REQUIRE_EQUAL(lines.size(), 1u);
   BOOST_CHECK(lines[0].find(R"(\")")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\\)")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\n)")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\t)")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\r)")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\u0001)") != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\u001f)") != std::string::npos);
   auto v = parse_json(lines[0]);
   BOOST_CHECK_EQUAL(v.get_object()["msg"].as_string(), payload);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(utf8_preserved_raw) try {
   const std::string payload = "na\xc3\xafve \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
   auto lines = sink_write_read("utf8", {}, [&](spdlog::logger& lgr) {
      SPDLOG_LOGGER_INFO(&lgr, "{}", payload);
   });
   BOOST_REQUIRE_EQUAL(lines.size(), 1u);
   BOOST_CHECK(lines[0].find("\\u") == std::string::npos);
   auto v = parse_json(lines[0]);
   BOOST_CHECK_EQUAL(v.get_object()["msg"].as_string(), payload);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(source_loc_captured) try {
   auto dir  = make_temp_dir("src");
   auto file = dir / "out.jsonl";
   int expected_line = 0;
   {
      auto sink = make_json_rotating_sink(file.string(), 100*1024*1024, 3);
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      expected_line = __LINE__ + 1;
      SPDLOG_LOGGER_INFO(&lgr, "src_loc_check");
   }
   auto lines = read_lines(file);
   BOOST_REQUIRE_EQUAL(lines.size(), 1u);
   auto v = parse_json(lines[0]);
   const auto& obj = v.get_object();
   BOOST_CHECK(obj["file"].as_string().find("test_json_formatter.cpp") != std::string::npos);
   BOOST_CHECK_EQUAL(obj["line"].as_int64(), expected_line);
   BOOST_CHECK(!obj["func"].as_string().empty());
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(extra_fields_emitted_and_omitted) try {
   auto dir  = make_temp_dir("extra");
   auto file_on  = dir / "on.jsonl";
   auto file_off = dir / "off.jsonl";
   {
      auto sink_on  = make_json_rotating_sink(file_on.string(),  100*1024*1024, 3,
         {{"env", "prod"}, {"region", "us-east-1"}});
      auto sink_off = make_json_rotating_sink(file_off.string(), 100*1024*1024, 3);
      spdlog::logger lgr_on("extras_on",   {sink_on});
      spdlog::logger lgr_off("extras_off", {sink_off});
      lgr_on.set_level(spdlog::level::trace);
      lgr_off.set_level(spdlog::level::trace);
      SPDLOG_LOGGER_INFO(&lgr_on,  "m");
      SPDLOG_LOGGER_INFO(&lgr_off, "m");
   }
   auto on_lines  = read_lines(file_on);
   auto off_lines = read_lines(file_off);
   BOOST_REQUIRE_EQUAL(on_lines.size(),  1u);
   BOOST_REQUIRE_EQUAL(off_lines.size(), 1u);
   auto on_obj  = parse_json(on_lines[0]).get_object();
   auto off_obj = parse_json(off_lines[0]).get_object();
   BOOST_CHECK(on_obj.contains("extra"));
   BOOST_CHECK(!off_obj.contains("extra"));
   const auto& extra = on_obj["extra"].get_object();
   BOOST_CHECK_EQUAL(extra["env"].as_string(),    "prod");
   BOOST_CHECK_EQUAL(extra["region"].as_string(), "us-east-1");
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(rotating_size_triggers_rollover) try {
   auto dir  = make_temp_dir("rotate");
   auto file = dir / "out.jsonl";
   {
      // 1 byte max -> every write rotates. (spdlog rejects max_size == 0.)
      auto sink = make_json_rotating_sink(file.string(), 1, 3);
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      for (int i = 0; i < 5; ++i) {
         SPDLOG_LOGGER_INFO(&lgr, "m{}", i);
      }
   }
   BOOST_CHECK(fs::exists(dir / "out.jsonl"));
   BOOST_CHECK(fs::exists(dir / "out.1.jsonl"));
   BOOST_CHECK(fs::exists(dir / "out.2.jsonl"));
   BOOST_CHECK(fs::exists(dir / "out.3.jsonl"));
   BOOST_CHECK(!fs::exists(dir / "out.4.jsonl"));
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(parse_config_from_json_text) try {
   // Full sink_config round-trip through fc::json. extra_fields must accept
   // the natural JSON-object form so operators can hand-edit logging.json.
   auto cfg_json = R"(
   {
      "name": "rot_json",
      "type": "rotating_file_sink",
      "args": { "base_filename": "/tmp/x.jsonl", "max_size": 10, "max_files": 3 },
      "format": {
         "type": "json",
         "args": { "extra_fields": { "env": "prod", "region": "us-east-1" } }
      },
      "enabled": true
   })";
   auto v = fc::json::from_string(cfg_json);
   auto cfg = v.as<fc::sink_config>();
   BOOST_CHECK_EQUAL(cfg.name, "rot_json");
   BOOST_CHECK_EQUAL(cfg.type, "rotating_file_sink");
   BOOST_REQUIRE(cfg.format.has_value());
   BOOST_CHECK_EQUAL(cfg.format->type, "json");
   auto jc = cfg.format->args.as<fc::format::json_config>();
   BOOST_CHECK_EQUAL(jc.extra_fields["env"].as_string(),    "prod");
   BOOST_CHECK_EQUAL(jc.extra_fields["region"].as_string(), "us-east-1");

   // Sink with no format field parses to format = nullopt.
   auto no_format_json = R"(
   {
      "name": "plain",
      "type": "rotating_file_sink",
      "args": { "base_filename": "/tmp/y.log", "max_size": 10, "max_files": 3 },
      "enabled": true
   })";
   auto nfv = fc::json::from_string(no_format_json);
   auto ncfg = nfv.as<fc::sink_config>();
   BOOST_CHECK(!ncfg.format.has_value());

   // Empty format object parses to the in-struct default type = "pattern".
   auto empty_format_json = R"(
   {
      "name": "plain",
      "type": "rotating_file_sink",
      "args": { "base_filename": "/tmp/z.log", "max_size": 10, "max_files": 3 },
      "format": {},
      "enabled": true
   })";
   auto efv = fc::json::from_string(empty_format_json);
   auto ecfg = efv.as<fc::sink_config>();
   BOOST_REQUIRE(ecfg.format.has_value());
   BOOST_CHECK_EQUAL(ecfg.format->type, "pattern");
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(configure_logging_without_format_uses_default_pattern) try {
   auto dir = make_temp_dir("noformat");
   auto restore = fc::make_scoped_exit([]() {
      fc::configure_logging(fc::logging_config::default_config());
   });
   fc::logging_config cfg;

   fc::sink_config s;
   s.name = "plain_rot";
   s.type = "rotating_file_sink";
   fc::sink::rotating_file_sink_config rot_cfg;
   rot_cfg.base_filename = (dir / "plain.log").string();
   rot_cfg.max_size  = 10;
   rot_cfg.max_files = 3;
   s.args = fc::variant{rot_cfg};
   // Intentionally no s.format -- exercises the default-pattern attachment
   // path in configure_logging.
   BOOST_CHECK(!s.format.has_value());
   cfg.sinks.push_back(s);

   fc::logger_config lcfg;
   lcfg.name    = "test_noformat_logger";
   lcfg.level   = fc::log_level::info;
   lcfg.enabled = true;
   lcfg.sinks   = {"plain_rot"};
   cfg.loggers.push_back(lcfg);

   BOOST_CHECK(fc::configure_logging(cfg));
   auto lgr = fc::log_config::get_logger("test_noformat_logger");
   fc_ilog(lgr, "plain pattern message");

   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(configure_logging_with_json_format) try {
   auto dir = make_temp_dir("cfg");
   auto restore = fc::make_scoped_exit([]() {
      fc::configure_logging(fc::logging_config::default_config());
   });
   fc::logging_config cfg;

   // rotating_file_sink + json format
   fc::sink_config s1;
   s1.name = "rot_json";
   s1.type = "rotating_file_sink";
   fc::sink::rotating_file_sink_config rot_cfg;
   rot_cfg.base_filename = (dir / "rot.jsonl").string();
   rot_cfg.max_size  = 10;
   rot_cfg.max_files = 3;
   s1.args = fc::variant{rot_cfg};
   fc::format_config fmt_cfg;
   fmt_cfg.type = "json";
   s1.format = fmt_cfg;
   cfg.sinks.push_back(s1);

   // daily_file_sink + json format
   fc::sink_config s2;
   s2.name = "daily_json";
   s2.type = "daily_file_sink";
   fc::sink::daily_file_sink_config day_cfg;
   day_cfg.base_filename = (dir / "daily.jsonl").string();
   s2.args = fc::variant{day_cfg};
   s2.format = fmt_cfg;
   cfg.sinks.push_back(s2);

   fc::logger_config lcfg;
   lcfg.name    = "test_cfg_logger";
   lcfg.level   = fc::log_level::info;
   lcfg.enabled = true;
   lcfg.sinks   = {"rot_json", "daily_json"};
   cfg.loggers.push_back(lcfg);

   BOOST_CHECK(fc::configure_logging(cfg));
   auto lgr = fc::log_config::get_logger("test_cfg_logger");
   fc_ilog(lgr, "configured message");

   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

// SIGHUP-safety contract: a typo in format.type must not wipe logging
// config. build_formatter warns to stderr and falls back to pattern
// formatter; configure_logging returns true.
BOOST_AUTO_TEST_CASE(unknown_format_type_falls_back_to_pattern) try {
   auto dir = make_temp_dir("unknownfmt");
   auto restore = fc::make_scoped_exit([]() {
      fc::configure_logging(fc::logging_config::default_config());
   });
   // Capture stderr so the expected warning doesn't pollute test output;
   // we also assert it was emitted with the sink name.
   std::stringstream captured;
   std::streambuf* orig_cerr = std::cerr.rdbuf(captured.rdbuf());
   auto restore_cerr = fc::make_scoped_exit([&]{ std::cerr.rdbuf(orig_cerr); });

   fc::logging_config cfg;

   fc::sink_config s;
   s.name = "typo_rot";
   s.type = "rotating_file_sink";
   fc::sink::rotating_file_sink_config rot_cfg;
   rot_cfg.base_filename = (dir / "typo.log").string();
   rot_cfg.max_size  = 10;
   rot_cfg.max_files = 3;
   s.args = fc::variant{rot_cfg};
   fc::format_config fmt_cfg;
   fmt_cfg.type = "josn"; // intentional typo -- exercises the warn-and-fallback path
   s.format = fmt_cfg;
   cfg.sinks.push_back(s);

   fc::logger_config lcfg;
   lcfg.name    = "test_typo_logger";
   lcfg.level   = fc::log_level::info;
   lcfg.enabled = true;
   lcfg.sinks   = {"typo_rot"};
   cfg.loggers.push_back(lcfg);

   BOOST_CHECK(fc::configure_logging(cfg));
   auto lgr = fc::log_config::get_logger("test_typo_logger");
   fc_ilog(lgr, "fallback pattern message");

   auto warn_text = captured.str();
   BOOST_CHECK(warn_text.find("Unknown format type") != std::string::npos);
   BOOST_CHECK(warn_text.find("josn")                != std::string::npos);
   BOOST_CHECK(warn_text.find("typo_rot")            != std::string::npos);

   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

// Sinks defined in config but not referenced by any logger must not be constructed -- no file handle,
// no file on disk. This lets operators maintain a "menu" of sink definitions in logging.json and swap
// in/out by editing logger sinks[] arrays without churning unused files.
BOOST_AUTO_TEST_CASE(unreferenced_sinks_are_not_built) try {
   auto dir = make_temp_dir("unused");
   auto restore = fc::make_scoped_exit([]() {
      fc::configure_logging(fc::logging_config::default_config());
   });
   fc::logging_config cfg;

   // used sink -- attached to the logger below
   fc::sink_config used;
   used.name = "used_rot";
   used.type = "rotating_file_sink";
   fc::sink::rotating_file_sink_config used_rot;
   used_rot.base_filename = (dir / "used.log").string();
   used_rot.max_size  = 10;
   used_rot.max_files = 3;
   used.args = fc::variant{used_rot};
   cfg.sinks.push_back(used);

   // unused sink -- defined but never attached to any logger
   fc::sink_config unused;
   unused.name = "unused_rot";
   unused.type = "rotating_file_sink";
   fc::sink::rotating_file_sink_config unused_rot;
   unused_rot.base_filename = (dir / "unused.log").string();
   unused_rot.max_size  = 10;
   unused_rot.max_files = 3;
   unused.args = fc::variant{unused_rot};
   cfg.sinks.push_back(unused);

   fc::logger_config lcfg;
   lcfg.name    = "test_unused_logger";
   lcfg.level   = fc::log_level::info;
   lcfg.enabled = true;
   lcfg.sinks   = {"used_rot"};
   cfg.loggers.push_back(lcfg);

   BOOST_CHECK(fc::configure_logging(cfg));
   auto lgr = fc::log_config::get_logger("test_unused_logger");
   fc_ilog(lgr, "only writes to used_rot");
   lgr.get_agent_logger()->flush();

   BOOST_CHECK(fs::exists(dir / "used.log"));
   BOOST_CHECK(!fs::exists(dir / "unused.log"));

   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(pattern_format_custom_pattern_string) try {
   auto dir = make_temp_dir("custompat");
   auto restore = fc::make_scoped_exit([]() {
      fc::configure_logging(fc::logging_config::default_config());
   });
   fc::logging_config cfg;

   fc::sink_config s;
   s.name = "custom_rot";
   s.type = "rotating_file_sink";
   fc::sink::rotating_file_sink_config rot_cfg;
   rot_cfg.base_filename = (dir / "custom.log").string();
   rot_cfg.max_size  = 10;
   rot_cfg.max_files = 3;
   s.args = fc::variant{rot_cfg};
   fc::format_config fmt_cfg;
   fmt_cfg.type = "pattern";
   fc::format::pattern_config pc;
   pc.pattern = "PAYLOAD[%v]"; // payload only, in a distinctive envelope
   fmt_cfg.args = fc::variant{pc};
   s.format = fmt_cfg;
   cfg.sinks.push_back(s);

   fc::logger_config lcfg;
   lcfg.name    = "test_custom_logger";
   lcfg.level   = fc::log_level::info;
   lcfg.enabled = true;
   lcfg.sinks   = {"custom_rot"};
   cfg.loggers.push_back(lcfg);

   BOOST_CHECK(fc::configure_logging(cfg));
   auto lgr = fc::log_config::get_logger("test_custom_logger");
   fc_ilog(lgr, "hello");
   lgr.get_agent_logger()->flush();

   auto lines = read_lines(dir / "custom.log");
   BOOST_REQUIRE_EQUAL(lines.size(), 1u);
   BOOST_CHECK_EQUAL(lines[0], "PAYLOAD[hello]");

   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(multi_thread_no_corruption) try {
   auto dir  = make_temp_dir("mt");
   auto file = dir / "out.jsonl";
   constexpr int kThreads = 8;
   constexpr int kPerThread = 200;
   {
      auto sink = make_json_rotating_sink(file.string(), 100*1024*1024, 3);
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      std::vector<std::thread> ths;
      for (int t = 0; t < kThreads; ++t) {
         ths.emplace_back([&lgr, t]() {
            for (int i = 0; i < kPerThread; ++i) {
               SPDLOG_LOGGER_INFO(&lgr, "t{}-i{}", t, i);
            }
         });
      }
      for (auto& th : ths) th.join();
   }
   auto lines = read_lines(file);
   BOOST_REQUIRE_EQUAL(lines.size(), static_cast<size_t>(kThreads * kPerThread));
   for (const auto& l : lines) {
      BOOST_CHECK_NO_THROW(parse_json(l));
   }
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

// Direct unit test of fc::log::json_formatter against a synthetic log_msg, with
// no sink in the loop -- pinpoints formatter bugs independent of spdlog's
// sink plumbing.
BOOST_AUTO_TEST_CASE(formatter_direct_format) try {
   fc::log::json_formatter fmt({{"env", "test"}});
   spdlog::details::log_msg msg{
      spdlog::source_loc{"src.cpp", 42, "my_func"},
      "direct_logger",
      spdlog::level::warn,
      "hello \"world\""
   };
   spdlog::memory_buf_t out;
   fmt.format(msg, out);
   std::string line{out.data(), out.size()};
   BOOST_REQUIRE(!line.empty());
   BOOST_CHECK_EQUAL(line.back(), '\n');
   auto v = parse_json(line);
   const auto& obj = v.get_object();
   BOOST_CHECK_EQUAL(obj["lvl"].as_string(),    "warn");
   BOOST_CHECK_EQUAL(obj["logger"].as_string(), "direct_logger");
   BOOST_CHECK_EQUAL(obj["file"].as_string(),   "src.cpp");
   BOOST_CHECK_EQUAL(obj["line"].as_int64(),    42);
   BOOST_CHECK_EQUAL(obj["func"].as_string(),   "my_func");
   BOOST_CHECK_EQUAL(obj["msg"].as_string(),    "hello \"world\"");
   BOOST_CHECK_EQUAL(obj["extra"].get_object()["env"].as_string(), "test");
} FC_LOG_AND_RETHROW()

// Direct unit test of fc::log::dmlog_formatter against a synthetic log_msg.
// Exact byte sequence "DMLOG " + payload + "\n" must match what dfuse
// postprocessing tools consume.
BOOST_AUTO_TEST_CASE(dmlog_formatter_direct_format) try {
   fc::log::dmlog_formatter fmt;
   spdlog::details::log_msg msg{
      spdlog::source_loc{"src.cpp", 1, "f"},
      "dmlogger",
      spdlog::level::info,
      "ABI_DUMP foo bar"
   };
   spdlog::memory_buf_t out;
   fmt.format(msg, out);
   std::string line{out.data(), out.size()};
   BOOST_CHECK_EQUAL(line, "DMLOG ABI_DUMP foo bar\n");

   // Empty payload still produces "DMLOG \n".
   spdlog::memory_buf_t out2;
   spdlog::details::log_msg empty_msg{
      spdlog::source_loc{"src.cpp", 1, "f"},
      "dmlogger",
      spdlog::level::info,
      ""
   };
   fmt.format(empty_msg, out2);
   std::string line2{out2.data(), out2.size()};
   BOOST_CHECK_EQUAL(line2, "DMLOG \n");
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
