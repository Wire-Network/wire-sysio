#include "query_fixture.hpp"
#include "query_schema.hpp"

#include <sysio/chain/app.hpp>
#include <sysio/chain_api_plugin/chain_api_plugin.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/query_engine_plugin/query_engine_plugin.hpp>

#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>

using namespace sysio;
using namespace sysio::query_engine;
using namespace sysio::query_engine::test;
namespace http = boost::beast::http;
namespace {
constexpr auto socket_name = "query.sock";
constexpr auto genesis_name = "genesis.json";
constexpr auto all_rows_sql = "SELECT beneficiary, COUNT(*) AS records, SUM(amount) AS total FROM positions OWNER "
                              "'sample' GROUP BY beneficiary ORDER BY total DESC";
constexpr auto selected_sql = "SELECT * FROM sample.positions WHERE key.id = 1";
constexpr auto table_route = "/v1/chain/get_table_rows";
constexpr int http_version = 11;
constexpr uint32_t default_read_threads = 2;
constexpr auto write_window_option = "read-only-write-window-time-us";
/// Real-clock request deadlines include read-window waits; a generous default keeps runs on a loaded
/// host deterministic unless a test pins its own timeout.
constexpr auto default_query_timeout = std::chrono::seconds(10);

/// Actual application and dependency closure, with an explicitly disabled TCP listener.
struct http_application {
   fc::temp_directory directory;
   chain::application application{
      {.enable_logging_config = false,
       .enable_resource_monitor = false,
       .sighup_loads_logging_config = false,
       .log_on_exit = false}
   };
   std::thread loop;
   std::promise<chain::exit_code::exit_code> exit;
   std::future<chain::exit_code::exit_code> exited = exit.get_future();
   std::filesystem::path socket = directory.path() / socket_name;
   bool initialized = false;
   bool http_enabled = false;
   bool has_listener = false;
   std::atomic<uint64_t> query_requests{0}, denied_table_requests{0};

   /// Enablement is supplied via the public --plugin option, never a test route.
   chain::exit_code::exit_code initialize(bool enabled, uint32_t read_threads = default_read_threads,
                                          const std::vector<std::string>& extra = {},
                                          const std::string& read_mode = "head", bool listener = true,
                                          bool enable_http = true) {
      appbase::application_base::register_plugin<sysio::query_engine_plugin>();
      appbase::application_base::register_plugin<http_plugin>();
      appbase::application_base::register_plugin<chain_api_plugin>();
      http_enabled = enable_http;
      has_listener = listener;
      const auto genesis = directory.path() / genesis_name;
      std::ofstream(genesis) << fc::json::to_string(testing::base_tester::default_genesis(), fc::time_point::maximum());
      std::vector<std::string> args{"query-http-test",
                                    "--data-dir",
                                    directory.path().string(),
                                    "--config-dir",
                                    directory.path().string(),
                                    "--genesis-json",
                                    genesis.string(),
                                    "--http-server-address",
                                    "",
                                    "--unix-socket-path",
                                    listener ? socket.string() : "",
                                    "--read-mode",
                                    read_mode,
                                    "--read-only-threads",
                                    std::to_string(read_threads),
                                    "--chain-state-db-size-mb",
                                    "64",
                                    "--chain-state-db-guard-size-mb",
                                    "0",
                                    "--sys-vm-oc-enable",
                                    "none"};
      if (enabled)
         args.insert(args.end(), {"--plugin", "sysio::query_engine_plugin"});
      if (enable_http)
         args.insert(args.end(), {"--plugin", "sysio::chain_api_plugin", "--plugin", "sysio::http_plugin"});
      args.insert(args.end(), extra.begin(), extra.end());
      const auto timeout_flag = flag(option::timeout_ms);
      if (std::find(extra.begin(), extra.end(), timeout_flag) == extra.end())
         args.insert(args.end(),
                     {timeout_flag, std::to_string(std::chrono::milliseconds(default_query_timeout).count())});
      std::vector<char*> argv;
      for (auto& argument : args)
         argv.push_back(argument.data());
      const auto result = application.init<producer_plugin>(argv.size(), argv.data());
      initialized = result == chain::exit_code::SUCCESS;
      return result;
   }

