#include <boost/test/unit_test.hpp>

#include <fc/crypto/base64.hpp>
#include <fc/io/json.hpp>
#include <fc/log/es_sink.hpp>
#include <fc/log/json_formatter.hpp>
#include <fc/log/json_layout.hpp>
#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/variant_object.hpp>

#include <fc-test/capture_http_server.hpp>
#include <fc-test/one_shot_http_server.hpp>

#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr std::string_view test_index      = "test-logs";
constexpr auto             delivery_wait   = 5s;
constexpr auto             idle_wait       = 5s;
/// A window several interval-flush periods long; used to assert something did NOT
/// happen (e.g. no request from an unreferenced sink).
constexpr auto             quiet_window    = 300ms;

/// es_sink_config with fast test timings against @p url.
fc::sink::es_sink_config make_cfg(const std::string& url) {
   fc::sink::es_sink_config cfg;
   cfg.url                       = url;
   cfg.index                     = std::string{test_index};
   cfg.flush_interval_ms         = 50;
   cfg.retry_backoff_ms          = 10;
   cfg.connect_timeout_ms        = 1000;
   cfg.request_timeout_ms        = 2000;
   cfg.shutdown_flush_timeout_ms = 2000;
   return cfg;
}

/// Split an NDJSON bulk body into its lines (the trailing newline yields no entry).
std::vector<std::string> split_bulk_lines(const std::string& body) {
   std::vector<std::string> lines;
   std::stringstream        in{body};
   std::string              line;
   while (std::getline(in, line)) {
      lines.push_back(line);
   }
   return lines;
}

/// Log @p count records through a stack-local spdlog logger wired to @p sink.
void log_records(const std::shared_ptr<fc::es_sink_mt>& sink, int count, const std::string& payload = "record") {
   spdlog::logger lgr("es_test_logger", {sink});
   lgr.set_level(spdlog::level::trace);
   for (int i = 0; i < count; ++i) {
      SPDLOG_LOGGER_INFO(&lgr, "{} {}", payload, i);
   }
}

/// A successful default-body response whose delivery stalls for @p delay.
fc::test::capture_http_server::scripted_response delayed_ok(std::chrono::milliseconds delay) {
   fc::test::capture_http_server::scripted_response response;
   response.delay = delay;
   return response;
}

/// A 2-item bulk response: one indexed, one rejected with a reason.
std::string partial_failure_body() {
   return R"({"took":3,"errors":true,"items":[)"
          R"({"index":{"_index":"test-logs","status":201}},)"
          R"({"index":{"_index":"test-logs","status":400,"error":{"type":"mapper_parsing_exception","reason":"boom"}}})"
          R"(]})";
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(es_sink_tests)

// The ctor installs json_formatter with the es_default_layout template; delivered
// doc lines are es-shaped, the action line names the configured index, and the POST
// targets /_bulk with the NDJSON content type. The trailing-slash URL also pins the
// validate() slash-strip (a double slash would surface as //_bulk here).
BOOST_AUTO_TEST_CASE(default_formatter_is_es_layout) try {
   fc::test::capture_http_server server;
   auto cfg       = make_cfg(server.url() + "/"); // trailing slash on purpose
   cfg.batch_size = 1;
   auto sink      = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 1);
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));

   auto req = server.request(0);
   BOOST_CHECK_EQUAL(req.method, "POST");
   BOOST_CHECK_EQUAL(req.target, "/_bulk");
   BOOST_CHECK_EQUAL(req.header("content-type"), "application/x-ndjson");
   BOOST_CHECK(req.header("authorization").empty());
   BOOST_REQUIRE(!req.body.empty());
   BOOST_CHECK_EQUAL(req.body.back(), '\n');

   auto lines = split_bulk_lines(req.body);
   BOOST_REQUIRE_EQUAL(lines.size(), 2u);
   BOOST_CHECK_EQUAL(lines[0], R"({"index":{"_index":"test-logs"}})");
   BOOST_CHECK_EQUAL(lines[1].rfind(R"({"@timestamp":)", 0), 0u);
   auto doc = fc::json::from_string(lines[1]).get_object();
   BOOST_CHECK_EQUAL(doc["level"].as_string(), "INFO");
   BOOST_CHECK_EQUAL(doc["category"].as_string(), "es_test_logger");
   BOOST_CHECK_EQUAL(doc["message"].as_string(), "record 0");
   BOOST_CHECK(!doc["data"].get_object()["thread"].as_string().empty());
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(size_triggered_flush) try {
   fc::test::capture_http_server server;
   auto cfg              = make_cfg(server.url());
   cfg.batch_size        = 3;
   cfg.flush_interval_ms = 60'000; // interval must not be the trigger here
   auto sink             = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 3);
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   auto lines = split_bulk_lines(server.request(0).body);
   BOOST_CHECK_EQUAL(lines.size(), 6u); // 3 action/document pairs
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(byte_cap_flush) try {
   fc::test::capture_http_server server;
   auto cfg              = make_cfg(server.url());
   cfg.batch_size        = 1000;
   cfg.max_batch_bytes   = 512;
   cfg.max_doc_bytes     = 512;
   cfg.flush_interval_ms = 60'000;
   auto sink             = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 5, std::string(200, 'x'));
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   for (std::size_t i = 0; i < server.request_count(); ++i) {
      BOOST_CHECK_LE(server.request(i).body.size(), cfg.max_batch_bytes + cfg.max_doc_bytes);
   }
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(interval_flush) try {
   fc::test::capture_http_server server;
   auto cfg       = make_cfg(server.url());
   cfg.batch_size = 1000; // size must not be the trigger here
   auto sink      = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 1);
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   BOOST_CHECK_EQUAL(split_bulk_lines(server.request(0).body).size(), 2u);
} FC_LOG_AND_RETHROW()

