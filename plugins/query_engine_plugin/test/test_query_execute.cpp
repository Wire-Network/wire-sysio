#include "query_fixture.hpp"

#include <limits>
#include <type_traits>

using namespace sysio::query_engine;
using namespace sysio::query_engine::test;

namespace {
constexpr auto ordered_sql = "SELECT key.id AS id, amount FROM sample.positions ORDER BY amount DESC";
constexpr auto grouped_sql = "SELECT beneficiary, SUM(amount) AS total FROM sample.positions "
                             "GROUP BY beneficiary HAVING SUM(amount) > 0 ORDER BY total DESC";
} // namespace

BOOST_FIXTURE_TEST_SUITE(query_execute, chain_fixture)

/// The public service returns owned typed rows and the same metadata serialized by HTTP.
BOOST_AUTO_TEST_CASE(typed_rows_metadata_and_worker_execution) {
   read_queue reads;
   const auto caller = std::this_thread::get_id();
   std::atomic<size_t> worker_stages{0}, caller_stages{0};
   query_engine engine({}, reads.create_api(*validating_node), query_budget::clock::now, [&](query_stage) {
      ++worker_stages;
      caller_stages += std::this_thread::get_id() == caller;
   });
   auto result = execute(engine, reads, ordered_sql);
   static_assert(std::is_same_v<decltype(result.rows), std::vector<fc::variant_object>>);
   BOOST_REQUIRE_EQUAL(result.rows.size(), 3);
   BOOST_CHECK_EQUAL(result.rows.front()["id"].as_string(), "2");
   BOOST_CHECK_EQUAL(result.schema_version, constants::schema_version);
   BOOST_CHECK(result.complete);
   BOOST_CHECK_EQUAL(result.columns.size(), 2);
   BOOST_CHECK_EQUAL(result.source["table"].as_string(), "positions");
   BOOST_CHECK_EQUAL(result.state["block_id"].as_string(), validating_node->head().id().str());
   BOOST_CHECK_EQUAL(result.stats["scanned_rows"].as_string(), "3");
   BOOST_CHECK_EQUAL(worker_stages.load(), 5);
   BOOST_CHECK_EQUAL(caller_stages.load(), 0);
   engine.stop();
   put(2, position(999));
   BOOST_CHECK_EQUAL(result.rows.front()["amount"].as_string(), "90");
}

/// Pagination is applied to completed, ordered groups and composes with SQL LIMIT by taking the smaller limit.
BOOST_AUTO_TEST_CASE(limit_offset_and_aggregate_semantics) {
   read_queue reads;
   query_engine engine({}, reads.create_api(*validating_node));
   auto result = execute(engine, reads, ordered_sql, query_options{.limit = 1, .offset = 1});
   BOOST_REQUIRE_EQUAL(result.rows.size(), 1);
   BOOST_CHECK_EQUAL(result.rows[0]["id"].as_string(), "1");
   result = execute(engine, reads, grouped_sql, query_options{.limit = 1});
   BOOST_CHECK_EQUAL(result.rows[0]["total"].as_string(), "150");
   result = execute(engine, reads, grouped_sql, query_options{.limit = 1, .offset = 1});
   BOOST_CHECK_EQUAL(result.rows[0]["total"].as_string(), "20");
   BOOST_CHECK_EQUAL(result.stats["groups"].as_string(), "2");
   result = execute(engine, reads, std::string(ordered_sql) + " LIMIT 1", query_options{.limit = 2, .offset = 1});
   BOOST_REQUIRE_EQUAL(result.rows.size(), 1);
   BOOST_CHECK_EQUAL(result.rows[0]["id"].as_string(), "1");
   BOOST_CHECK(execute(engine, reads, ordered_sql, query_options{.limit = 0}).rows.empty());
   BOOST_CHECK(execute(engine, reads, ordered_sql, query_options{.offset = 3}).rows.empty());
   BOOST_CHECK(
      execute(engine, reads, ordered_sql, query_options{.offset = std::numeric_limits<uint64_t>::max()}).rows.empty());
   BOOST_CHECK_EQUAL(
      execute(engine, reads, ordered_sql, query_options{.limit = std::numeric_limits<uint64_t>::max()}).rows.size(), 3);
}

/// Absent and default-constructed options apply the configured deadline; only no_deadline opts out.
BOOST_AUTO_TEST_CASE(default_timeout_is_configured_and_no_deadline_is_explicit) {
   for (const auto& options : {std::optional<query_options>{}, std::optional<query_options>{query_options{}},
                               std::optional<query_options>{query_options{.timeout = constants::no_deadline}}}) {
      read_queue reads;
      query_config config;
      std::atomic<int64_t> elapsed_ms{0};
      query_engine engine(
         config, reads.create_api(*validating_node),
         [&] { return query_budget::clock::time_point{} + std::chrono::milliseconds(elapsed_ms.load()); },
         [&](query_stage stage) {
            if (stage == query_stage::parse)
               elapsed_ms = config.timeout.count() + 1;
         });
      if (options && options->timeout == constants::no_deadline)
         BOOST_CHECK_EQUAL(execute(engine, reads, ordered_sql, options).rows.size(), 3);
      else
         BOOST_CHECK_EXCEPTION(execute(engine, reads, ordered_sql, options), query_error,
                               [](const auto& error) { return error.kind == error_kind::QUERY_TIMEOUT; });
   }
}

