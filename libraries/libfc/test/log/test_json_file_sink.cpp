#include <boost/test/unit_test.hpp>

#include <fc/io/json.hpp>
#include <fc/log/json_file_sink.hpp>
#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/variant_object.hpp>

#include <spdlog/logger.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(const std::string& prefix) {
   static std::atomic<int> counter{0};
   auto pid = static_cast<long>(::getpid());
   fs::path p = fs::temp_directory_path()
      / ("fc_json_sink_" + prefix + "_" + std::to_string(pid) + "_" + std::to_string(counter++));
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

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(json_file_sink_tests)

BOOST_AUTO_TEST_CASE(writes_one_json_object_per_line) try {
   auto dir  = make_temp_dir("basic");
   auto file = dir / "out.jsonl";
   {
      auto sink = std::make_shared<fc::json_rotating_file_sink_mt>(
         file.string(), 100*1024*1024, 3, std::map<std::string, std::string>{});
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      for (int i = 0; i < 5; ++i) {
         SPDLOG_LOGGER_INFO(&lgr, "message {}", i);
      }
   }
   auto lines = read_lines(file);
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
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(timestamp_iso8601_microseconds) try {
   auto dir  = make_temp_dir("ts");
   auto file = dir / "out.jsonl";
   {
      auto sink = std::make_shared<fc::json_rotating_file_sink_mt>(
         file.string(), 100*1024*1024, 3, std::map<std::string, std::string>{});
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      SPDLOG_LOGGER_INFO(&lgr, "ts check");
   }
   auto lines = read_lines(file);
   BOOST_REQUIRE_EQUAL(lines.size(), 1u);
   auto v = parse_json(lines[0]);
   std::string ts = v.get_object()["ts"].as_string();
   std::regex iso_re{R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z$)"};
   BOOST_CHECK_MESSAGE(std::regex_match(ts, iso_re),
      "timestamp did not match ISO-8601 microsecond pattern: " + ts);
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(levels_match_spdlog) try {
   auto dir  = make_temp_dir("levels");
   auto file = dir / "out.jsonl";
   {
      auto sink = std::make_shared<fc::json_rotating_file_sink_mt>(
         file.string(), 100*1024*1024, 3, std::map<std::string, std::string>{});
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      SPDLOG_LOGGER_TRACE(&lgr, "t");
      SPDLOG_LOGGER_DEBUG(&lgr, "d");
      SPDLOG_LOGGER_INFO (&lgr, "i");
      SPDLOG_LOGGER_WARN (&lgr, "w");
      SPDLOG_LOGGER_ERROR(&lgr, "e");
   }
   auto lines = read_lines(file);
   BOOST_REQUIRE_EQUAL(lines.size(), 5u);
   BOOST_CHECK_EQUAL(parse_json(lines[0]).get_object()["lvl"].as_string(), "trace");
   BOOST_CHECK_EQUAL(parse_json(lines[1]).get_object()["lvl"].as_string(), "debug");
   BOOST_CHECK_EQUAL(parse_json(lines[2]).get_object()["lvl"].as_string(), "info");
   BOOST_CHECK_EQUAL(parse_json(lines[3]).get_object()["lvl"].as_string(), "warn");
   BOOST_CHECK_EQUAL(parse_json(lines[4]).get_object()["lvl"].as_string(), "error");
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(escapes_special_and_control_chars) try {
   auto dir  = make_temp_dir("escape");
   auto file = dir / "out.jsonl";
   const std::string payload = "a\"b\\c\nd\te\rf\x01g\x1fh";
   {
      auto sink = std::make_shared<fc::json_rotating_file_sink_mt>(
         file.string(), 100*1024*1024, 3, std::map<std::string, std::string>{});
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      SPDLOG_LOGGER_INFO(&lgr, "{}", payload);
   }
   auto lines = read_lines(file);
   BOOST_REQUIRE_EQUAL(lines.size(), 1u);
   // Raw line contains JSON-escaped sequences verbatim.
   BOOST_CHECK(lines[0].find(R"(\")")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\\)")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\n)")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\t)")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\r)")     != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\u0001)") != std::string::npos);
   BOOST_CHECK(lines[0].find(R"(\u001f)") != std::string::npos);
   // Full round-trip through fc::json (exercises fixed parse_escape).
   auto v = parse_json(lines[0]);
   BOOST_CHECK_EQUAL(v.get_object()["msg"].as_string(), payload);
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(utf8_preserved_raw) try {
   auto dir  = make_temp_dir("utf8");
   auto file = dir / "out.jsonl";
   const std::string payload = "na\xc3\xafve \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
   {
      auto sink = std::make_shared<fc::json_rotating_file_sink_mt>(
         file.string(), 100*1024*1024, 3, std::map<std::string, std::string>{});
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      SPDLOG_LOGGER_INFO(&lgr, "{}", payload);
   }
   auto lines = read_lines(file);
   BOOST_REQUIRE_EQUAL(lines.size(), 1u);
   BOOST_CHECK(lines[0].find("\\u") == std::string::npos);
   auto v = parse_json(lines[0]);
   BOOST_CHECK_EQUAL(v.get_object()["msg"].as_string(), payload);
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(source_loc_captured) try {
   auto dir  = make_temp_dir("src");
   auto file = dir / "out.jsonl";
   int expected_line = 0;
   {
      auto sink = std::make_shared<fc::json_rotating_file_sink_mt>(
         file.string(), 100*1024*1024, 3, std::map<std::string, std::string>{});
      spdlog::logger lgr("test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      expected_line = __LINE__ + 1;
      SPDLOG_LOGGER_INFO(&lgr, "src_loc_check");
   }
   auto lines = read_lines(file);
   BOOST_REQUIRE_EQUAL(lines.size(), 1u);
   auto v = parse_json(lines[0]);
   const auto& obj = v.get_object();
   BOOST_CHECK(obj["file"].as_string().find("test_json_file_sink.cpp") != std::string::npos);
   BOOST_CHECK_EQUAL(obj["line"].as_int64(), expected_line);
   BOOST_CHECK(!obj["func"].as_string().empty());
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(extra_fields_emitted_and_omitted) try {
   auto dir  = make_temp_dir("extra");
   auto file_on  = dir / "on.jsonl";
   auto file_off = dir / "off.jsonl";
   {
      auto sink_on  = std::make_shared<fc::json_rotating_file_sink_mt>(
         file_on.string(), 100*1024*1024, 3,
         std::map<std::string, std::string>{{"env", "prod"}, {"region", "us-east-1"}});
      auto sink_off = std::make_shared<fc::json_rotating_file_sink_mt>(
         file_off.string(), 100*1024*1024, 3, std::map<std::string, std::string>{});
      spdlog::logger lgr_on("test_logger", {sink_on});
      spdlog::logger lgr_off("test_logger", {sink_off});
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
      // 1 byte max -> every write rotates
      auto sink = std::make_shared<fc::json_rotating_file_sink_mt>(
         file.string(), 1, 3, std::map<std::string, std::string>{});
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
   // extra_fields must accept the natural JSON-object form (not fc's
   // default array-of-pairs map encoding) so operators can hand-edit
   // logging.json without surprise.
   auto cfg_json = R"(
   {
      "base_filename": "/tmp/x.jsonl",
      "max_size": 10,
      "max_files": 3,
      "extra_fields": { "env": "prod", "region": "us-east-1" }
   })";
   auto v = fc::json::from_string(cfg_json);
   auto cfg = v.as<fc::sink::json_rotating_file_sink_config>();
   BOOST_CHECK_EQUAL(cfg.base_filename, "/tmp/x.jsonl");
   BOOST_CHECK_EQUAL(cfg.max_size,  10u);
   BOOST_CHECK_EQUAL(cfg.max_files, 3u);
   BOOST_CHECK_EQUAL(cfg.extra_fields["env"].as_string(),    "prod");
   BOOST_CHECK_EQUAL(cfg.extra_fields["region"].as_string(), "us-east-1");

   // Empty extra_fields as empty object must also work.
   auto empty_json = R"(
   { "base_filename": "/tmp/y.jsonl", "max_size": 5, "max_files": 2, "extra_fields": {} })";
   auto ev = fc::json::from_string(empty_json);
   auto ecfg = ev.as<fc::sink::json_rotating_file_sink_config>();
   BOOST_CHECK_EQUAL(ecfg.extra_fields.size(), 0u);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(configure_logging_accepts_json_sinks) try {
   auto dir = make_temp_dir("cfg");
   fc::logging_config cfg;

   fc::sink_config s1;
   s1.name = "json_rot";
   s1.type = "json_rotating_file_sink";
   fc::sink::json_rotating_file_sink_config rot_cfg;
   rot_cfg.base_filename = (dir / "rot.jsonl").string();
   rot_cfg.max_size = 10;
   rot_cfg.max_files = 3;
   s1.args = fc::variant{rot_cfg};
   cfg.sinks.push_back(s1);

   fc::sink_config s2;
   s2.name = "json_daily";
   s2.type = "json_daily_file_sink";
   fc::sink::json_daily_file_sink_config day_cfg;
   day_cfg.base_filename = (dir / "daily.jsonl").string();
   s2.args = fc::variant{day_cfg};
   cfg.sinks.push_back(s2);

   fc::logger_config lcfg;
   lcfg.name = "test_cfg_logger";
   lcfg.level = fc::log_level::info;
   lcfg.enabled = true;
   lcfg.sinks = {"json_rot", "json_daily"};
   cfg.loggers.push_back(lcfg);

   BOOST_CHECK(fc::configure_logging(cfg));
   auto lgr = fc::log_config::get_logger("test_cfg_logger");
   fc_ilog(lgr, "configured message");

   fc::configure_logging(fc::logging_config::default_config());
   fs::remove_all(dir);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(multi_thread_no_corruption) try {
   auto dir  = make_temp_dir("mt");
   auto file = dir / "out.jsonl";
   constexpr int kThreads = 8;
   constexpr int kPerThread = 200;
   {
      auto sink = std::make_shared<fc::json_rotating_file_sink_mt>(
         file.string(), 100*1024*1024, 3, std::map<std::string, std::string>{});
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

BOOST_AUTO_TEST_SUITE_END()