// A document whose formatted size exceeds max_doc_bytes is dropped whole + counted;
// the surrounding documents still deliver.
BOOST_AUTO_TEST_CASE(oversized_doc_dropped) try {
   fc::test::capture_http_server server;
   auto cfg            = make_cfg(server.url());
   cfg.batch_size      = 2;
   cfg.max_batch_bytes = 4096;
   cfg.max_doc_bytes   = 300;
   auto sink           = std::make_shared<fc::es_sink_mt>(cfg);
   {
      spdlog::logger lgr("es_test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      SPDLOG_LOGGER_INFO(&lgr, "small one");
      SPDLOG_LOGGER_INFO(&lgr, "{}", std::string(400, 'y')); // formatted size > 300
      SPDLOG_LOGGER_INFO(&lgr, "small two");
   }
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   BOOST_CHECK_EQUAL(sink->dropped_docs(), 1u);
   std::string all_bodies;
   for (std::size_t i = 0; i < server.request_count(); ++i)
      all_bodies += server.request(i).body;
   BOOST_CHECK(all_bodies.find("small one") != std::string::npos);
   BOOST_CHECK(all_bodies.find(std::string(400, 'y')) == std::string::npos);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(basic_auth_header) try {
   fc::test::capture_http_server server;
   auto cfg       = make_cfg(server.url());
   cfg.batch_size = 1;
   cfg.username   = "u";
   cfg.password   = "p";
   auto sink      = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 1);
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   BOOST_CHECK_EQUAL(server.request(0).header("authorization"), "Basic " + fc::base64_encode(std::string{"u:p"}));
} FC_LOG_AND_RETHROW()

// A stalled endpoint + a full bounded queue must never block the logging thread:
// per-call latency stays far below the scripted service delay, and overflow batches
// are counted as dropped.
BOOST_AUTO_TEST_CASE(queue_full_drops_without_blocking) try {
   constexpr auto scripted_delay = 1500ms;
   fc::test::capture_http_server server{{delayed_ok(scripted_delay)}};
   auto cfg                = make_cfg(server.url());
   cfg.batch_size          = 1;
   cfg.max_pending_batches = 1;
   cfg.flush_interval_ms   = 60'000;
   auto sink               = std::make_shared<fc::es_sink_mt>(cfg);

   spdlog::logger lgr("es_test_logger", {sink});
   lgr.set_level(spdlog::level::trace);
   auto max_call = 0ms;
   for (int i = 0; i < 10; ++i) {
      const auto start = std::chrono::steady_clock::now();
      SPDLOG_LOGGER_INFO(&lgr, "burst {}", i);
      const auto elapsed =
         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
      max_call = std::max(max_call, elapsed);
   }
   // Relative bound: a memcpy + try_push must be far below the 1500 ms service delay
   // even on a loaded CI host.
   BOOST_CHECK_LT(max_call.count(), (scripted_delay / 5).count());
   BOOST_CHECK_GT(sink->dropped_batches(), 0u);
} FC_LOG_AND_RETHROW()

// max_retries counts ADDITIONAL attempts after the first: max_retries=1 -> exactly
// two identical requests when the first attempt gets a retryable 500.
BOOST_AUTO_TEST_CASE(retry_semantics_pinned) try {
   fc::test::capture_http_server server{{fc::test::capture_http_server::scripted_response{500, "boom", 0ms},
                                         fc::test::capture_http_server::scripted_response{}}};
   auto cfg        = make_cfg(server.url());
   cfg.batch_size  = 2;
   cfg.max_retries = 1;
   auto sink       = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 2);
   BOOST_REQUIRE(server.wait_for_requests(2, delivery_wait));
   BOOST_CHECK_EQUAL(server.request(0).body, server.request(1).body);
   BOOST_REQUIRE(sink->wait_until_idle(idle_wait));
   BOOST_CHECK_EQUAL(sink->failed_batches(), 0u);
   BOOST_CHECK_EQUAL(sink->indexed_docs(), 2u);
} FC_LOG_AND_RETHROW()

