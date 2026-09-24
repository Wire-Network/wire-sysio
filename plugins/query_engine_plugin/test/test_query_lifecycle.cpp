#include "query_fixture.hpp"

#include <atomic>
#include <thread>

using namespace sysio::query_engine;
using namespace sysio::query_engine::test;

namespace {
constexpr auto count_query = "SELECT COUNT(*) AS n FROM sample.wide";

/// Bound harness synchronization without advancing the injected query clock.
template <typename Predicate>
void await(Predicate predicate) {
   const auto deadline = query_budget::clock::now() + test_wait;
   while (!predicate()) {
      BOOST_REQUIRE(query_budget::clock::now() < deadline);
      std::this_thread::yield();
   }
}

/// Errors cross the synchronous public API as their original typed exceptions.
void check_error(std::future<query_result>& future, error_kind kind) {
   BOOST_REQUIRE(future.wait_for(test_wait) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(future.get(), query_error, [&](const query_error& error) { return error.kind == kind; });
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(query_lifecycle, chain_fixture)

/// HTTP queue deadlines and busy responses remain live while its only worker waits on execute().
BOOST_AUTO_TEST_CASE(http_queue_deadlines_and_shutdown_do_not_need_query_workers) {
   read_queue reads;
   std::atomic<int64_t> elapsed_ms{0};
   query_config config;
   config.worker_threads = 1;
   config.max_in_flight = 2;
   auto engine = std::make_shared<query_engine>(config, reads.create_api(*validating_node), [&] {
      return query_budget::clock::time_point{} + std::chrono::milliseconds(elapsed_ms.load());
   });
   query_http_handler handler(engine);
   std::array<std::promise<fc::variant>, 3> completed;
   std::array<std::future<fc::variant>, 3> responses;
   std::array<std::atomic<size_t>, 3> calls{};
   for (size_t i = 0; i < completed.size(); ++i) {
      responses[i] = completed[i].get_future();
      handler.submit(request_body(count_query), [&, i](int, std::optional<fc::variant> response) {
         if (++calls[i] == 1)
            completed[i].set_value(std::move(response.value()));
      });
      if (i == 0)
         await([&] { return reads.size() == 1; });
   }
   BOOST_REQUIRE(responses[2].wait_for(test_wait) == std::future_status::ready);
   BOOST_CHECK_EQUAL(responses[2].get()["error"]["data"]["kind"].as_string(), "QUERY_BUSY");
   elapsed_ms = config.timeout_ms + 1;
   for (size_t i = 0; i < 2; ++i) {
      BOOST_REQUIRE(responses[i].wait_for(test_wait) == std::future_status::ready);
      BOOST_CHECK_EQUAL(responses[i].get()["error"]["data"]["kind"].as_string(), "QUERY_TIMEOUT");
   }
   handler.stop();
   engine->stop();
   BOOST_CHECK_EQUAL(engine->stats().accepted, 1);
   BOOST_CHECK_EQUAL(engine->stats().in_flight, 1);
   BOOST_REQUIRE(reads.run_one());
   BOOST_CHECK_EQUAL(engine->stats().in_flight, 0);
   for (const auto& count : calls)
      BOOST_CHECK_EQUAL(count.load(), 1);
}

BOOST_AUTO_TEST_CASE(queued_deadline_retains_admission_until_callback_drains) {
   read_queue reads;
   std::atomic<int64_t> elapsed_ms{0};
   query_config config;
   config.max_in_flight = 1;
   query_engine engine(config, reads.create_api(*validating_node), [&] {
      return query_budget::clock::time_point{} + std::chrono::milliseconds(elapsed_ms.load());
   });
   auto first = std::async(std::launch::async, [&] {
      return engine.execute(count_query, query_options{.timeout = std::chrono::milliseconds(config.timeout_ms)});
   });
   await([&] { return reads.size() == 1; });
   BOOST_CHECK_EXCEPTION(engine.execute(count_query), query_error,
                         [](const auto& error) { return error.kind == error_kind::QUERY_BUSY; });
   elapsed_ms = config.timeout_ms + 1;
   check_error(first, error_kind::QUERY_TIMEOUT);
   BOOST_CHECK_EQUAL(engine.stats().in_flight, 1);
   BOOST_CHECK_EXCEPTION(engine.execute(count_query), query_error,
                         [](const auto& error) { return error.kind == error_kind::QUERY_BUSY; });
   BOOST_REQUIRE(reads.run_one());
   await([&] { return engine.stats().in_flight == 0; });
   const auto success = execute(engine, reads, count_query);
   BOOST_CHECK_EQUAL(success.rows.size(), 1);
   engine.stop();
   BOOST_CHECK_EQUAL(engine.stats().timed_out, 1);
   BOOST_CHECK_EQUAL(engine.stats().completed, 2);
}

BOOST_AUTO_TEST_CASE(shutdown_with_pending_chain_callbacks_never_waits_for_app_thread) {
   for (auto stage : {query_stage::describe, query_stage::capture}) {
      read_queue reads;
      query_engine engine({}, reads.create_api(*validating_node));
      auto result = std::async(std::launch::async, [&] { return engine.execute(count_query); });
      await([&] { return reads.size() == 1; });
      if (stage == query_stage::capture) {
         BOOST_REQUIRE(reads.run_one());
         await([&] { return reads.size() == 1; });
      }
      engine.stop();
      check_error(result, error_kind::QUERY_CANCELLED);
      BOOST_CHECK_EQUAL(engine.stats().in_flight, 1);
      BOOST_REQUIRE(reads.run_one());
      BOOST_CHECK_EQUAL(engine.stats().in_flight, 0);
      BOOST_CHECK_EQUAL(engine.stats().completed, 1);
   }
}

BOOST_AUTO_TEST_CASE(shutdown_at_each_worker_stage_completes_once) {
   for (auto stage : {query_stage::parse, query_stage::plan, query_stage::evaluate}) {
      read_queue reads;
      std::promise<void> release;
      const auto barrier = release.get_future().share();
      std::atomic<bool> reached{false};
      query_engine engine({}, reads.create_api(*validating_node), query_budget::clock::now, [&](auto current) {
         if (current == stage) {
            reached = true;
            barrier.wait();
         }
      });
      auto result = std::async(std::launch::async, [&] { return engine.execute(count_query); });
      const auto deadline = query_budget::clock::now() + test_wait;
      while (!reached) {
         if (query_budget::clock::now() >= deadline) {
            release.set_value();
            engine.stop();
            BOOST_FAIL("Worker stage was not reached");
         }
         reads.run_one();
      }
      auto stopped = std::async(std::launch::async, [&] { engine.stop(); });
      const auto completed = result.wait_for(test_wait);
      release.set_value();
      BOOST_REQUIRE(completed == std::future_status::ready);
      BOOST_REQUIRE(stopped.wait_for(test_wait) == std::future_status::ready);
      stopped.get();
      check_error(result, error_kind::QUERY_CANCELLED);
      BOOST_CHECK_EQUAL(engine.stats().in_flight, 0);
      BOOST_CHECK_EQUAL(engine.stats().completed, 1);
   }
}

BOOST_AUTO_TEST_CASE(notifications_have_no_payload_even_on_invocation_errors) {
   read_queue reads;
   auto engine = std::make_shared<query_engine>(query_config{}, reads.create_api(*validating_node));
   query_http_handler handler(engine);
   for (const auto* body : {"{\"jsonrpc\":\"2.0\",\"method\":\"missing\"}",
                            "{\"jsonrpc\":\"2.0\",\"method\":\"query.execute\",\"params\":{}}"}) {
      std::promise<std::pair<int, std::optional<fc::variant>>> completion;
      auto result = completion.get_future();
      std::atomic<size_t> calls{0};
      handler.submit(body, [&](int status, std::optional<fc::variant> response) {
         if (++calls == 1)
            completion.set_value({status, std::move(response)});
      });
      BOOST_REQUIRE(result.wait_for(test_wait) == std::future_status::ready);
      const auto [status, response] = result.get();
      BOOST_CHECK_EQUAL(status, 204);
      BOOST_CHECK(!response);
      BOOST_CHECK_EQUAL(calls.load(), 1);
   }
}

/// Only the adapter enforces JSON envelope size; engine counters count completed C++ results.
BOOST_AUTO_TEST_CASE(response_byte_limit_has_no_partial_result) {
   read_queue reads;
   query_config config;
   config.max_response_bytes = 1;
   auto engine = std::make_shared<query_engine>(config, reads.create_api(*validating_node));
   query_http_handler handler(engine);
   const auto response = submit(handler, reads, count_query);
   BOOST_CHECK(!response.get_object().contains("result"));
   BOOST_CHECK_EQUAL(response["error"]["data"]["kind"].as_string(), "QUERY_LIMIT");
   BOOST_CHECK_EQUAL(response["error"]["data"]["limit"].as_string(), option::max_response_bytes);
   engine->stop();
   BOOST_CHECK_EQUAL(engine->stats().returned_rows, 1);
   BOOST_CHECK_EQUAL(engine->stats().in_flight, 0);
}
BOOST_AUTO_TEST_SUITE_END()
