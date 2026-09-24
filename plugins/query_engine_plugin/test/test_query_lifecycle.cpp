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
   elapsed_ms = config.timeout.count() + 1;
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

/// Stopping the HTTP handler with requests waiting on chain reads completes each callback exactly
/// once with a cancellation, wakes the engine workers those requests block, and leaves the queued
/// chain callbacks to drain harmlessly afterwards.
BOOST_AUTO_TEST_CASE(http_handler_stop_completes_in_flight_requests_once) {
   read_queue reads;
   query_config config;
   config.worker_threads = 2;
   config.max_in_flight = 2;
   auto engine = std::make_shared<query_engine>(config, reads.create_api(*validating_node));
   auto handler = std::make_unique<query_http_handler>(engine);
   std::array<std::promise<fc::variant>, 2> completed;
   std::array<std::future<fc::variant>, 2> responses;
   std::array<std::atomic<size_t>, 2> calls{};
   std::array<std::atomic<int>, 2> statuses{};
   for (size_t i = 0; i < completed.size(); ++i) {
      responses[i] = completed[i].get_future();
      handler->submit(request_body(count_query), [&, i](int status, std::optional<fc::variant> response) {
         statuses[i] = status;
         if (++calls[i] == 1)
            completed[i].set_value(std::move(response.value()));
      });
   }
   await([&] { return reads.size() == completed.size(); });
   auto stopped = std::async(std::launch::async, [&] { handler->stop(); });
   BOOST_REQUIRE(stopped.wait_for(test_wait) == std::future_status::ready);
   stopped.get();
   for (auto& response : responses) {
      BOOST_REQUIRE(response.wait_for(test_wait) == std::future_status::ready);
      BOOST_CHECK_EQUAL(response.get()["error"]["data"]["kind"].as_string(), "QUERY_CANCELLED");
   }
   handler.reset();
   engine->stop();
   BOOST_CHECK_EQUAL(engine->stats().completed, completed.size());
   BOOST_CHECK_EQUAL(engine->stats().in_flight, completed.size());
   while (reads.run_one()) {}
   BOOST_CHECK_EQUAL(engine->stats().in_flight, 0);
   for (const auto& count : calls)
      BOOST_CHECK_EQUAL(count.load(), 1);
   for (const auto& status : statuses)
      BOOST_CHECK_EQUAL(status.load(), 200);
}