   /// Startup and shutdown both run through the real application executor.
   void start() {
      BOOST_REQUIRE(initialized);
      // The ingress observer also denies the old table route: a hidden HTTP read cannot succeed.
      if (http_enabled)
         app().get_plugin<http_plugin>().register_update_metrics([this](http_plugin::metrics request) {
            if (request.target == constants::endpoint)
               ++query_requests;
            if (request.target == table_route) {
               ++denied_table_requests;
               throw std::runtime_error("Table HTTP route denied by query integration test");
            }
         });
      loop = std::thread([this] { exit.set_value(application.exec()); });
      const auto deadline = query_budget::clock::now() + test_wait;
      while (http_enabled && has_listener && !app().get_plugin<http_plugin>().listening()) {
         BOOST_REQUIRE(exited.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
         BOOST_REQUIRE(query_budget::clock::now() < deadline);
         std::this_thread::yield();
      }
      write([] {}); // Also wait for startup of plugins ordered after the HTTP listener.
   }

   /// Run the application until its startup guards reject the configuration, and return the exit code.
   chain::exit_code::exit_code run_until_startup_fails() {
      BOOST_REQUIRE(initialized);
      loop = std::thread([this] { exit.set_value(application.exec()); });
      const auto finished = exited.wait_for(test_wait);
      if (finished != std::future_status::ready)
         app().quit();
      BOOST_REQUIRE(finished == std::future_status::ready);
      return exited.get();
   }

   /// Queue actual state mutations on the same read/write executor used by nodeop.
   std::future<void> post_write(std::function<void()> function) {
      auto completion = std::make_shared<std::promise<void>>();
      auto result = completion->get_future();
      app().executor().post(priority::medium, exec_queue::read_write, [function = std::move(function), completion] {
         try {
            function();
            completion->set_value();
         } catch (...) {
            completion->set_exception(std::current_exception());
         }
      });
      return result;
   }
   void write(std::function<void()> function) {
      auto result = post_write(std::move(function));
      BOOST_REQUIRE(result.wait_for(test_wait) == std::future_status::ready);
      result.get();
   }

   /// Captures use the real read-exclusive queue, exactly as the plugin schedules them: only the
   /// producer's read workers drain it, inside a read window the app thread cannot enter.
   void post_read(std::function<void()> function) {
      app().executor().post(priority::medium_low, exec_queue::read_exclusive, std::move(function));
   }

   /// Copy ABI bytes on the read executor; the description is resolved outside its callbacks.
   std::vector<table_schema> describe(const ast_query& ast, query_budget& budget) {
      auto completion = std::make_shared<std::promise<std::vector<table_schema>>>();
      auto result = completion->get_future();
      post_read([&ast, &budget, completion] {
         try {
            local_table_source source(app().get_plugin<chain_plugin>().chain());
            completion->set_value(source.capture_abis(ast, budget));
         } catch (...) {
            completion->set_exception(std::current_exception());
         }
      });
      BOOST_REQUIRE(result.wait_for(test_wait) == std::future_status::ready);
      auto schemas = result.get();
      for (auto& schema : schemas)
         resolve_schema(schema, ast.table, budget);
      return schemas;
   }

   /// Copy signed blocks from the real WASM fixture and apply them as scheduled writes.
   std::future<void> post_sync(chain_fixture& fixture) {
      std::vector<chain::signed_block_ptr> blocks;
      for (uint32_t i = 1; i <= fixture.control->head().block_num(); ++i)
         blocks.push_back(fixture.control->fetch_block_by_number(i));
      return post_write([blocks = std::move(blocks)] {
         auto& control = app().get_plugin<chain_plugin>().chain();
         for (const auto& block : blocks) {
            if (block->block_num() <= control.head().block_num())
               continue;
            control.accept_block(block->calculate_id(), block);
            app().get_plugin<producer_plugin>().on_incoming_block();
         }
      });
   }

   /// Await scheduled blocks without accessing the controller from the test thread.
   void sync(chain_fixture& fixture) {
      auto result = post_sync(fixture);
      BOOST_REQUIRE(result.wait_for(test_wait) == std::future_status::ready);
      result.get();
   }

   /// Every response from the production route is validated in its actual wire encoding.
   http::response<http::string_body> request(std::string body, const char* route = constants::endpoint) {
      boost::asio::io_context io;
      boost::beast::basic_stream<boost::asio::local::stream_protocol> stream(io);
      http::request<http::string_body> request{http::verb::post, route, http_version};
      request.set(http::field::host, "localhost");
      request.set(http::field::content_type, "application/json");
      request.body() = std::move(body);
      request.prepare_payload();
      boost::beast::flat_buffer buffer;
      http::response<http::string_body> response;
      boost::system::error_code failure;
      stream.expires_after(test_wait);
      stream.async_connect(boost::asio::local::stream_protocol::endpoint(socket.string()),
                           [&](boost::system::error_code error) {
                              if (error) {
                                 failure = error;
                                 return;
                              }
                              http::async_write(stream, request, [&](boost::system::error_code error, size_t) {
                                 if (error) {
                                    failure = error;
                                    return;
                                 }
                                 http::async_read(stream, buffer, response,
                                                  [&](boost::system::error_code error, size_t) { failure = error; });
                              });
                           });
      io.run();
      if (failure)
         throw boost::system::system_error(failure);
      if (response.result() == http::status::ok && route == std::string(constants::endpoint))
         validate_response(response.body());
      if (response.result() == http::status::no_content)
         BOOST_CHECK(response.body().empty());
      return response;
   }
   /// Shutdown is callable explicitly so retained C++ service handles can be tested.
   void stop() {
      if (loop.joinable()) {
         app().quit();
         loop.join();
      }
   }
   ~http_application() { stop(); }
};
} // namespace

/// Opt-in registration, real controller access, IDs, errors, notifications and schema on the wire.
BOOST_AUTO_TEST_CASE(query_http_plugin_route) {
   chain_fixture fixture;
   http_application server;
   BOOST_REQUIRE(server.initialize(true) == chain::exit_code::SUCCESS);
   server.start();
   server.sync(fixture);
   BOOST_CHECK(server.request("{}", table_route).result() == http::status::internal_server_error);
   const auto response = server.request(request_body(all_rows_sql));
   BOOST_REQUIRE(response.result() == http::status::ok);
   const auto result = fc::json::from_string(response.body())["result"];
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["total"].as_string(), "150");
   BOOST_CHECK_EQUAL(result["state"]["block_id"].as_string(), fixture.control->head().id().str());
   BOOST_CHECK_EQUAL(result["state"]["read_mode"].as_string(), "head");
   BOOST_CHECK(!result["state"]["synced"].as_bool());
   BOOST_CHECK_EQUAL(server.query_requests.load(), 1);
   BOOST_CHECK_EQUAL(server.denied_table_requests.load(), 1);
   server.request(request_body(selected_sql));
   for (
      const auto& body :
      {std::string("{"), std::string("[]"), std::string("{}"),
       std::string(
          R"({"jsonrpc":"2.0","id":true,"method":"query.execute","params":{"query":"SELECT * FROM sample.positions"}})"),
       std::string(R"({"jsonrpc":"2.0","id":4294967296,"method":"query.execute","params":{"query":"x"}})"),
       std::string(R"({"jsonrpc":"2.0","id":null,"method":"unknown"})"),
       std::string(R"({"jsonrpc":"2.0","id":1,"method":"query.execute","params":{"query":"x","owner":"sample"}})"),
       request_body("SELECT * FROM sample.positions JOIN other.positions"),
       request_body("SELECT missing FROM sample.positions")}) {
      const auto error = server.request(body);
      BOOST_REQUIRE(error.result() == http::status::ok);
      BOOST_CHECK(fc::json::from_string(error.body()).get_object().contains("error"));
   }
   // The error contract on the wire: numeric code, retryability, 1-based position and the echoed ID.
   const auto syntax =
      fc::json::from_string(server.request(request_body("SELECT * FROM sample.positions JOIN other.positions")).body());
   BOOST_CHECK_EQUAL(syntax["id"].as_string(), "test");
   BOOST_CHECK_EQUAL(syntax["error"]["code"].as_int64(), error_code(error_kind::QUERY_SYNTAX));
   BOOST_CHECK_EQUAL(syntax["error"]["data"]["kind"].as_string(), "QUERY_SYNTAX");
   BOOST_CHECK(!syntax["error"]["data"]["retryable"].as_bool());
   BOOST_CHECK_EQUAL(syntax["error"]["data"]["line"].as_uint64(), 1);
   BOOST_CHECK_EQUAL(syntax["error"]["data"]["column"].as_uint64(), 32);
   for (const auto& body :
        {R"({"jsonrpc":"2.0","method":"query.execute","params":{"query":"SELECT * FROM sample.positions"}})",
         R"({"jsonrpc":"2.0","method":"query.execute","params":{"query":"bad sql"}})",
         R"({"jsonrpc":"2.0","method":"unknown"})"}) {
      BOOST_CHECK(server.request(body).result() == http::status::no_content);
   }
   for (const auto* id : {"null", "4294967295", "-4294967295", "1.0e2", "\"9007199254740993\""}) {
      const auto body =
         std::string(R"({"jsonrpc":"2.0","id":)") + id +
         R"(,"method":"query.execute","params":{"query":"SELECT COUNT(*) AS records FROM sample.positions"}})";
      BOOST_CHECK(server.request(body).result() == http::status::ok);
   }
   fixture.put(1, chain_fixture::position(600));
   server.sync(fixture);
   const auto updated = fc::json::from_string(server.request(request_body(all_rows_sql)).body())["result"];
   BOOST_CHECK_EQUAL(updated["rows"][size_t{0}]["total"].as_string(), "690");
   BOOST_CHECK_EQUAL(server.denied_table_requests.load(), 1);
}