// 429 is endpoint back-pressure -- the one 4xx that retries.
BOOST_AUTO_TEST_CASE(retry_on_429) try {
   fc::test::capture_http_server server{{fc::test::capture_http_server::scripted_response{429, "slow down", 0ms},
                                         fc::test::capture_http_server::scripted_response{}}};
   auto cfg       = make_cfg(server.url());
   cfg.batch_size = 1;
   auto sink      = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 1);
   BOOST_REQUIRE(server.wait_for_requests(2, delivery_wait));
   BOOST_REQUIRE(sink->wait_until_idle(idle_wait));
   BOOST_CHECK_EQUAL(sink->indexed_docs(), 1u);
} FC_LOG_AND_RETHROW()

// Any other 4xx is terminal -- retrying a rejected request only repeats the rejection.
BOOST_AUTO_TEST_CASE(no_retry_on_4xx) try {
   fc::test::capture_http_server server{{fc::test::capture_http_server::scripted_response{400, "bad request", 0ms}}};
   auto cfg       = make_cfg(server.url());
   cfg.batch_size = 1;
   auto sink      = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 1);
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   BOOST_REQUIRE(sink->wait_until_idle(idle_wait));
   std::this_thread::sleep_for(quiet_window); // past any (wrong) backoff window
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
   BOOST_CHECK_EQUAL(sink->failed_batches(), 1u);
} FC_LOG_AND_RETHROW()

// HTTP 200 with "errors":true is a PARTIAL success: never retried (a replay would
// duplicate the indexed documents); per-item failures are accounted.
BOOST_AUTO_TEST_CASE(errors_true_no_retry_and_counts) try {
   fc::test::capture_http_server server{
      {fc::test::capture_http_server::scripted_response{200, partial_failure_body(), 0ms}}};
   auto cfg       = make_cfg(server.url());
   cfg.batch_size = 2;
   auto sink      = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 2);
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   BOOST_REQUIRE(sink->wait_until_idle(idle_wait));
   std::this_thread::sleep_for(quiet_window);
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
   BOOST_CHECK_EQUAL(sink->indexed_docs(), 1u);
   BOOST_CHECK_EQUAL(sink->failed_batches(), 1u);
} FC_LOG_AND_RETHROW()

// A 2xx that is not a bulk response (a proxy landing page) must never count as
// indexed -- the positive-signal probe requires an explicit "errors" verdict.
BOOST_AUTO_TEST_CASE(non_bulk_2xx_counts_failed) try {
   fc::test::capture_http_server server{
      {fc::test::capture_http_server::scripted_response{200, "<html>welcome</html>", 0ms}}};
   auto cfg       = make_cfg(server.url());
   cfg.batch_size = 1;
   auto sink      = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 1);
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   BOOST_REQUIRE(sink->wait_until_idle(idle_wait));
   BOOST_CHECK_EQUAL(sink->indexed_docs(), 0u);
   BOOST_CHECK_EQUAL(sink->failed_batches(), 1u);
} FC_LOG_AND_RETHROW()

// Destruction enqueues + delivers the below-threshold tail batch.
BOOST_AUTO_TEST_CASE(shutdown_drains_tail) try {
   fc::test::capture_http_server server;
   auto cfg              = make_cfg(server.url());
   cfg.batch_size        = 1000;
   cfg.flush_interval_ms = 60'000;
   {
      auto sink = std::make_shared<fc::es_sink_mt>(cfg);
      log_records(sink, 2);
   } // logger + sink destroyed here -- the tail must still ship
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   BOOST_CHECK_EQUAL(split_bulk_lines(server.request(0).body).size(), 4u);
} FC_LOG_AND_RETHROW()

