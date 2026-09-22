#include "query_fixture.hpp"
#include "query_schema.hpp"

#include <sysio/chain/app.hpp>
#include <sysio/chain_api_plugin/chain_api_plugin.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/query_engine_plugin/query_engine_plugin.hpp>

#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <fstream>

using namespace sysio;
using namespace sysio::query_engine_plugin;
using namespace sysio::query_engine_plugin::test;
namespace http = boost::beast::http;
namespace {
constexpr auto socket_name = "query.sock";
constexpr auto genesis_name = "genesis.json";
constexpr auto all_rows_sql = "SELECT beneficiary, COUNT(*) AS records, SUM(amount) AS total FROM positions OWNER "
                              "'sample' GROUP BY beneficiary ORDER BY total DESC";
constexpr auto selected_sql = "SELECT * FROM sample.positions WHERE key.id = 1";
constexpr auto table_route = "/v1/chain/get_table_rows";
constexpr int http_version = 11;

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
   chain::exit_code::exit_code initialize(bool enabled, uint32_t read_threads = 0,
                                          const std::vector<std::string>& extra = {},
                                          const std::string& read_mode = "head", bool listener = true,
                                          bool enable_http = true) {
      appbase::application_base::register_plugin<sysio::query_engine_plugin::query_engine_plugin>();
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
         args.insert(args.end(), {"--plugin", "sysio::query_engine_plugin::query_engine_plugin"});
      if (enable_http)
         args.insert(args.end(), {"--plugin", "sysio::chain_api_plugin", "--plugin", "sysio::http_plugin"});
      args.insert(args.end(), extra.begin(), extra.end());
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

   /// Pool-specific captures use the real read-exclusive queue so the app thread cannot drain
   /// them in a write window before producer_plugin starts its read workers.
   void post_read(std::function<void()> function, bool require_read_window = false) {
      app().executor().post(priority::medium_low,
                            require_read_window ? exec_queue::read_exclusive : exec_queue::read_only,
                            std::move(function));
   }

   /// Copy ABI descriptions on the read executor; callers compile plans outside its callbacks.
   std::vector<table_schema> describe(const ast_query& ast, query_budget& budget) {
      auto completion = std::make_shared<std::promise<std::vector<table_schema>>>();
      auto result = completion->get_future();
      post_read([&ast, &budget, completion] {
         try {
            local_table_source source(app().get_plugin<chain_plugin>().chain());
            completion->set_value(source.describe(ast, budget));
         } catch (...) {
            completion->set_exception(std::current_exception());
         }
      });
      BOOST_REQUIRE(result.wait_for(test_wait) == std::future_status::ready);
      return result.get();
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

/// Reject a configured producer during initialization, before workers or routes exist.
BOOST_AUTO_TEST_CASE(query_http_rejects_producer) {
   http_application server;
   BOOST_CHECK(server.initialize(true, 0, {"--producer-name", "sysio"}) != chain::exit_code::SUCCESS);
}

/// Startup rejects speculative chain state independently of HTTP enablement.
BOOST_AUTO_TEST_CASE(query_http_startup_guards) {
   http_application server;
   BOOST_REQUIRE(server.initialize(true, 0, {}, "speculative") == chain::exit_code::SUCCESS);
   server.loop = std::thread([&] { server.exit.set_value(server.application.exec()); });
   const auto finished = server.exited.wait_for(test_wait);
   if (finished != std::future_status::ready)
      app().quit();
   BOOST_REQUIRE(finished == std::future_status::ready);
   BOOST_CHECK(server.exited.get() != chain::exit_code::SUCCESS);
}

/// Registering HTTP never makes it a required dependency; execute remains usable without listeners.
BOOST_AUTO_TEST_CASE(query_service_without_http) {
   for (const uint32_t read_threads : {0, 2}) {
      for (const bool http_enabled : {false, true}) {
         chain_fixture fixture;
         http_application server;
         BOOST_REQUIRE(server.initialize(true, read_threads, {}, "head", false, http_enabled) ==
                       chain::exit_code::SUCCESS);
         auto& plugin = app().get_plugin<sysio::query_engine_plugin::query_engine_plugin>();
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
               service->execute(selected_sql, query_options{.timeout_ms = 0});
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
         BOOST_CHECK(!app().find_plugin<sysio::query_engine_plugin::query_engine_plugin>());
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
   BOOST_REQUIRE(server.initialize(true, 0, {}, "irreversible") == chain::exit_code::SUCCESS);
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
   BOOST_REQUIRE(server.initialize(true, 0, {"--query-max-in-flight", "1"}) == chain::exit_code::SUCCESS);
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

/// A real executor excludes block application between pages, in both read-thread configurations.
BOOST_AUTO_TEST_CASE(query_application_coherent_pages_and_capture_timeout) {
   for (const uint32_t read_threads : {0U, 2U}) {
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
      server.post_read(
         [&] {
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
         },
         read_threads != 0);
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
      BOOST_CHECK_EQUAL(used_read_window, read_threads != 0);
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
      app().executor().post(priority::medium_low, exec_queue::read_only, [&] {
         query_budget* active = nullptr;
         query_budget timeout({}, [&] {
            return started + std::chrono::milliseconds(
                                active && active->scanned_rows >= constants::page_rows ? defaults::max_capture_ms : 0);
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
   query_budget budget({});
   const auto ast = parse_query(sql, budget);
   const auto plan = create_plan(ast, server.describe(ast, budget), budget);
   std::promise<void> baseline_completion;
   auto baseline = baseline_completion.get_future();
   uint64_t capture_us = 0, raw_bytes = 0, memory_bytes = 0;
   app().executor().post(priority::medium_low, exec_queue::read_only, [&] {
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