/// Registration alone does not enable the endpoint or initialize its service.
BOOST_AUTO_TEST_CASE(query_http_plugin_disabled) {
   http_application server;
   BOOST_REQUIRE(server.initialize(false) == chain::exit_code::SUCCESS);
   server.start();
   BOOST_CHECK(server.request(request_body(selected_sql)).result() == http::status::not_found);
}

/// Reject a configured producer during initialization, before workers or routes exist. Zero read
/// threads keep producer_plugin's own producer/read-thread check out of the way, so the rejection is
/// the query plugin's.
BOOST_AUTO_TEST_CASE(query_http_rejects_producer) {
   constexpr uint32_t no_read_threads = 0;
   const std::vector<std::string> producer = {"--producer-name", "sysio"};
   {
      // Control: the same node without the query plugin initializes, so the rejection below is ours.
      http_application control;
      BOOST_REQUIRE(control.initialize(false, no_read_threads, producer) == chain::exit_code::SUCCESS);
   }
   http_application server;
   BOOST_CHECK(server.initialize(true, no_read_threads, producer) != chain::exit_code::SUCCESS);
}

/// Startup rejects speculative chain state independently of HTTP enablement.
BOOST_AUTO_TEST_CASE(query_http_startup_guards) {
   http_application server;
   BOOST_REQUIRE(server.initialize(true, default_read_threads, {}, "speculative") == chain::exit_code::SUCCESS);
   BOOST_CHECK(server.run_until_startup_fails() != chain::exit_code::SUCCESS);
}

