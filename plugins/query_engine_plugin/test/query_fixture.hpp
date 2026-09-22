#pragma once
#include "query_schema.hpp"

#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>
#include <sysio/query_engine_plugin/query_http_handler.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <test_contracts.hpp>

namespace sysio::query_engine::test {
using namespace sysio::chain::literals;
inline constexpr auto account = "sample"_n;
inline constexpr auto other_account = "other"_n;
inline constexpr auto put_action = "put"_n;
inline constexpr auto seed_action = "seed"_n;
inline constexpr auto erase_action = "erase"_n;
inline constexpr auto composite_action = "putcomp"_n;
inline constexpr auto wide_action = "putwide"_n;
inline constexpr auto test_wait = std::chrono::seconds(10);

/// Real signed fixture actions, replicated into the validating controller used by queries.
struct chain_fixture : testing::validating_tester {
   chain_fixture() {
      create_accounts({account, other_account});
      for (const auto code : {account, other_account}) {
         set_code(code, testing::test_contracts::query_fixture_wasm());
         set_abi(code, testing::test_contracts::query_fixture_abi().c_str());
      }
      produce_block();
      seed(account, 1, 1, "alice"_n, 60);
      seed(account, 2, 1, "alice"_n, 90);
      seed(account, 3, 1, "bob"_n, 20);
      produce_block();
   }
   /// Apply a bounded batch of rows through WASM, then seal the state for readers.
   void seed(chain::name code, uint64_t first, uint32_t count, chain::name beneficiary, int64_t amount) {
      push_action(
         code, seed_action, code,
         fc::mutable_variant_object()("first", first)("count", count)("beneficiary", beneficiary)("amount", amount));
   }
   /// Shared action input uses the fixture ABI, preserving nested and optional values.
   static fc::variant position(int64_t amount, fc::variant nullable = {}, const std::string& quantity = "1.2500 SYS") {
      return fc::mutable_variant_object()("beneficiary", "alice")("amount", amount)("nullable", std::move(nullable))(
         "quantity", quantity)("created", "2023-11-14T22:13:20.123")("memo", "a'b")(
         "nested", fc::mutable_variant_object()("score", -7)(
                      "numbers", fc::variants{fc::variant(uint64_t{9007199254740993ULL})}))("enabled", true);
   }
   /// Replace one row with an ABI-encoded action.
   void put(uint64_t id, fc::variant row) {
      push_action(account, put_action, account, fc::mutable_variant_object()("id", id)("row", row));
      produce_block();
   }
   /// Capture/evaluate directly for fine-grained ownership, range and counter assertions.
   fc::variant evaluate_query(const std::string& sql, query_budget& budget, uint64_t* pages = nullptr) {
      local_table_source source(*validating_node);
      auto ast = parse_query(sql, budget);
      auto plan = create_plan(ast, source.describe(ast, budget), budget);
      auto input = source.capture(plan, budget);
      if (pages)
         *pages = input.native_pages;
      return fc::variant(evaluate(plan, std::move(input), budget));
   }
};

/// Deterministic read-callback pump: controller callbacks run only on the fixture's owning thread.
struct read_queue {
   std::mutex mutex;
   std::condition_variable available;
   std::deque<std::function<void()>> callbacks;
   void post(std::function<void()> callback) {
      {
         std::lock_guard lock(mutex);
         callbacks.push_back(std::move(callback));
      }
      available.notify_one();
   }
   bool run_one() {
      std::function<void()> callback;
      {
         std::unique_lock lock(mutex);
         if (!available.wait_for(lock, std::chrono::milliseconds(10), [&] { return !callbacks.empty(); }))
            return false;
         callback = std::move(callbacks.front());
         callbacks.pop_front();
      }
      callback();
      return true;
   }
   /// Synchronization remains inside the production read API; the test thread supplies its executor.
   std::shared_ptr<query_read_api> create_api(const chain::controller& controller) {
      return std::make_shared<query_read_api>(std::make_shared<local_table_source>(controller),
                                              [this](auto callback) { post(std::move(callback)); });
   }
   size_t size() {
      std::lock_guard lock(mutex);
      return callbacks.size();
   }
};

/// Production JSON-RPC envelope without a second route implementation.
inline std::string request_body(const std::string& sql) {
   return fc::json::to_string(fc::mutable_variant_object()("jsonrpc", constants::version)("id", "test")(
                                 "method", constants::method)("params", fc::mutable_variant_object()("query", sql)),
                              fc::time_point::maximum());
}

/// Run the production HTTP handler through its worker queues, with a finite test harness deadline.
inline fc::variant submit(query_http_handler& handler, read_queue& reads, const std::string& sql) {
   std::promise<fc::variant> completion;
   auto future = completion.get_future();
   handler.submit(request_body(sql), [&](int status, std::optional<fc::variant> result) {
      BOOST_CHECK_EQUAL(status, 200);
      completion.set_value(result.value());
   });
   const auto deadline = query_budget::clock::now() + test_wait;
   while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      BOOST_REQUIRE(query_budget::clock::now() < deadline);
      reads.run_one();
   }
   auto response = future.get();
   validate_response(fc::json::to_string(response, fc::time_point::maximum()));
   return response;
}
/// Exercise the public blocking API from a caller worker while the test thread drives safe reads.
inline query_result execute(query_engine& engine, read_queue& reads, const std::string& sql,
                            const std::optional<query_options>& options = std::nullopt) {
   query_service& service = engine;
   auto future = std::async(std::launch::async, [&] { return service.execute(sql, options); });
   const auto deadline = query_budget::clock::now() + test_wait;
   while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      if (query_budget::clock::now() >= deadline) {
         // Bound the failure path too: stopping wakes workers even if the test cannot pump reads.
         engine.stop();
         throw std::runtime_error("Query test exceeded its completion deadline");
      }
      reads.run_one();
   }
   return future.get();
}
} // namespace sysio::query_engine::test