// The graceful drain waits for the IN-FLIGHT delivery, not just an empty queue:
// with a scripted response delay far above the drain poll, the tail batch must still
// complete instead of being cancelled the moment the worker pops it.
BOOST_AUTO_TEST_CASE(shutdown_drains_tail_with_slow_response) try {
   fc::test::capture_http_server server{{delayed_ok(300ms)}};
   auto cfg              = make_cfg(server.url());
   cfg.batch_size        = 2;
   cfg.flush_interval_ms = 60'000;
   auto sink             = std::make_shared<fc::es_sink_mt>(cfg);
   log_records(sink, 2); // batch_size hit -> enqueued -> worker stalls in the delayed response
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   // wait_until_idle shares the destructor's drain predicate: it must report BUSY
   // while the 300 ms response delay stalls the worker (the queue is already empty
   // here -- only the in-flight counter can know), then idle once delivery completes.
   BOOST_CHECK(!sink->wait_until_idle(50ms));
   BOOST_REQUIRE(sink->wait_until_idle(idle_wait));
   BOOST_CHECK_EQUAL(sink->indexed_docs(), 2u);
   BOOST_CHECK_EQUAL(sink->failed_batches(), 0u);
   sink.reset(); // destructor path with nothing left in flight
} FC_LOG_AND_RETHROW()

// An endpoint that kills connections must neither block the logging thread nor hang
// destruction.
BOOST_AUTO_TEST_CASE(unreachable_endpoint_never_blocks_logging) try {
   fc::test::connection_closing_http_server closing_server;
   auto cfg              = make_cfg(closing_server.url());
   cfg.batch_size        = 1;
   cfg.max_retries       = 0;
   cfg.flush_interval_ms = 60'000;
   auto sink             = std::make_shared<fc::es_sink_mt>(cfg);

   spdlog::logger lgr("es_test_logger", {sink});
   lgr.set_level(spdlog::level::trace);
   const auto start = std::chrono::steady_clock::now();
   SPDLOG_LOGGER_INFO(&lgr, "into the void");
   const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
   BOOST_CHECK_LT(elapsed.count(), 300);

   BOOST_REQUIRE(sink->wait_until_idle(idle_wait));
   BOOST_CHECK_EQUAL(sink->failed_batches(), 1u);
   BOOST_CHECK_EQUAL(sink->indexed_docs(), 0u);
} FC_LOG_AND_RETHROW()

// Eight producer threads through one sink: every delivered line parses, bodies hold
// an even action/document line count, and nothing is lost or duplicated.
BOOST_AUTO_TEST_CASE(concurrent_multithread_no_corruption) try {
   constexpr int threads_count     = 8;
   constexpr int records_per_thread = 100;
   fc::test::capture_http_server server;
   auto cfg                = make_cfg(server.url());
   cfg.batch_size          = 100;
   cfg.max_pending_batches = 64;
   {
      auto sink = std::make_shared<fc::es_sink_mt>(cfg);
      spdlog::logger lgr("es_test_logger", {sink});
      lgr.set_level(spdlog::level::trace);
      std::vector<std::thread> producers;
      producers.reserve(threads_count);
      for (int t = 0; t < threads_count; ++t) {
         producers.emplace_back([&lgr, t] {
            for (int i = 0; i < records_per_thread; ++i) {
               SPDLOG_LOGGER_INFO(&lgr, "t{} r{}", t, i);
            }
         });
      }
      for (auto& producer : producers)
         producer.join();
   } // destruction drains everything
   std::size_t total_docs = 0;
   for (std::size_t i = 0; i < server.request_count(); ++i) {
      auto lines = split_bulk_lines(server.request(i).body);
      BOOST_REQUIRE_EQUAL(lines.size() % 2, 0u);
      for (std::size_t l = 1; l < lines.size(); l += 2) {
         BOOST_CHECK_NO_THROW(fc::json::from_string(lines[l]));
      }
      total_docs += lines.size() / 2;
   }
   BOOST_CHECK_EQUAL(total_docs, static_cast<std::size_t>(threads_count * records_per_thread));
} FC_LOG_AND_RETHROW()