/// Reads run on the read-exclusive queue, which only read-only threads drain: startup rejects a node without them.
BOOST_AUTO_TEST_CASE(query_http_requires_read_only_threads) {
   http_application server;
   BOOST_REQUIRE(server.initialize(true, 0) == chain::exit_code::SUCCESS);
   BOOST_CHECK(server.run_until_startup_fails() != chain::exit_code::SUCCESS);
}

/// A configured capture bound may only tighten producer_plugin's read-only transaction budget: a value
/// within the request timeout still fails at startup once it exceeds that budget.
BOOST_AUTO_TEST_CASE(query_http_capture_bound_cannot_exceed_read_only_budget) {
   {
      http_application server;
      BOOST_REQUIRE(server.initialize(true, default_read_threads, {flag(option::max_capture_ms), "500"}) ==
                    chain::exit_code::SUCCESS);
      BOOST_CHECK(server.run_until_startup_fails() != chain::exit_code::SUCCESS);
   }
   chain_fixture fixture;
   http_application server;
   BOOST_REQUIRE(server.initialize(true, default_read_threads, {flag(option::max_capture_ms), "5"}) ==
                 chain::exit_code::SUCCESS);
   server.start();
   server.sync(fixture);
   BOOST_CHECK(
      fc::json::from_string(server.request(request_body(selected_sql)).body()).get_object().contains("result"));
}