/// Invalid options, typed SQL failures and configured resource caps propagate without an RPC envelope.
BOOST_AUTO_TEST_CASE(option_errors_and_resource_limits) {
   read_queue reads;
   query_config config;
   config.max_result_rows = 1;
   query_engine engine(config, reads.create_api(*validating_node));
   for (const auto timeout : {std::chrono::milliseconds(-2), constants::no_deadline - std::chrono::milliseconds(1)})
      BOOST_CHECK_EXCEPTION(execute(engine, reads, ordered_sql, query_options{.timeout = timeout}), query_error,
                            [](const auto& error) { return error.kind == error_kind::INVALID_PARAMS; });
   BOOST_CHECK_EXCEPTION(execute(engine, reads, ordered_sql, query_options{.timeout = std::chrono::milliseconds(0)}),
                         query_error, [](const auto& error) { return error.kind == error_kind::QUERY_TIMEOUT; });
   BOOST_CHECK_EXCEPTION(execute(engine, reads, ordered_sql), query_error, [](const auto& error) {
      return error.kind == error_kind::QUERY_LIMIT && error.limit == option::max_result_rows;
   });
   BOOST_CHECK_EQUAL(execute(engine, reads, ordered_sql, query_options{.limit = 1}).rows.size(), 1);
   BOOST_CHECK_EXCEPTION(execute(engine, reads, "SELECT"), query_error,
                         [](const auto& error) { return error.kind == error_kind::QUERY_SYNTAX; });
   BOOST_CHECK_EXCEPTION(execute(engine, reads, "SELECT missing FROM sample.positions"), query_error,
                         [](const auto& error) { return error.kind == error_kind::QUERY_SEMANTICS; });
   engine.stop();
   BOOST_CHECK_EXCEPTION(engine.execute(ordered_sql), query_error,
                         [](const auto& error) { return error.kind == error_kind::QUERY_CANCELLED; });
}

/// A caller-owned budget is required and serves exactly one call; it may be inspected afterwards.
BOOST_AUTO_TEST_CASE(caller_owned_budgets_serve_one_call) {
   read_queue reads;
   query_engine engine({}, reads.create_api(*validating_node));
   BOOST_CHECK_EXCEPTION(engine.execute(ordered_sql, std::shared_ptr<query_budget>{}), query_error,
                         [](const auto& error) { return error.kind == error_kind::INVALID_PARAMS; });
   const auto budget = engine.create_budget();
   auto first = std::async(std::launch::async, [&] { return engine.execute(ordered_sql, budget); });
   const auto deadline = query_budget::clock::now() + test_wait;
   while (first.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      BOOST_REQUIRE(query_budget::clock::now() < deadline);
      reads.run_one();
   }
   BOOST_CHECK_EQUAL(first.get().rows.size(), 3);
   BOOST_CHECK_GT(budget->peak_accounted_bytes, 0);
   BOOST_CHECK_EQUAL(budget->scanned_rows, 3);
   BOOST_CHECK_EXCEPTION(engine.execute(ordered_sql, budget), query_error,
                         [](const auto& error) { return error.kind == error_kind::INVALID_PARAMS; });
   engine.stop();
}

/// Reject accidental application-thread waits before creating a query or scheduling a read.
BOOST_AUTO_TEST_CASE(application_thread_cannot_block_its_own_read_dispatch) {
   read_queue reads;
   auto api = std::make_shared<query_read_api>(
      std::make_shared<local_table_source>(*validating_node), [&](auto callback) { reads.post(std::move(callback)); },
      std::this_thread::get_id());
   query_engine engine({}, api);
   query_service& service = engine;
   BOOST_CHECK_EXCEPTION(service.execute(ordered_sql), query_error,
                         [](const auto& error) { return error.kind == error_kind::INVALID_PARAMS; });
   BOOST_CHECK_EQUAL(engine.stats().accepted, 0);
   BOOST_CHECK_EQUAL(reads.size(), 0);
   BOOST_CHECK_EQUAL(execute(engine, reads, ordered_sql).rows.size(), 3);
   engine.stop();
   BOOST_CHECK_EXCEPTION(service.execute(ordered_sql), query_error,
                         [](const auto& error) { return error.kind == error_kind::QUERY_CANCELLED; });
}
BOOST_AUTO_TEST_SUITE_END()