// Two es_sinks in one process: independent transports/workers, independent targets.
BOOST_AUTO_TEST_CASE(two_es_sinks_coexist) try {
   fc::test::capture_http_server server_a;
   fc::test::capture_http_server server_b;
   auto cfg_a       = make_cfg(server_a.url());
   cfg_a.batch_size = 1;
   auto cfg_b       = make_cfg(server_b.url());
   cfg_b.batch_size = 1;
   cfg_b.index      = "other-index";
   auto sink_a      = std::make_shared<fc::es_sink_mt>(cfg_a);
   auto sink_b      = std::make_shared<fc::es_sink_mt>(cfg_b);
   log_records(sink_a, 1, "for a");
   log_records(sink_b, 1, "for b");
   BOOST_REQUIRE(server_a.wait_for_requests(1, delivery_wait));
   BOOST_REQUIRE(server_b.wait_for_requests(1, delivery_wait));
   BOOST_CHECK(server_a.request(0).body.find("for a") != std::string::npos);
   BOOST_CHECK(server_b.request(0).body.find("for b") != std::string::npos);
   BOOST_CHECK(server_b.request(0).body.find(R"("_index":"other-index")") != std::string::npos);
} FC_LOG_AND_RETHROW()

// ------------------------------------------------------------------------------
// configure_logging end-to-end cases. Each restores the default config on exit.
// ------------------------------------------------------------------------------

namespace {

/// A logging_config with one es_sink (from @p args) referenced by one logger, plus
/// an optional format block.
fc::logging_config make_es_logging_config(const fc::sink::es_sink_config& es_cfg, const std::string& logger_name,
                                          std::optional<fc::format_config> format = {}) {
   fc::logging_config cfg;
   fc::sink_config    s;
   s.name   = "es";
   s.type   = "es_sink";
   s.args   = fc::variant{es_cfg};
   s.format = std::move(format);
   cfg.sinks.push_back(s);
   fc::logger_config lcfg;
   lcfg.name    = logger_name;
   lcfg.level   = fc::log_level::info;
   lcfg.enabled = true;
   lcfg.sinks   = {"es"};
   cfg.loggers.push_back(lcfg);
   return cfg;
}

} // anonymous namespace

// No format block: the sink's own default (json + es_default_layout) survives the
// formatter-attachment pass -- a pattern-formatted line would not start {"@timestamp".
BOOST_AUTO_TEST_CASE(configure_logging_es_sink_default_format) try {
   auto restore = fc::make_scoped_exit([] { fc::configure_logging(fc::logging_config::default_config()); });
   fc::test::capture_http_server server;
   auto es_cfg       = make_cfg(server.url());
   es_cfg.batch_size = 1;
   BOOST_REQUIRE(fc::configure_logging(make_es_logging_config(es_cfg, "test_es_default_fmt_logger")));

   auto lgr = fc::log_config::get_logger("test_es_default_fmt_logger");
   fc_ilog(lgr, "configured record");
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   auto lines = split_bulk_lines(server.request(0).body);
   BOOST_REQUIRE_EQUAL(lines.size(), 2u);
   BOOST_CHECK_EQUAL(lines[1].rfind(R"({"@timestamp":)", 0), 0u);
} FC_LOG_AND_RETHROW()

// Explicit json format block with the es template + extra_fields: the identity
// fields land at the document top level (mirrors logging_elasticsearch.json).
BOOST_AUTO_TEST_CASE(configure_logging_es_sink_explicit_format_override) try {
   auto restore = fc::make_scoped_exit([] { fc::configure_logging(fc::logging_config::default_config()); });
   fc::test::capture_http_server server;
   auto es_cfg       = make_cfg(server.url());
   es_cfg.batch_size = 1;

   fc::format_config fmt_cfg;
   fmt_cfg.type = "json";
   fc::format::json_config jc;
   jc.layout       = std::string{fc::log::es_default_layout};
   jc.extra_fields = fc::mutable_variant_object{}("env", "local-test")("app", "es-sink-test");
   fmt_cfg.args    = fc::variant{jc};

   BOOST_REQUIRE(
      fc::configure_logging(make_es_logging_config(es_cfg, "test_es_explicit_fmt_logger", fmt_cfg)));
   auto lgr = fc::log_config::get_logger("test_es_explicit_fmt_logger");
   fc_ilog(lgr, "stamped record");
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   auto doc = fc::json::from_string(split_bulk_lines(server.request(0).body)[1]).get_object();
   BOOST_CHECK_EQUAL(doc["env"].as_string(), "local-test");
   BOOST_CHECK_EQUAL(doc["app"].as_string(), "es-sink-test");
} FC_LOG_AND_RETHROW()