/// The capture bound is producer_plugin's read-only transaction budget: the smaller of
/// max-transaction-time and the read window less the minimum producer_plugin keeps free of new work.
/// Inside a read window, the deadline producer_plugin reports lies ahead by at most the window.
BOOST_AUTO_TEST_CASE(query_http_capture_bound_follows_the_read_window) {
   constexpr auto read_window = std::chrono::milliseconds(30);
   // The floor producer_plugin keeps free of new work in every read window, per its option help.
   constexpr auto read_window_minimum = std::chrono::milliseconds(10);
   chain_fixture fixture;
   http_application server;
   BOOST_REQUIRE(
      server.initialize(true, default_read_threads,
                        {flag(bound::read_window), std::to_string(std::chrono::microseconds(read_window).count())}) ==
      chain::exit_code::SUCCESS);
   server.start();
   server.sync(fixture);
   const auto engine = std::dynamic_pointer_cast<sysio::query_engine::query_engine>(
      app().get_plugin<sysio::query_engine_plugin>().get_query_service());
   BOOST_REQUIRE(engine);
   BOOST_CHECK(engine->config().max_capture == read_window - read_window_minimum);
   BOOST_CHECK_EQUAL(engine->config().max_capture.count(),
                     app().get_plugin<producer_plugin>().get_read_only_max_transaction_time().count());
   // The window time the plugin binds on every read agrees with producer_plugin's deadline.
   std::promise<std::pair<fc::microseconds, std::chrono::microseconds>> remaining_completion;
   auto remaining = remaining_completion.get_future();
   server.post_read([&] {
      const auto& producer = app().get_plugin<producer_plugin>();
      remaining_completion.set_value({producer.get_read_only_window_deadline() - fc::time_point::now(),
                                      sysio::query_engine_plugin::read_window_remaining(producer)});
   });
   BOOST_REQUIRE(remaining.wait_for(test_wait) == std::future_status::ready);
   const auto [direct, plugin_remaining] = remaining.get();
   // The task was popped while the window was open; a preempted thread may measure a hair past its end.
   constexpr auto scheduling_margin = std::chrono::milliseconds(5);
   BOOST_CHECK_GT(direct.count(), -std::chrono::microseconds(scheduling_margin).count());
   BOOST_CHECK_LE(direct.count(), std::chrono::microseconds(read_window).count());
   BOOST_CHECK_LE(std::abs(plugin_remaining.count() - direct.count()),
                  std::chrono::microseconds(scheduling_margin).count());
   BOOST_CHECK(
      fc::json::from_string(server.request(request_body(selected_sql)).body()).get_object().contains("result"));
   // The plugin binds that window on every read: a budget the engine ran carries the capture deadline
   // the read API set, no later than one window after the read.
   const auto budget = engine->create_budget();
   BOOST_CHECK_EQUAL(engine->execute(selected_sql, budget).rows.size(), 1);
   BOOST_REQUIRE(budget->capture_deadline);
   BOOST_CHECK(*budget->capture_deadline <= query_budget::clock::now() + read_window);
   BOOST_CHECK(*budget->capture_deadline > budget->started);
}

/// Chain reads run on the read-exclusive queue, which the application thread never drains: with a
/// write window longer than the request deadline, a query cannot complete on the main thread the way
/// a read_only task would, and times out waiting for a read window.
BOOST_AUTO_TEST_CASE(query_http_reads_wait_for_a_read_window) {
   chain_fixture fixture;
   http_application server;
   constexpr auto long_write_window = std::chrono::seconds(30);
   constexpr auto short_timeout = std::chrono::milliseconds(500);
   BOOST_REQUIRE(server.initialize(
                    true, default_read_threads,
                    {flag(write_window_option), std::to_string(std::chrono::microseconds(long_write_window).count()),
                     flag(option::timeout_ms), std::to_string(short_timeout.count())}) == chain::exit_code::SUCCESS);
   server.start();
   server.sync(fixture);
   const auto response = fc::json::from_string(server.request(request_body(selected_sql)).body());
   BOOST_REQUIRE_MESSAGE(response.get_object().contains("error"),
                         fc::json::to_string(response, fc::time_point::maximum()));
   BOOST_CHECK_EQUAL(response["error"]["data"]["kind"].as_string(), "QUERY_TIMEOUT");
}