/// The read API bounds every chain read by the window time its scheduler reports. A window that has
/// already ended fails the attempt, which is queued again for the next window; once the request
/// deadline has passed instead, the query times out.
BOOST_AUTO_TEST_CASE(reads_cut_short_by_the_window_retry_until_the_deadline) {
   read_queue reads;
   std::atomic<size_t> windows{0};
   constexpr auto open_window = std::chrono::seconds(1);
   // The first window has already ended; every later one leaves plenty of time.
   query_engine engine({}, reads.create_api(*validating_node, [&] {
      return ++windows == 1 ? std::chrono::microseconds::zero() : std::chrono::microseconds(open_window);
   }));
   BOOST_CHECK_EQUAL(execute(engine, reads, count_query).rows.size(), 1);
   BOOST_CHECK_EQUAL(windows.load(), 3); // the expired ABI read, its retry, and the data capture
   engine.stop();

   read_queue expired_reads;
   std::atomic<int64_t> elapsed_ms{0};
   query_config config;
   query_engine expired(
      config, expired_reads.create_api(*validating_node, [] { return std::chrono::microseconds::zero(); }),
      [&] { return query_budget::clock::time_point{} + std::chrono::milliseconds(elapsed_ms.load()); });
   auto pending = std::async(std::launch::async, [&] { return expired.execute(count_query); });
   await([&] { return expired_reads.size() == 1; });
   BOOST_REQUIRE(expired_reads.run_one()); // the window has ended: the attempt is queued again
   await([&] { return expired_reads.size() == 1; });
   elapsed_ms = config.timeout.count() + 1; // the request deadline passes before the next window
   BOOST_REQUIRE(expired_reads.run_one());
   BOOST_REQUIRE(pending.wait_for(test_wait) == std::future_status::ready);
   // The deadline failure names the window, since the window and not the query's own budget cut it.
   BOOST_CHECK_EXCEPTION(pending.get(), query_error, [](const query_error& error) {
      return error.kind == error_kind::QUERY_TIMEOUT && error.limit == bound::read_window;
   });
   expired.stop();
   BOOST_CHECK_EQUAL(expired.stats().timed_out, 1);

   // Once the cut read's retry succeeds, a later deadline no longer names the window.
   read_queue cleared_reads;
   std::atomic<int64_t> cleared_ms{0};
   std::atomic<size_t> cleared_windows{0};
   query_engine cleared(
      config,
      cleared_reads.create_api(*validating_node,
                               [&] {
                                  return ++cleared_windows == 1 ? std::chrono::microseconds::zero()
                                                                : std::chrono::microseconds(open_window);
                               }),
      [&] { return query_budget::clock::time_point{} + std::chrono::milliseconds(cleared_ms.load()); });
   auto later = std::async(std::launch::async, [&] { return cleared.execute(count_query); });
   await([&] { return cleared_reads.size() == 1; });
   BOOST_REQUIRE(cleared_reads.run_one()); // the cut ABI read is queued again
   await([&] { return cleared_reads.size() == 1; });
   BOOST_REQUIRE(cleared_reads.run_one()); // its retry succeeds; the capture read follows
   await([&] { return cleared_reads.size() == 1; });
   cleared_ms = config.timeout.count() + 1;
   BOOST_REQUIRE(cleared_reads.run_one());
   BOOST_REQUIRE(later.wait_for(test_wait) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(later.get(), query_error, [](const query_error& error) {
      return error.kind == error_kind::QUERY_TIMEOUT && !error.limit;
   });
   cleared.stop();
   BOOST_CHECK_EQUAL(cleared.stats().read_window_cuts, 1);
}

/// A capture that overruns its own budget inside an open window fails without a retry: only the
/// window's end re-queues a read.
BOOST_AUTO_TEST_CASE(capture_budget_failures_are_not_retried) {
   constexpr uint32_t extra_rows = 600;
   seed(account, 4, extra_rows, "alice"_n, 1);
   produce_block();
   read_queue reads;
   std::atomic<int64_t> ticks{0};
   std::atomic<size_t> windows{0};
   constexpr auto tick = std::chrono::milliseconds(1);
   auto config = relaxed_config();
   // The ABI copy spends about ten ticks on its checks and charges and fits; the capture spends that
   // many before its second row and overruns long before the table is read.
   config.max_capture = 16 * tick;
   query_engine engine(config,
                       reads.create_api(*validating_node,
                                        [&] {
                                           ++windows;
                                           return std::chrono::microseconds(relaxed_deadline);
                                        }),
                       [&] { return query_budget::clock::time_point{} + tick * ticks++; });
   BOOST_CHECK_EXCEPTION(execute(engine, reads, "SELECT COUNT(*) AS n FROM sample.positions"), query_error,
                         [](const auto& error) {
                            return error.kind == error_kind::QUERY_TIMEOUT && error.limit == option::max_capture_ms;
                         });
   BOOST_CHECK_EQUAL(windows.load(), 2); // the ABI copy and one capture attempt
   engine.stop();
   BOOST_CHECK_EQUAL(engine.stats().read_window_cuts, 0);
}

/// A read the window cuts short after charging rows restores the budget before its retry: the row
/// and byte counters reflect only the attempt that completed, so a scan sized exactly to the table
/// still fits its limit.
BOOST_AUTO_TEST_CASE(retried_reads_restore_the_charges_of_the_cut_attempt) {
   constexpr uint32_t extra_rows = 600;
   constexpr uint64_t total_rows = extra_rows + 3;
   seed(account, 4, extra_rows, "alice"_n, 1);
   produce_block();
   // Every clock read advances one tick, so a window measured in ticks ends partway through the first
   // capture attempt: after rows were charged, well before the table was read.
   constexpr auto tick = std::chrono::milliseconds(1);
   constexpr auto short_window = 200 * tick;
   constexpr auto count_rows = "SELECT COUNT(*) AS n FROM sample.positions";
   // One run with the second read (the first capture attempt) cut, or none; returns the result and the
   // engine counters.
   const auto run = [&](uint64_t scan_limit, bool cut_first_capture) {
      read_queue reads;
      std::atomic<int64_t> ticks{0};
      std::atomic<size_t> windows{0};
      auto config = relaxed_config();
      config.max_scan_rows = scan_limit;
      query_engine engine(config,
                          reads.create_api(*validating_node,
                                           [&] {
                                              return cut_first_capture && ++windows == 2
                                                        ? std::chrono::microseconds(short_window)
                                                        : std::chrono::microseconds(relaxed_deadline);
                                           }),
                          [&] { return query_budget::clock::time_point{} + tick * ticks++; });
      query_result result;
      BOOST_REQUIRE_NO_THROW(result = execute(engine, reads, count_rows));
      if (cut_first_capture)
         BOOST_CHECK_EQUAL(windows.load(), 3); // the ABI copy, the cut capture and its retry
      engine.stop();
      return std::pair{result, engine.stats()};
   };
   const auto [reference, reference_stats] = run(defaults::max_scan_rows, false);
   BOOST_CHECK_EQUAL(reference_stats.read_window_cuts, 0);
   for (const auto scan_limit : {defaults::max_scan_rows, total_rows}) {
      const auto [result, stats] = run(scan_limit, true);
      BOOST_CHECK_EQUAL(result.rows.front()["n"].as_string(), std::to_string(total_rows));
      BOOST_CHECK_EQUAL(result.stats["scanned_rows"].as_string(), std::to_string(total_rows));
      BOOST_CHECK_EQUAL(result.stats["raw_bytes"].as_string(), reference.stats["raw_bytes"].as_string());
      BOOST_CHECK_EQUAL(stats.scanned_rows, total_rows);
      BOOST_CHECK_EQUAL(stats.capture_bytes, reference_stats.capture_bytes);
      BOOST_CHECK_EQUAL(stats.read_window_cuts, 1);
   }
}

/// Over HTTP the deadline watchdog and the engine race to fail a request; both name the read window
/// while a read it cut short is waiting for the next window. The race is run enough times that a
/// watchdog which dropped the label could not win every round.
BOOST_AUTO_TEST_CASE(http_timeout_after_a_window_cut_names_the_window) {
   constexpr size_t watchdog_races = 16;
   read_queue reads;
   std::atomic<int64_t> elapsed_ms{0};
   query_config config;
   auto engine = std::make_shared<query_engine>(
      config, reads.create_api(*validating_node, [] { return std::chrono::microseconds::zero(); }),
      [&] { return query_budget::clock::time_point{} + std::chrono::milliseconds(elapsed_ms.load()); });
   query_http_handler handler(engine);
   for (size_t round = 0; round < watchdog_races; ++round) {
      std::promise<fc::variant> completion;
      auto response = completion.get_future();
      std::atomic<size_t> calls{0};
      handler.submit(request_body(count_query), [&](int, std::optional<fc::variant> result) {
         if (++calls == 1)
            completion.set_value(std::move(result.value()));
      });
      await([&] { return reads.size() == 1; });
      BOOST_REQUIRE(reads.run_one()); // the window has ended: the attempt is queued again
      await([&] { return reads.size() == 1; });
      elapsed_ms += config.timeout.count() + 1;
      BOOST_REQUIRE(response.wait_for(test_wait) == std::future_status::ready);
      const auto error = response.get()["error"];
      BOOST_CHECK_EQUAL(error["data"]["kind"].as_string(), "QUERY_TIMEOUT");
      BOOST_CHECK_EQUAL(error["data"]["limit"].as_string(), bound::read_window);
      BOOST_REQUIRE(reads.run_one()); // the queued retry fails its deadline check and releases admission
      await([&] { return engine->stats().in_flight == 0; });
      BOOST_CHECK_EQUAL(calls.load(), 1);
   }
   handler.stop();
   engine->stop();
   BOOST_CHECK_EQUAL(engine->stats().timed_out, watchdog_races);
}

BOOST_AUTO_TEST_CASE(queued_deadline_retains_admission_until_callback_drains) {
   read_queue reads;
   std::atomic<int64_t> elapsed_ms{0};
   query_config config;
   config.max_in_flight = 1;
   query_engine engine(config, reads.create_api(*validating_node), [&] {
      return query_budget::clock::time_point{} + std::chrono::milliseconds(elapsed_ms.load());
   });
   auto first = std::async(std::launch::async,
                           [&] { return engine.execute(count_query, query_options{.timeout = config.timeout}); });
   await([&] { return reads.size() == 1; });
   BOOST_CHECK_EXCEPTION(engine.execute(count_query), query_error,
                         [](const auto& error) { return error.kind == error_kind::QUERY_BUSY; });
   elapsed_ms = config.timeout.count() + 1;
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