// A custom template with different field names is ACCEPTED (an open layout is the
// point) as long as it renders valid one-line JSON.
BOOST_AUTO_TEST_CASE(configure_logging_accepts_custom_json_layout_on_es_sink) try {
   auto restore = fc::make_scoped_exit([] { fc::configure_logging(fc::logging_config::default_config()); });
   fc::test::capture_http_server server;
   auto es_cfg       = make_cfg(server.url());
   es_cfg.batch_size = 1;

   fc::format_config fmt_cfg;
   fmt_cfg.type = "json";
   fc::format::json_config jc;
   jc.layout    = R"({"m":"${message}","src":"custom"})";
   fmt_cfg.args = fc::variant{jc};

   BOOST_REQUIRE(fc::configure_logging(make_es_logging_config(es_cfg, "test_es_custom_layout_logger", fmt_cfg)));
   auto lgr = fc::log_config::get_logger("test_es_custom_layout_logger");
   fc_ilog(lgr, "custom shape");
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   auto doc = fc::json::from_string(split_bulk_lines(server.request(0).body)[1]).get_object();
   BOOST_CHECK_EQUAL(doc["m"].as_string(), "custom shape");
   BOOST_CHECK_EQUAL(doc["src"].as_string(), "custom");
} FC_LOG_AND_RETHROW()

// A non-json format block would make every bulk request malformed NDJSON -> rejected.
BOOST_AUTO_TEST_CASE(configure_logging_rejects_non_json_format_on_es_sink) try {
   auto restore = fc::make_scoped_exit([] { fc::configure_logging(fc::logging_config::default_config()); });
   fc::test::capture_http_server server;
   fc::format_config pattern_fmt;
   pattern_fmt.type = "pattern";
   BOOST_CHECK(
      !fc::configure_logging(make_es_logging_config(make_cfg(server.url()), "test_es_pattern_logger", pattern_fmt)));
} FC_LOG_AND_RETHROW()

// A json layout that renders non-JSON text fails the configure-time sample render.
BOOST_AUTO_TEST_CASE(configure_logging_rejects_non_json_rendering_layout) try {
   auto restore = fc::make_scoped_exit([] { fc::configure_logging(fc::logging_config::default_config()); });
   fc::test::capture_http_server server;
   fc::format_config fmt_cfg;
   fmt_cfg.type = "json";
   fc::format::json_config jc;
   jc.layout    = R"(hello ${message})";
   fmt_cfg.args = fc::variant{jc};
   BOOST_CHECK(
      !fc::configure_logging(make_es_logging_config(make_cfg(server.url()), "test_es_nonjson_logger", fmt_cfg)));
} FC_LOG_AND_RETHROW()