/// Registering HTTP never makes it a required dependency; execute remains usable without listeners.
BOOST_AUTO_TEST_CASE(query_service_without_http) {
   for (const uint32_t read_threads : {1, 2}) {
      for (const bool http_enabled : {false, true}) {
         chain_fixture fixture;
         http_application server;
         BOOST_REQUIRE(server.initialize(true, read_threads, {}, "head", false, http_enabled) ==
                       chain::exit_code::SUCCESS);
         auto& plugin = app().get_plugin<sysio::query_engine_plugin>();
         BOOST_CHECK(!plugin.get_query_service());
         if (!http_enabled) {
            BOOST_CHECK(app().get_plugin<http_plugin>().get_state() == appbase::abstract_plugin::registered);
            BOOST_CHECK(app().get_plugin<chain_api_plugin>().get_state() == appbase::abstract_plugin::registered);
         }
         server.start();
         server.sync(fixture);
         auto service = plugin.get_query_service();
         BOOST_REQUIRE(service);
         std::optional<error_kind> application_error;
         server.write([&] {
            try {
               service->execute(selected_sql, query_options{.timeout = std::chrono::milliseconds(0)});
            } catch (const query_error& error) {
               application_error = error.kind;
            }
         });
         BOOST_REQUIRE(application_error);
         BOOST_CHECK(*application_error == error_kind::INVALID_PARAMS);
         const auto result = service->execute(all_rows_sql, query_options{.limit = 1});
         BOOST_REQUIRE_EQUAL(result.rows.size(), 1);
         BOOST_CHECK_EQUAL(result.rows[0]["total"].as_string(), "150");
         BOOST_CHECK_EQUAL(result.state["block_id"].as_string(), fixture.control->head().id().str());
         BOOST_CHECK_EQUAL(server.query_requests.load(), 0);
         server.stop();
         BOOST_CHECK(!app().find_plugin<sysio::query_engine_plugin>());
         BOOST_CHECK_EXCEPTION(service->execute(selected_sql), query_error,
                               [](const auto& error) { return error.kind == error_kind::QUERY_CANCELLED; });
      }
   }
}

/// Irreversible mode reads only the locally applied finalized state and labels it explicitly.
BOOST_AUTO_TEST_CASE(query_http_irreversible_read_mode) {
   chain_fixture fixture;
   fixture.produce_blocks(3);
   http_application server;
   BOOST_REQUIRE(server.initialize(true, default_read_threads, {}, "irreversible") == chain::exit_code::SUCCESS);
   server.start();
   server.sync(fixture);
   const auto response = fc::json::from_string(server.request(request_body(all_rows_sql)).body());
   BOOST_REQUIRE_MESSAGE(response.get_object().contains("result"),
                         fc::json::to_string(response, fc::time_point::maximum()));
   const auto& result = response["result"];
   BOOST_CHECK_EQUAL(result["state"]["read_mode"].as_string(), "irreversible");
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["total"].as_string(), "150");
   BOOST_CHECK_LE(result["state"]["block_num"].as_uint64(), result["state"]["last_irreversible_block_num"].as_uint64());
}

