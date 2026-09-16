#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <fc-test/capture_http_server.hpp>
#include <fc-test/one_shot_http_server.hpp>
#include <fc/crypto/base64.hpp>
#include <fc/exception/exception.hpp>
#include <fc/network/es/es_client.hpp>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using fc::network::es::es_bulk_result;
using fc::network::es::es_client;
using fc::test::bulk_partial_failure_body;
using fc::test::delayed_ok;
using fc::test::split_bulk_lines;

namespace {

constexpr std::string_view test_index = "test-status";
constexpr std::string_view sample_document = R"({"@timestamp":1700000000123,"message":"sample"})"
                                             "\n";
constexpr uint32_t fast_backoff_ms = 10;
constexpr uint32_t two_retries = 2;
constexpr auto delivery_wait = 5s;
/// A backoff long enough that only cancel() can end the wait inside the test's budget.
constexpr uint32_t long_backoff_ms = 60'000;
constexpr auto cancel_budget = 2s;
constexpr uint32_t no_retries = 0;
constexpr uint32_t one_retry = 1;
/// A scripted service delay many times short_timeout_ms: delivery times out on the header.
constexpr auto timeout_delay = 3s;
/// The timeout case's own request timeout: 6x inside timeout_delay, so the deadline is reached with a wide
/// margin on a loaded host without lengthening the scripted delay.
constexpr uint32_t short_timeout_ms = 500;
/// A scripted service delay that holds one request in flight while a second is attempted, with a request
/// timeout generous enough that the held request still succeeds once the delay elapses.
constexpr auto overlap_delay = 1s;
constexpr uint32_t slow_timeout_ms = 10'000;
/// Bounds the attempt that connects into a closed server's backlog and is never answered.
constexpr uint32_t dead_timeout_ms = 500;
/// A 2xx body that is not JSON at all: the full parse behind the success probe must fail.
constexpr std::string_view html_2xx_body = "<html>";
/// The path probe() requests: the base URL's root.
constexpr std::string_view probe_target = "/";
/// Comfortably inside make_options()'s request timeout, so a probe that returns within it cannot have made a
/// second attempt: that attempt could only end at the 2000 ms request timeout against an endpoint that
/// answers no attempt but the first.
constexpr auto probe_attempt_budget = 1500ms;
/// An origin that never resolves: only a proxy can deliver a request addressed to it.
constexpr std::string_view unresolvable_origin = "http://origin.invalid:9200";

/// Client options with fast test timings against @p url.
fc::network::es::es_client_options make_options(const std::string& url) {
   fc::network::es::es_client_options options;
   options.url = url;
   options.index = std::string{test_index};
   options.retry_backoff_ms = fast_backoff_ms;
   options.connect_timeout_ms = 1000;
   options.request_timeout_ms = 2000;
   return options;
}

/// One action/document pair, as a producer assembles it.
std::string one_document_body(const es_client& client) {
   return client.action_line() + std::string{sample_document};
}

/// A scripted reply with @p status and @p body.
fc::test::capture_http_server::scripted_response reply(unsigned status, std::string body = {}) {
   fc::test::capture_http_server::scripted_response response;
   response.status = status;
   response.body = std::move(body);
   return response;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(es_client_tests)

BOOST_AUTO_TEST_CASE(validate_normalizes_the_url_and_rejects_bad_options) try {
   auto options = make_options("http://127.0.0.1:9/");
   BOOST_CHECK_EQUAL(es_client::validate(options).url, "http://127.0.0.1:9");

   auto bad_scheme = options;
   bad_scheme.url = "ftp://127.0.0.1:9";
   BOOST_CHECK_THROW(es_client::validate(bad_scheme), fc::exception);
   auto no_index = options;
   no_index.index = "";
   BOOST_CHECK_THROW(es_client::validate(no_index), fc::exception);
   auto doc_over_batch = options;
   doc_over_batch.max_doc_bytes = doc_over_batch.max_batch_bytes + 1;
   BOOST_CHECK_THROW(es_client::validate(doc_over_batch), fc::exception);
   auto batch_over_ceiling = options;
   batch_over_ceiling.max_batch_bytes = fc::network::es::es_max_batch_bytes_ceiling + 1;
   BOOST_CHECK_THROW(es_client::validate(batch_over_ceiling), fc::exception);
   auto half_auth = options;
   half_auth.username = "u";
   BOOST_CHECK_THROW(es_client::validate(half_auth), fc::exception);
}
FC_LOG_AND_RETHROW()

// The trailing-slash URL also pins the validate() slash-strip (a double slash would surface as //_bulk).
BOOST_AUTO_TEST_CASE(bulk_posts_ndjson_and_reports_indexed) try {
   fc::test::capture_http_server server;
   es_client client{make_options(server.url() + "/")};
   BOOST_CHECK_EQUAL(client.action_line(), R"({"index":{"_index":"test-status"}})"
                                           "\n");

   const auto result = client.bulk(one_document_body(client), 1);
   BOOST_CHECK(result.outcome == es_bulk_result::status::indexed);
   BOOST_CHECK_EQUAL(result.attempts, 1u);
   BOOST_CHECK_EQUAL(result.indexed_docs, 1u);
   BOOST_CHECK_EQUAL(result.failed_docs, 0u);
   BOOST_CHECK(result.detail.empty());

   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   const auto req = server.request(0);
   BOOST_CHECK_EQUAL(req.method, "POST");
   BOOST_CHECK_EQUAL(req.target, "/_bulk");
   BOOST_CHECK_EQUAL(req.header("content-type"), "application/x-ndjson");
   BOOST_CHECK_EQUAL(req.header("user-agent"), "wire-es-client");
   BOOST_CHECK(req.header("authorization").empty());
   const auto lines = split_bulk_lines(req.body);
   BOOST_REQUIRE_EQUAL(lines.size(), 2u);
   BOOST_CHECK_EQUAL(lines[0], R"({"index":{"_index":"test-status"}})");
   BOOST_CHECK_EQUAL(lines[1] + "\n", std::string{sample_document});
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(basic_auth_header_is_sent_when_configured) try {
   fc::test::capture_http_server server;
   auto options = make_options(server.url());
   options.username = "u";
   options.password = "p";
   es_client client{options};
   BOOST_CHECK(client.bulk(one_document_body(client), 1).outcome == es_bulk_result::status::indexed);
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   BOOST_CHECK_EQUAL(server.request(0).header("authorization"), "Basic " + fc::base64_encode(std::string{"u:p"}));
}
FC_LOG_AND_RETHROW()

// A 2xx is never retried: the documents that DID index must not be replayed.
BOOST_AUTO_TEST_CASE(partial_failure_is_accounted_and_not_retried) try {
   fc::test::capture_http_server server{{reply(200, bulk_partial_failure_body(test_index))}};
   es_client client{make_options(server.url())};
   const auto result = client.bulk(one_document_body(client) + one_document_body(client), 2);
   BOOST_CHECK(result.outcome == es_bulk_result::status::partial);
   BOOST_CHECK_EQUAL(result.attempts, 1u);
   BOOST_CHECK_EQUAL(result.indexed_docs, 1u);
   BOOST_CHECK_EQUAL(result.failed_docs, 1u);
   BOOST_CHECK(result.detail.find("boom") != std::string::npos);
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(retries_5xx_and_429_then_reports_unavailable) try {
   fc::test::capture_http_server server{
      {reply(503), reply(429), reply(500)}
   };
   auto options = make_options(server.url());
   options.max_retries = two_retries;
   es_client client{options};
   const auto result = client.bulk(one_document_body(client), 1);
   BOOST_CHECK(result.outcome == es_bulk_result::status::unavailable);
   BOOST_CHECK_EQUAL(result.attempts, two_retries + 1);
   BOOST_CHECK_EQUAL(result.failed_docs, 1u);
   BOOST_CHECK(result.detail.find("HTTP 500") != std::string::npos);
   BOOST_CHECK_EQUAL(server.request_count(), two_retries + 1);
   // Every attempt carries the identical body: a retry is a replay, not a re-assembly.
   BOOST_CHECK_EQUAL(server.request(0).body, server.request(2).body);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(a_retry_that_succeeds_reports_indexed_with_its_attempt_count) try {
   fc::test::capture_http_server server{{reply(503)}}; // the script's end falls back to the default success body
   es_client client{make_options(server.url())};
   const auto result = client.bulk(one_document_body(client), 1);
   BOOST_CHECK(result.outcome == es_bulk_result::status::indexed);
   BOOST_CHECK_EQUAL(result.attempts, 2u);
   BOOST_CHECK_EQUAL(result.indexed_docs, 1u);
}
FC_LOG_AND_RETHROW()

// An endpoint that never starts answering within the request timeout is a delivery failure, not a rejection:
// nothing was acknowledged, so the batch is unavailable rather than refused.
BOOST_AUTO_TEST_CASE(request_timeout_reports_unavailable) try {
   fc::test::capture_http_server server{{delayed_ok(timeout_delay)}};
   auto options = make_options(server.url());
   options.max_retries = no_retries;
   options.request_timeout_ms = short_timeout_ms; // 6x inside timeout_delay -- the deadline always wins
   es_client client{options};
   const auto result = client.bulk(one_document_body(client), 1);
   BOOST_CHECK(result.outcome == es_bulk_result::status::unavailable);
   BOOST_CHECK_EQUAL(result.attempts, 1u);
   BOOST_CHECK_EQUAL(result.indexed_docs, 0u);
   BOOST_CHECK_EQUAL(result.failed_docs, 1u);
   BOOST_CHECK(result.detail.find("timeout") != std::string::npos);
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
}
FC_LOG_AND_RETHROW()

// A connection failure is retried to exhaustion and then reported unavailable. The fixture owns its port
// until the client connects, so no other process can race the test onto a working endpoint.
BOOST_AUTO_TEST_CASE(unreachable_endpoint_reports_unavailable_after_retries) try {
   fc::test::connection_closing_http_server closing_server;
   auto options = make_options(closing_server.url());
   options.max_retries = one_retry;
   options.request_timeout_ms = dead_timeout_ms; // bounds an attempt the dead endpoint never answers
   es_client client{options};
   const auto result = client.bulk(one_document_body(client), 1);
   BOOST_CHECK(result.outcome == es_bulk_result::status::unavailable);
   BOOST_CHECK_EQUAL(result.attempts, one_retry + 1);
   BOOST_CHECK_EQUAL(result.indexed_docs, 0u);
   BOOST_CHECK_EQUAL(result.failed_docs, 1u);
   BOOST_CHECK(!result.detail.empty());
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(other_4xx_and_non_bulk_2xx_are_rejected_without_retry) try {
   // The non-bulk 2xx is a JSON object with no "errors" verdict: the probe requires that verdict explicitly.
   fc::test::capture_http_server server{
      {reply(400), reply(200, R"({"took":1})")}
   };
   es_client client{make_options(server.url())};
   const auto rejected = client.bulk(one_document_body(client), 1);
   BOOST_CHECK(rejected.outcome == es_bulk_result::status::rejected);
   BOOST_CHECK_EQUAL(rejected.attempts, 1u);
   BOOST_CHECK(rejected.detail.find("HTTP 400") != std::string::npos);
   const auto not_bulk = client.bulk(one_document_body(client), 1);
   BOOST_CHECK(not_bulk.outcome == es_bulk_result::status::rejected);
   BOOST_CHECK(not_bulk.detail.find("not a bulk response") != std::string::npos);
   BOOST_CHECK_EQUAL(server.request_count(), 2u);
}
FC_LOG_AND_RETHROW()

// A 2xx whose body is not JSON at all fails the full parse behind the success probe. Like every other 2xx it
// is terminal: a replay could duplicate documents the endpoint may already have indexed.
BOOST_AUTO_TEST_CASE(unparseable_2xx_is_rejected_without_retry) try {
   fc::test::capture_http_server server{{reply(200, std::string{html_2xx_body})}};
   auto options = make_options(server.url());
   options.max_retries = two_retries;
   es_client client{options};
   const auto result = client.bulk(one_document_body(client), 1);
   BOOST_CHECK(result.outcome == es_bulk_result::status::rejected);
   BOOST_CHECK_EQUAL(result.attempts, 1u);
   BOOST_CHECK_EQUAL(result.indexed_docs, 0u);
   BOOST_CHECK_EQUAL(result.failed_docs, 1u);
   BOOST_CHECK(result.detail.find("unparseable 2xx response") != std::string::npos);
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
}
FC_LOG_AND_RETHROW()

// The coroutine is the client's real API -- bulk() is its blocking convenience for a worker thread -- so a
// caller on the client's executor gets the same result without any thread of its own blocking.
BOOST_AUTO_TEST_CASE(async_bulk_runs_on_the_client_executor) try {
   fc::test::capture_http_server server;
   es_client client{make_options(server.url())};
   const auto result = boost::asio::co_spawn(client.get_executor(), client.async_bulk(one_document_body(client), 1),
                                             boost::asio::use_future)
                          .get();
   BOOST_CHECK(result.outcome == es_bulk_result::status::indexed);
   BOOST_CHECK_EQUAL(result.attempts, 1u);
   BOOST_CHECK_EQUAL(result.indexed_docs, 1u);
   // An indexed result is the synchronization point: the stub has recorded and answered the request.
   BOOST_REQUIRE_EQUAL(server.request_count(), 1u);
   BOOST_CHECK_EQUAL(server.request(0).target, "/_bulk");
}
FC_LOG_AND_RETHROW()

// The cancellation signal holds a single slot, so the client admits one request at a time: a second
// async_bulk() while the first is in flight is an FC_ASSERT, and the first request is untouched by it.
BOOST_AUTO_TEST_CASE(overlapping_requests_are_rejected) try {
   fc::test::capture_http_server server{{delayed_ok(overlap_delay)}};
   auto options = make_options(server.url());
   options.request_timeout_ms = slow_timeout_ms; // the held request must outlive overlap_delay
   es_client client{options};

   auto first = boost::asio::co_spawn(client.get_executor(), client.async_bulk(one_document_body(client), 1),
                                      boost::asio::use_future);
   // The recorded request is the synchronization point: the first coroutine is past the guard and awaiting a
   // response the stub is still holding back.
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   auto second = boost::asio::co_spawn(client.get_executor(), client.async_bulk(one_document_body(client), 1),
                                       boost::asio::use_future);
   // bulk() folds a throwing coroutine into an `unavailable` value, so the assert is observed on the
   // coroutine's own future instead.
   BOOST_CHECK_THROW(second.get(), fc::assert_exception);

   const auto result = first.get();
   BOOST_CHECK(result.outcome == es_bulk_result::status::indexed);
   BOOST_CHECK_EQUAL(result.attempts, 1u);
   BOOST_CHECK_EQUAL(result.indexed_docs, 1u);
   // The rejected request left the flag to its owner, which cleared it on completion: the next one is admitted.
   BOOST_CHECK(client.bulk(one_document_body(client), 1).outcome == es_bulk_result::status::indexed);
   BOOST_CHECK_EQUAL(server.request_count(), 2u);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(cancel_interrupts_the_backoff_wait) try {
   fc::test::capture_http_server server{{reply(503)}};
   auto options = make_options(server.url());
   options.retry_backoff_ms = long_backoff_ms;
   es_client client{options};
   std::optional<es_bulk_result> result;
   std::thread sender{[&] { result = client.bulk(one_document_body(client), 1); }};
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait)); // the first attempt failed; the client is backing off
   const auto started = std::chrono::steady_clock::now();
   client.cancel();
   sender.join();
   BOOST_CHECK(std::chrono::steady_clock::now() - started < cancel_budget);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK(result->outcome == es_bulk_result::status::canceled);
   BOOST_CHECK_EQUAL(result->failed_docs, 1u);
   // Once canceled, the client stays canceled: a later bulk() returns immediately.
   BOOST_CHECK(client.bulk(one_document_body(client), 1).outcome == es_bulk_result::status::canceled);
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
}
FC_LOG_AND_RETHROW()

// probe() is the producer's one connectivity check at initialization: a single GET of the base URL's root,
// carrying the same credentials and User-Agent a bulk request would.
BOOST_AUTO_TEST_CASE(probe_succeeds_on_2xx) try {
   fc::test::capture_http_server server;
   auto options = make_options(server.url());
   options.username = "u";
   options.password = "p";
   es_client client{options};
   BOOST_CHECK_NO_THROW(client.probe());

   BOOST_REQUIRE_EQUAL(server.request_count(), 1u);
   const auto req = server.request(0);
   BOOST_CHECK_EQUAL(req.method, "GET");
   BOOST_CHECK_EQUAL(req.target, std::string{probe_target});
   BOOST_CHECK_EQUAL(req.header("user-agent"), "wire-es-client");
   BOOST_CHECK_EQUAL(req.header("authorization"), "Basic " + fc::base64_encode(std::string{"u:p"}));
   BOOST_CHECK(req.body.empty());
   // The guard the probe shares with a bulk request is released again: delivery still works afterwards.
   BOOST_CHECK(client.bulk(one_document_body(client), 1).outcome == es_bulk_result::status::indexed);
   BOOST_CHECK_EQUAL(server.request_count(), 2u);
}
FC_LOG_AND_RETHROW()

// An endpoint that answers something other than a 2xx is a configuration failure the producer must not start
// past -- and it is not retried, however many retries the delivery options allow.
BOOST_AUTO_TEST_CASE(probe_throws_on_non_2xx) try {
   fc::test::capture_http_server server{{reply(503)}};
   es_client client{make_options(server.url())};
   BOOST_CHECK_THROW(client.probe(), fc::exception);
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
}
FC_LOG_AND_RETHROW()

// The exception to the rule above: the probed root is the cluster-info path, which OpenSearch fine-grained
// access control gates behind the cluster `monitor/main` permission. A credential scoped to writing _bulk is
// answered with a 403 there while delivery works, so the endpoint answering at all -- and accepting the
// credential -- is what the check is after.
BOOST_AUTO_TEST_CASE(probe_accepts_403_as_reachable) try {
   fc::test::capture_http_server server{{reply(403)}};
   es_client client{make_options(server.url())};
   BOOST_CHECK_NO_THROW(client.probe());
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
}
FC_LOG_AND_RETHROW()

// A 401 is the credential itself being rejected, not a missing cluster-info permission: delivery would be
// rejected the same way, so startup must not continue past it.
BOOST_AUTO_TEST_CASE(probe_throws_on_401) try {
   fc::test::capture_http_server server{{reply(401)}};
   es_client client{make_options(server.url())};
   BOOST_CHECK_THROW(client.probe(), fc::exception);
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
}
FC_LOG_AND_RETHROW()

// The fixture owns its port until the client connects, so no other process can race the test onto a working
// endpoint; it answers that one connection with a reset and leaves any later one unanswered in its backlog.
BOOST_AUTO_TEST_CASE(probe_throws_when_unreachable) try {
   fc::test::connection_closing_http_server closing_server;
   es_client client{make_options(closing_server.url())};
   const auto started = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(client.probe(), fc::exception);
   // A second attempt could only end at the request timeout, so returning well inside it is the single attempt.
   BOOST_CHECK(std::chrono::steady_clock::now() - started < probe_attempt_budget);
}
FC_LOG_AND_RETHROW()

// cancel() is permanent, so a probe after it could never send a request: a connectivity check that cannot
// check is a programming error rather than an outcome to report.
BOOST_AUTO_TEST_CASE(probe_after_cancel_is_rejected) try {
   fc::test::capture_http_server server;
   es_client client{make_options(server.url())};
   client.cancel();
   BOOST_CHECK_THROW(client.probe(), fc::assert_exception);
   BOOST_CHECK_EQUAL(server.request_count(), 0u);
}
FC_LOG_AND_RETHROW()

// A probe cancel() aborts never judged the endpoint, so it must report the cancellation rather than call the
// endpoint unreachable.
BOOST_AUTO_TEST_CASE(cancel_during_probe_reports_canceled) try {
   fc::test::capture_http_server server{{delayed_ok(overlap_delay)}};
   auto options = make_options(server.url());
   options.request_timeout_ms = slow_timeout_ms; // the held request must outlive overlap_delay
   es_client client{options};

   std::string message;
   std::thread prober{[&] {
      try {
         client.probe();
      } catch (const fc::exception& e) {
         message = e.top_message();
      } catch (const std::exception& e) {
         message = e.what(); // a failure to run the coroutine surfaces as a std::exception
      } catch (...) {
         message = "<unknown exception>";
      }
   }};
   // The recorded request is the synchronization point: the probe is past the guard and awaiting a response
   // the stub is still holding back.
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   client.cancel();
   prober.join();
   BOOST_CHECK(message.find("was canceled") != std::string::npos);
   BOOST_CHECK(message.find("not reachable") == std::string::npos);
}
FC_LOG_AND_RETHROW()

// The single cancellation slot admits one request at a time whichever entry point asks for it: a probe while a
// bulk request is in flight is an FC_ASSERT, and the request in flight is untouched by it.
BOOST_AUTO_TEST_CASE(probe_overlapping_a_bulk_is_rejected) try {
   fc::test::capture_http_server server{{delayed_ok(overlap_delay)}};
   auto options = make_options(server.url());
   options.request_timeout_ms = slow_timeout_ms; // the held request must outlive overlap_delay
   es_client client{options};

   auto first = boost::asio::co_spawn(client.get_executor(), client.async_bulk(one_document_body(client), 1),
                                      boost::asio::use_future);
   // The recorded request is the synchronization point: the first coroutine is past the guard and awaiting a
   // response the stub is still holding back.
   BOOST_REQUIRE(server.wait_for_requests(1, delivery_wait));
   BOOST_CHECK_THROW(client.probe(), fc::assert_exception);

   const auto result = first.get();
   BOOST_CHECK(result.outcome == es_bulk_result::status::indexed);
   BOOST_CHECK_EQUAL(result.attempts, 1u);
   BOOST_CHECK_EQUAL(result.indexed_docs, 1u);
   // The rejected probe left the flag to its owner, which cleared it on completion: the next request is admitted.
   BOOST_CHECK(client.bulk(one_document_body(client), 1).outcome == es_bulk_result::status::indexed);
   BOOST_CHECK_EQUAL(server.request_count(), 2u);
}
FC_LOG_AND_RETHROW()

// The transport options reach the HTTP client: with a proxy set, the bulk request goes to the proxy with the
// endpoint's absolute URL as its target.
BOOST_AUTO_TEST_CASE(bulk_is_sent_through_the_configured_proxy) try {
   fc::test::capture_http_server proxy;
   fc::http::transport_options transport;
   transport.proxy = proxy.url();
   es_client client{make_options(std::string{unresolvable_origin}), std::move(transport)};
   BOOST_CHECK(client.bulk(one_document_body(client), 1).outcome == es_bulk_result::status::indexed);
   BOOST_REQUIRE(proxy.wait_for_requests(1, delivery_wait));
   BOOST_CHECK_EQUAL(proxy.request(0).target, std::string{unresolvable_origin} + "/_bulk");
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