// Minimal JSON args land on the in-struct defaults.
BOOST_AUTO_TEST_CASE(es_config_defaults_from_json) try {
   // Loopback placeholder URL -- parsed only, never dialed (no sink is constructed).
   auto cfg = fc::json::from_string(R"({"url":"http://127.0.0.1:1","index":"i"})").as<fc::sink::es_sink_config>();
   BOOST_CHECK_EQUAL(cfg.url, "http://127.0.0.1:1");
   BOOST_CHECK_EQUAL(cfg.index, "i");
   BOOST_CHECK(!cfg.username.has_value());
   BOOST_CHECK(!cfg.password.has_value());
   BOOST_CHECK_EQUAL(cfg.batch_size, fc::sink::default_es_batch_size);
   BOOST_CHECK_EQUAL(cfg.max_batch_bytes, fc::sink::default_es_max_batch_bytes);
   BOOST_CHECK_EQUAL(cfg.max_doc_bytes, fc::sink::default_es_max_doc_bytes);
   BOOST_CHECK_EQUAL(cfg.flush_interval_ms, fc::sink::default_es_flush_interval_ms);
   BOOST_CHECK_EQUAL(cfg.max_pending_batches, fc::sink::default_es_max_pending_batches);
   BOOST_CHECK_EQUAL(cfg.max_retries, fc::sink::default_es_max_retries);
   BOOST_CHECK_EQUAL(cfg.retry_backoff_ms, fc::sink::default_es_retry_backoff_ms);
   BOOST_CHECK_EQUAL(cfg.connect_timeout_ms, fc::sink::default_es_connect_timeout_ms);
   BOOST_CHECK_EQUAL(cfg.request_timeout_ms, fc::sink::default_es_request_timeout_ms);
   BOOST_CHECK_EQUAL(cfg.shutdown_flush_timeout_ms, fc::sink::default_es_shutdown_flush_timeout_ms);
} FC_LOG_AND_RETHROW()

// Every construction-time invariant fails configure_logging cleanly.
BOOST_AUTO_TEST_CASE(configure_logging_rejects_invalid_es_config) try {
   auto restore = fc::make_scoped_exit([] { fc::configure_logging(fc::logging_config::default_config()); });
   fc::test::capture_http_server server;

   auto no_url = make_cfg(server.url());
   no_url.url  = "";
   BOOST_CHECK(!fc::configure_logging(make_es_logging_config(no_url, "test_es_invalid_logger")));

   auto bad_scheme = make_cfg(server.url());
   bad_scheme.url  = "file:///tmp/nope";
   BOOST_CHECK(!fc::configure_logging(make_es_logging_config(bad_scheme, "test_es_invalid_logger")));

   auto no_index  = make_cfg(server.url());
   no_index.index = "";
   BOOST_CHECK(!fc::configure_logging(make_es_logging_config(no_index, "test_es_invalid_logger")));

   auto zero_batch       = make_cfg(server.url());
   zero_batch.batch_size = 0;
   BOOST_CHECK(!fc::configure_logging(make_es_logging_config(zero_batch, "test_es_invalid_logger")));

   auto oversized_doc          = make_cfg(server.url());
   oversized_doc.max_doc_bytes = oversized_doc.max_batch_bytes + 1;
   BOOST_CHECK(!fc::configure_logging(make_es_logging_config(oversized_doc, "test_es_invalid_logger")));

   auto half_auth     = make_cfg(server.url());
   half_auth.username = "user-without-password";
   BOOST_CHECK(!fc::configure_logging(make_es_logging_config(half_auth, "test_es_invalid_logger")));
} FC_LOG_AND_RETHROW()

// A defined-but-unreferenced es_sink is never constructed: no worker, no connection.
BOOST_AUTO_TEST_CASE(unreferenced_es_sink_not_built) try {
   auto restore = fc::make_scoped_exit([] { fc::configure_logging(fc::logging_config::default_config()); });
   fc::test::capture_http_server server;
   auto cfg = make_es_logging_config(make_cfg(server.url()), "test_es_unref_logger");
   cfg.loggers[0].sinks = {}; // nothing references the sink
   BOOST_CHECK(fc::configure_logging(cfg));
   std::this_thread::sleep_for(quiet_window); // several interval periods
   BOOST_CHECK_EQUAL(server.request_count(), 0u);
} FC_LOG_AND_RETHROW()

// The SIGHUP path -- reconfigure destroys the old sink (tail delivered, threads
// joined) and a fresh es_sink works afterwards.
BOOST_AUTO_TEST_CASE(reconfigure_destroys_cleanly) try {
   auto restore = fc::make_scoped_exit([] { fc::configure_logging(fc::logging_config::default_config()); });
   fc::test::capture_http_server first_server;
   fc::test::capture_http_server second_server;

   auto first_cfg       = make_cfg(first_server.url());
   first_cfg.batch_size = 1;
   BOOST_REQUIRE(fc::configure_logging(make_es_logging_config(first_cfg, "test_es_reconfig_logger")));
   auto lgr = fc::log_config::get_logger("test_es_reconfig_logger");
   fc_ilog(lgr, "first generation");
   BOOST_REQUIRE(first_server.wait_for_requests(1, delivery_wait));

   // The SIGHUP handler's shape: re-run configure_logging (clears + rebuilds).
   BOOST_REQUIRE(fc::configure_logging(fc::logging_config::default_config()));

   auto second_cfg       = make_cfg(second_server.url());
   second_cfg.batch_size = 1;
   BOOST_REQUIRE(fc::configure_logging(make_es_logging_config(second_cfg, "test_es_reconfig_logger")));
   auto second_lgr = fc::log_config::get_logger("test_es_reconfig_logger");
   fc_ilog(second_lgr, "second generation");
   BOOST_REQUIRE(second_server.wait_for_requests(1, delivery_wait));
   BOOST_CHECK(second_server.request(0).body.find("second generation") != std::string::npos);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