/// Saturating admission rejects another HTTP request without waiting for chain work.
BOOST_AUTO_TEST_CASE(query_http_admission_while_chain_queue_is_blocked) {
   chain_fixture fixture;
   http_application server;
   BOOST_REQUIRE(server.initialize(true, default_read_threads, {flag(option::max_in_flight), "1"}) ==
                 chain::exit_code::SUCCESS);
   server.start();
   server.sync(fixture);
   auto entered = std::make_shared<std::promise<void>>();
   auto reached = entered->get_future();
   auto resume = std::make_shared<std::promise<void>>();
   const auto release = resume->get_future().share();
   auto blocked = server.post_write([entered, release] {
      entered->set_value();
      if (release.wait_for(test_wait) != std::future_status::ready)
         throw std::runtime_error("Admission barrier was not released");
   });
   BOOST_REQUIRE(reached.wait_for(test_wait) == std::future_status::ready);
   std::array<std::future<http::response<http::string_body>>, 2> requests;
   for (auto& request : requests)
      request = std::async(std::launch::async, [&] { return server.request(request_body(selected_sql)); });
   const auto deadline = query_budget::clock::now() + test_wait;
   size_t completed = requests.size();
   while (completed == requests.size() && query_budget::clock::now() < deadline) {
      for (size_t i = 0; i < requests.size(); ++i)
         if (requests[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            completed = i;
            break;
         }
      std::this_thread::yield();
   }
   resume->set_value();
   BOOST_REQUIRE(completed != requests.size());
   const auto busy = fc::json::from_string(requests[completed].get().body());
   BOOST_CHECK_EQUAL(busy["error"]["data"]["kind"].as_string(), "QUERY_BUSY");
   BOOST_CHECK(!busy.get_object().contains("result"));
   BOOST_REQUIRE(blocked.wait_for(test_wait) == std::future_status::ready);
   blocked.get();
   for (size_t i = 0; i < requests.size(); ++i) {
      if (i == completed)
         continue;
      BOOST_REQUIRE(requests[i].wait_for(test_wait) == std::future_status::ready);
      BOOST_CHECK(fc::json::from_string(requests[i].get().body()).get_object().contains("result"));
   }
}

/// A real executor excludes block application between pages, for one and for several read threads.
BOOST_AUTO_TEST_CASE(query_application_coherent_pages_and_capture_timeout) {
   for (const uint32_t read_threads : {1U, 2U}) {
      chain_fixture fixture;
      constexpr uint32_t extra_rows = 600;
      fixture.seed(account, 4, extra_rows, "alice"_n, 1);
      fixture.produce_block();
      http_application server;
      BOOST_REQUIRE(server.initialize(true, read_threads) == chain::exit_code::SUCCESS);
      server.start();
      server.sync(fixture);
      const auto original_head = fixture.control->head().id().str();
      fixture.put(1, chain_fixture::position(600));

      // Freeze query time while the barrier is held; test deadlines remain real wall time.
      const auto started = query_budget::clock::now();
      std::promise<void> paused, resume;
      auto reached = paused.get_future();
      auto release = resume.get_future();
      query_budget* observed_budget = nullptr;
      bool capturing = false, barrier_passed = false, used_read_window = false;
      query_budget budget({}, [&] {
         if (capturing && !barrier_passed && observed_budget->scanned_rows == constants::page_rows) {
            barrier_passed = true;
            paused.set_value();
            if (release.wait_for(test_wait) != std::future_status::ready)
               throw std::runtime_error("Capture barrier was not released");
         }
         return started;
      });
      observed_budget = &budget;
      const auto ast = parse_query("SELECT SUM(amount) AS total FROM sample.positions", budget);
      const auto plan = create_plan(ast, server.describe(ast, budget), budget);
      std::promise<captured_input> completion;
      auto captured = completion.get_future();
      server.post_read([&] {
         try {
            auto& controller = app().get_plugin<chain_plugin>().chain();
            used_read_window = !controller.is_write_window();
            local_table_source source(controller);
            capturing = true;
            auto input = source.capture(plan, budget);
            capturing = false;
            completion.set_value(std::move(input));
         } catch (...) {
            completion.set_exception(std::current_exception());
         }
      });
      const auto barrier_status = reached.wait_for(test_wait);
      if (barrier_status != std::future_status::ready)
         resume.set_value();
      BOOST_REQUIRE(barrier_status == std::future_status::ready);
      auto update = server.post_sync(fixture);
      const auto update_while_reading = update.wait_for(std::chrono::milliseconds(0));
      resume.set_value();
      BOOST_CHECK(update_while_reading == std::future_status::timeout);
      BOOST_REQUIRE(captured.wait_for(test_wait) == std::future_status::ready);
      auto input = captured.get();
      BOOST_CHECK(used_read_window);
      BOOST_CHECK_EQUAL(input.native_pages, 2);
      BOOST_CHECK_EQUAL(input.state["block_id"].as_string(), original_head);
      BOOST_REQUIRE(update.wait_for(test_wait) == std::future_status::ready);
      update.get();
      // Evaluation deliberately occurs after the scheduled update has applied.
      const auto result = fc::variant(evaluate(plan, std::move(input), budget));
      BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["total"].as_string(), "770");
      const auto next = fc::json::from_string(
         server.request(request_body("SELECT SUM(amount) AS total FROM sample.positions")).body())["result"];
      BOOST_CHECK_EQUAL(next["rows"][size_t{0}]["total"].as_string(), "1310");
      BOOST_CHECK_EQUAL(next["state"]["block_id"].as_string(), fixture.control->head().id().str());

      // Expire only the capture budget after one page, then prove later block work runs.
      std::promise<error_kind> timeout_completion;
      auto timed_out = timeout_completion.get_future();
      server.post_read([&] {
         query_budget* active = nullptr;
         query_budget timeout({}, [&] {
            return started + (active && active->scanned_rows >= constants::page_rows
                                 ? defaults::max_capture
                                 : std::chrono::microseconds::zero());
         });
         active = &timeout;
         try {
            local_table_source source(app().get_plugin<chain_plugin>().chain());
            source.capture(plan, timeout);
            timeout_completion.set_value(error_kind::INTERNAL_ERROR);
         } catch (const query_error& error) {
            timeout_completion.set_value(error.kind);
         } catch (...) {
            timeout_completion.set_exception(std::current_exception());
         }
      });
      BOOST_REQUIRE(timed_out.wait_for(test_wait) == std::future_status::ready);
      BOOST_CHECK(timed_out.get() == error_kind::QUERY_TIMEOUT);
      fixture.produce_block();
      server.sync(fixture);
      const auto after = fc::json::from_string(server.request(request_body(selected_sql)).body())["result"];
      BOOST_CHECK_EQUAL(after["state"]["block_id"].as_string(), fixture.control->head().id().str());
   }
}

/// Measure bounded native capture and concurrent HTTP work while signed blocks continue applying.
BOOST_AUTO_TEST_CASE(query_application_representative_workload) {
   chain_fixture fixture;
   constexpr uint32_t workload_rows = 1024;
   fixture.seed(account, 4, workload_rows, "alice"_n, 1);
   fixture.produce_block();
   http_application server;
   BOOST_REQUIRE(server.initialize(true, 2) == chain::exit_code::SUCCESS);
   server.start();
   server.sync(fixture);
   const auto before = fixture.control->head().block_num();
   const auto sql = "SELECT beneficiary, SUM(amount) AS total FROM sample.positions GROUP BY beneficiary";
   query_budget budget(relaxed_config());
   const auto ast = parse_query(sql, budget);
   const auto plan = create_plan(ast, server.describe(ast, budget), budget);
   std::promise<void> baseline_completion;
   auto baseline = baseline_completion.get_future();
   uint64_t capture_us = 0, raw_bytes = 0, memory_bytes = 0;
   server.post_read([&] {
      try {
         local_table_source source(app().get_plugin<chain_plugin>().chain());
         const auto input = source.capture(plan, budget);
         capture_us = input.capture_us;
         raw_bytes = budget.raw_bytes;
         memory_bytes = budget.accounted_bytes;
         baseline_completion.set_value();
      } catch (...) {
         baseline_completion.set_exception(std::current_exception());
      }
   });
   BOOST_REQUIRE(baseline.wait_for(test_wait) == std::future_status::ready);
   baseline.get();
   const auto begin = query_budget::clock::now();
   std::vector<std::future<http::response<http::string_body>>> requests;
   for (uint32_t i = 0; i < defaults::max_in_flight; ++i)
      requests.emplace_back(std::async(std::launch::async, [&] { return server.request(request_body(sql)); }));
   fixture.produce_block();
   server.sync(fixture);
   for (auto& request : requests) {
      BOOST_REQUIRE(request.wait_for(test_wait) == std::future_status::ready);
      const auto response = fc::json::from_string(request.get().body());
      BOOST_REQUIRE(response.get_object().contains("result"));
      BOOST_CHECK_EQUAL(response["result"]["stats"]["scanned_rows"].as_string(), std::to_string(workload_rows + 3));
   }
   const auto elapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(query_budget::clock::now() - begin).count();
   const auto after =
      fc::json::from_string(server.request(request_body(selected_sql)).body())["result"]["state"]["block_num"]
         .as_uint64();
   BOOST_CHECK_GT(after, before);
   BOOST_TEST_MESSAGE("Workload: rows=" << workload_rows + 3 << ", concurrency=" << defaults::max_in_flight
                                        << ", native_capture_us=" << capture_us << ", capture_raw_bytes=" << raw_bytes
                                        << ", capture_accounted_bytes=" << memory_bytes << ", admitted_memory_cap="
                                        << defaults::max_in_flight * defaults::max_memory_bytes
                                        << ", concurrent_elapsed_us=" << elapsed
                                        << ", block_progress=" << after - before);
}
