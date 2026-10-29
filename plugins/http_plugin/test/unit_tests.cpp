#include <sysio/chain/application.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/http_plugin/common.hpp>
#include <sysio/http_plugin/bind_stream.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/io/json_stream.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <fc/crypto/rand.hpp>
#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>

#define BOOST_TEST_MODULE http_plugin unit tests
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <future>
#include <optional>

#include "test_ports.hpp"

namespace bu = boost::unit_test;

using std::string;

using namespace appbase;
using namespace sysio;

namespace beast = boost::beast;     // from <boost/beast.hpp>
namespace http  = beast::http;      // from <boost/beast/http.hpp>
namespace net   = boost::asio;      // from <boost/asio.hpp>
using tcp       = net::ip::tcp;     // from <boost/asio/ip/tcp.hpp>

namespace {
constexpr uint32_t default_http_index = 0;
constexpr uint32_t category_rw_index = 1;
constexpr uint32_t category_ro_index = 2;
constexpr uint32_t bytes_in_flight_index = 3;
constexpr uint32_t requests_in_flight_index = 4;
constexpr uint32_t request_body_bytes_in_flight_index = 5;
constexpr uint32_t category_snapshot_index = 6;
constexpr uint32_t plaintext_response_index = 7;
constexpr uint32_t stream_status_codes_index = 8;
constexpr uint32_t stream_request_body_index = 9;
constexpr uint32_t stream_response_bytes_index = 10;
constexpr uint32_t stream_over_budget_index = 11;
constexpr uint32_t stream_single_token_index = 12;
constexpr uint32_t stream_queued_payload_index = 13;
constexpr uint32_t stream_deferred_payload_index = 14;
constexpr uint32_t stream_absorbed_abort_index = 15;
constexpr uint32_t request_body_read_index = 16;
constexpr uint32_t request_body_deadline_index = 17;
/** Index of the IPv6 probe listener used by the IPv6 tests. */
constexpr uint32_t ipv6_probe_index = 2;

/** Return the string form of a node HTTP port from this test's lease. */
std::string test_http_port(uint32_t index) {
   return std::to_string(sysio::testing::get_port(sysio::testing::port_category::node_http, index));
}

/** Return host:port for a node HTTP port from this test's lease. */
std::string test_http_endpoint(const std::string& host, uint32_t index) {
   return host + ":" + test_http_port(index);
}
} // namespace

// -------------------------------------------------------------------------
// this class handles some basic http requests.
// -------------------------------------------------------------------------
class Db
{
 public:
   void add_api(http_plugin& p) {
      p.add_api({
            {  std::string("/hello"),
               api_category::node,
               [&](string&&, string&& body, url_response_callback&& cb) {
                  cb(200, fc::variant("world!"));
               }
            },
            {  std::string("/echo"),
               api_category::node,
               [&](string&&, string&& body, url_response_callback&& cb) {
                  cb(200, fc::variant(body));
               }
            },
            {  std::string("/check_ones"), // returns "yes" if body only has only '1' chars, "no" otherwise
               api_category::node,
               [&](string&&, string&& body, url_response_callback&& cb) {
                  bool ok = std::all_of(body.begin(), body.end(), [](char c) { return c == '1'; });
                  cb(200, fc::variant(ok ? string("yes") : string("no")));
               }
          },
         }, appbase::exec_queue::read_write);
   }

 private:
};

// --------------------------------------------------------------------------
// Expect100ContinueProtocol sends requests using the `Expect "100-continue"`
// from HTTP 1.1
// --------------------------------------------------------------------------
template <class Results>
struct ProtocolCommon
{
   auto get_response() -> std::optional<string>  {
      try {
         beast::flat_buffer buffer;
         http::response<http::dynamic_body> res;
         http::read(stream, buffer, res);
         return { beast::buffers_to_string(res.body().data()) };
      }
      catch(std::exception const& e)
      {
         std::cerr << "Error: " << e.what() << std::endl;
         this->reconnect();
         return {};
      }
   }

   void reconnect()
   {
      try {
         stream.connect(results);
      } catch(...) {};
   }

   const char* host;
   beast::tcp_stream& stream;
   Results& results;
};

template <class Results>
struct BasicProtocol : public ProtocolCommon<Results>
{
   bool send_request(const char* r, const char* body, bool expect_fail) {
      try {
         http::request<http::string_body> req{http::verb::post, r, 11};
         req.set(http::field::host, this->host);
         req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
         if (body) {
            req.body() = body;
            req.prepare_payload();
         }
         return http::write(this->stream, req) != 0;
      }
      catch(std::exception const& e)
      {
         std::cerr << "Error: " << e.what() << std::endl;
         this->reconnect();
         return false;
      }
   }
};

template <class Results>
struct Expect100ContinueProtocol : public ProtocolCommon<Results>
{
   bool send_request(const char* r, const char* body, bool expect_fail) {
      try {
         http::request<http::string_body> req{http::verb::post, r, 11};
         req.set(http::field::host, this->host);
         req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
         if (body) {
            req.set(http::field::expect, "100-continue");
            req.body() = body;
            req.prepare_payload();

            http::request_serializer<http::string_body> sr{req};
            beast::error_code ec;
            http::write_header(this->stream, sr, ec);
            if (ec) {
               BOOST_CHECK_MESSAGE(expect_fail, "write_header failed");
               return false;
            }

            {
               http::response<http::string_body> res {};
               beast::flat_buffer buffer;
               http::read(this->stream, buffer, res, ec);
               // std::cerr << "Result: " << res.result() << '\n';
               if (ec) {
                  BOOST_CHECK_MESSAGE(expect_fail, "continue_ read  failed");
                  if (res.result() != http::status::continue_)
                  {
                     // The server indicated that it will not
                     // accept the request, so skip sending the body.
                     // actually we don't get here, the server closes the connection
                     BOOST_CHECK_MESSAGE(expect_fail, "server rejected 100-continue request");
                     this->reconnect();
                  }
                  return false;
               }
            }

            // Server is OK with the request, send the body
            http::write(this->stream, sr, ec);
            return !ec;
         }
         return http::write(this->stream, req) != 0;
      }
      catch(std::exception const& e)
      {
         std::cerr << "Error: " << e.what() << std::endl;
         this->reconnect();
         return false;
      }
   }
};

// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
template<class Protocol>
void check_request(Protocol& p, const char* r, const char* body,
                   std::optional<const char*> expected_response)
{
   if (p.send_request(r, body, !expected_response)) {
      auto resp = p.get_response();
      BOOST_CHECK(!resp || expected_response);
      if (expected_response) {
         // substr to remove enclosing '"' characters
         BOOST_CHECK(resp->substr(1, resp->size() - 2) == string(*expected_response));
      }
      if (resp)
         std::cout << *resp << '\n';
      else
         p.reconnect();
   } else
      BOOST_CHECK(!expected_response);
}

template<class Protocol>
void run_test(Protocol& p, size_t max_body_size)
{
   // try a echo
   check_request(p, "/echo", "hello", {"hello"});

   // try a simple request
   check_request(p, "/hello", nullptr, {"world!"});

   // check ones with small body
   check_request(p, "/check_ones", "111111111111111111111111", {"yes"});

   // check ones with long body exactly max_req_size - should work and return yes
   {
      string test_str;
      test_str.resize(max_body_size, '1');
      check_request(p, "/check_ones", test_str.c_str(), {"yes"});
   }

   // check ones with long body (should be rejected by http_plugin as over max_body_size
   {
      string test_str;
      test_str.resize(max_body_size + 1, '1');
      check_request(p, "/check_ones", test_str.c_str(), {}); // we don't expect a response
   }
}

namespace sysio {
class chain_api_plugin : public appbase::plugin<chain_api_plugin> {
 public:
   APPBASE_PLUGIN_REQUIRES();
   virtual void set_program_options(options_description& cli, options_description& cfg) override {}
   void         plugin_initialize(const variables_map& options) {}
   void         plugin_startup() {}
   void         plugin_shutdown() {}
};

class net_api_plugin : public appbase::plugin<net_api_plugin> {
 public:
   APPBASE_PLUGIN_REQUIRES();
   virtual void set_program_options(options_description& cli, options_description& cfg) override {}
   void         plugin_initialize(const variables_map& options) {}
   void         plugin_startup() {}
   void         plugin_shutdown() {}
};

class producer_api_plugin : public appbase::plugin<producer_api_plugin> {
 public:
   APPBASE_PLUGIN_REQUIRES();
   virtual void set_program_options(options_description& cli, options_description& cfg) override {}
   void         plugin_initialize(const variables_map& options) {}
   void         plugin_startup() {}
   void         plugin_shutdown() {}
};

// Minimal registration fixture for the retained snapshot HTTP category.
class snapshot_api_plugin : public appbase::plugin<snapshot_api_plugin> {
 public:
   APPBASE_PLUGIN_REQUIRES();
   virtual void set_program_options(options_description& cli, options_description& cfg) override {}
   void         plugin_initialize(const variables_map& options) {}
   void         plugin_startup() {}
   void         plugin_shutdown() {}
};

static auto _chain_api_plugin    = application::register_plugin<chain_api_plugin>();
static auto _net_api_plugin      = application::register_plugin<net_api_plugin>();
static auto _producer_api_plugin = application::register_plugin<producer_api_plugin>();
static auto _snapshot_api_plugin = application::register_plugin<snapshot_api_plugin>();
} // namespace sysio

struct http_plugin_test_fixture {
   appbase::scoped_app app;
   std::thread         app_thread;

   http_plugin* init(std::initializer_list<const char*> args) {
      if (app->initialize<http_plugin>(args.size(), const_cast<char**>(args.begin()))) {
         auto                       plugin  = app->find_plugin<http_plugin>();
         std::atomic<bool>& listening_or_failed = plugin->listening();
         app_thread = std::thread([&]() {
            try {
               app->startup();
               // app was constructed on the outer thread; capture this thread as main_thread_id_
               // so producer_plugin's main-thread asserts see the loop thread.
               app->executor().set_main_thread_id();
               app->exec();
            } catch (...) {
               plugin = nullptr;
               listening_or_failed.store(true);
            }
         });

         while (!listening_or_failed.load()) {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(10ms);
         }
         return plugin;
      }
      return nullptr;
   }

   ~http_plugin_test_fixture() {
      if (app_thread.joinable()) {
         app->quit();
         app_thread.join();
      }
   }
};

// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(http_plugin_unit_tests, http_plugin_test_fixture) {

   const uint16_t default_port = sysio::testing::get_port(sysio::testing::port_category::node_http);
   const std::string port = test_http_port(default_http_index);
   const char*       host = "127.0.0.1";

   http_plugin::set_defaults({.default_unix_socket_path = "", .default_http_port = default_port, .server_header = "/"});

   auto http_plugin =
       init({bu::framework::current_test_case().p_name->c_str(), "--plugin", "sysio::http_plugin",
             "--http-validate-host", "false", "--http-threads", "4", "--http-max-response-time-ms", "50"});

   BOOST_REQUIRE(http_plugin);
   BOOST_CHECK(http_plugin->get_state() == abstract_plugin::started);

   Db db;
   db.add_api(*http_plugin);

   size_t max_body_size = http_plugin->get_max_body_size();

   try
   {
      net::io_context ioc;

      // These objects perform our I/O
      tcp::resolver resolver(ioc);
      beast::tcp_stream stream(ioc);

      // Look up IP and connect to it
      auto const results = resolver.resolve(host, port);
      stream.connect(results);

      {
         BasicProtocol<decltype(results)> p{ {host, stream, results} };
         run_test(p, max_body_size);
      }

      {
         Expect100ContinueProtocol<decltype(results)> p{ {host, stream, results} };
         run_test(p, max_body_size);
      }

      // Gracefully close the socket
      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);
      if (ec && ec != beast::errc::not_connected) // not_connected happens sometimes
         throw beast::system_error{ec};
   }
   catch(std::exception const& e)
   {
      std::cerr << "Error: " << e.what() << "\n";
   }
}

class app_log {
   std::string result;
   int         fork_app_and_redirect_stderr(const char* redirect_filename, std::initializer_list<const char*> args) {
      int pid = fork();
      if (pid == 0) {
         [[maybe_unused]] auto stream = freopen(redirect_filename, "w", stderr);
         bool ret = 0;
         try {
            appbase::scoped_app app;
            ret = app->initialize<http_plugin>(args.size(), const_cast<char**>(args.begin()));
         } catch (...) {
         }
         fclose(stderr);
         exit(ret ? 0 : 1);
      } else {
         int chld_state;
         waitpid(pid, &chld_state, 0);
         BOOST_CHECK(WIFEXITED(chld_state));
         return WEXITSTATUS(chld_state);
      }
   }

 public:
   app_log(std::initializer_list<const char*> args) {
      fc::temp_directory dir;
      std::filesystem::path log = dir.path()/"test.stderr";
      BOOST_CHECK(fork_app_and_redirect_stderr(log.c_str(), args));
      std::ifstream file(log.c_str());
      result.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
      std::filesystem::remove(log);
   }

   boost::test_tools::predicate_result contains(const char* str) const {
      if (result.find(str) == std::string::npos) {
         boost::test_tools::predicate_result res(false);
         res.message() << "\nlog result: " << result << "\n";
         return res;
      }
      return true;
   }

   boost::test_tools::predicate_result contains(const std::string& str) const { return contains(str.c_str()); }
};

BOOST_AUTO_TEST_CASE(invalid_category_addresses) {

   const char* test_name = bu::framework::current_test_case().p_name->c_str();
   const std::string localhost_rw = test_http_endpoint("localhost", category_rw_index);
   const std::string loopback_rw = test_http_endpoint("127.0.0.1", category_rw_index);
   const std::string chain_ro_localhost = "chain_ro," + localhost_rw;
   const std::string chain_ro_loopback = "chain_ro," + loopback_rw;
   const std::string chain_rw_localhost = "chain_rw," + localhost_rw;
   const std::string node_localhost = "node," + localhost_rw;
   const std::string unable_to_listen_msg = "unable to listen to port " + test_http_port(category_rw_index);

   BOOST_TEST(app_log({test_name, "--plugin=sysio::http_plugin", "--http-server-address",
                                 "http-category-address", "--http-category-address", chain_ro_localhost.c_str()})
                  .contains("--plugin=sysio::chain_api_plugin is required"));

   const std::string snapshot_localhost = "snapshot_ro," + localhost_rw;
   BOOST_TEST(app_log({test_name, "--plugin=sysio::chain_api_plugin", "--http-server-address",
                                 "http-category-address", "--http-category-address", snapshot_localhost.c_str()})
                  .contains("--plugin=sysio::snapshot_api_plugin is required"));

   // The test_control endpoints are registered by test_control_api_plugin, so that is the plugin the category needs.
   const std::string test_control_localhost = "test_control," + localhost_rw;
   BOOST_TEST(app_log({test_name, "--plugin=sysio::chain_api_plugin", "--http-server-address",
                                 "http-category-address", "--http-category-address", test_control_localhost.c_str()})
                  .contains("--plugin=sysio::test_control_api_plugin is required"));

   BOOST_TEST(app_log({test_name, "--plugin=sysio::chain_api_plugin", "--http-category-address", chain_ro_localhost.c_str()})
                  .contains("http-server-address must be set as `http-category-address`"));

   BOOST_TEST(app_log({test_name, "--plugin=sysio::chain_api_plugin", "--http-server-address",
                                 "http-category-address", "--unix-socket-path", "/tmp/tmp.sock",
                                 "--http-category-address", chain_ro_localhost.c_str()})
                  .contains("`unix-socket-path` must be left unspecified"));

   BOOST_TEST(app_log({test_name, "--plugin=sysio::chain_api_plugin", "--http-server-address",
                                 "http-category-address", "--http-category-address", node_localhost.c_str()})
                  .contains("invalid category name"));

   BOOST_TEST(app_log({test_name, "--plugin=sysio::chain_api_plugin", "--http-server-address",
                                 "http-category-address", "--http-category-address", chain_ro_loopback.c_str(),
                                 "--http-category-address", chain_rw_localhost.c_str()})
                  .contains(unable_to_listen_msg));
}

using unix_stream = beast::basic_stream<boost::asio::local::stream_protocol, beast::tcp_stream::executor_type,
                                        beast::unlimited_rate_policy>;

struct http_response_for {
   net::io_context                    ioc;
   http::response<http::dynamic_body> response;
   http_response_for(const char* addr, const char* path) {
      auto [host, port] = fc::split_host_port(addr);
      // These objects perform our I/O
      tcp::resolver     resolver(ioc);
      beast::tcp_stream stream(ioc);

      // Look up IP and connect to it
      auto const results = resolver.resolve(host, port);
      stream.connect(results);
      initiate(stream, addr, path);
   }

   http_response_for(std::filesystem::path addr, const char* path) {
      unix_stream stream(ioc);
      stream.connect(addr.c_str());
      initiate(stream, "", path);
   }

   template <typename Stream>
   void initiate(Stream&& stream, const char* addr, const char* path) {
      int                              http_version = 11;
      http::request<http::string_body> req{http::verb::post, path, http_version};
      if (addr)
         req.set(http::field::host, addr);
      req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
      BOOST_CHECK(http::write(stream, req) != 0);

      beast::flat_buffer buffer;
      http::read(stream, buffer, response);
   }

   http::status status() const { return response.result(); }

   std::string body() const { return beast::buffers_to_string(response.body().data()); }
};

/// Notifications have no body; ordinary absent payloads retain their JSON object.
BOOST_FIXTURE_TEST_CASE(no_content_response_over_unix_socket, http_plugin_test_fixture) {
   constexpr auto socket_name = "http-response.sock";
   constexpr auto empty_route = "/v1/node/no_content";
   constexpr auto ignored_payload_route = "/v1/node/no_content_payload";
   constexpr auto absent_payload_route = "/v1/node/absent_payload";
   fc::temp_directory directory;
   const auto socket = directory.path() / socket_name;
   auto* plugin = init({bu::framework::current_test_case().p_name->c_str(),
      "--data-dir", directory.path().c_str(), "--http-server-address", "", "--unix-socket-path", socket.c_str()});
   BOOST_REQUIRE(plugin);
   plugin->add_async_api({
      {empty_route, api_category::node, [](string&&, string&&, url_response_callback&& callback) {
         callback(magic_enum::enum_integer(http::status::no_content), std::nullopt);
      }},
      {ignored_payload_route, api_category::node, [](string&&, string&&, url_response_callback&& callback) {
         callback(magic_enum::enum_integer(http::status::no_content), fc::variant(true));
      }},
      {absent_payload_route, api_category::node, [](string&&, string&&, url_response_callback&& callback) {
         callback(magic_enum::enum_integer(http::status::ok), std::nullopt);
      }}
   });
   for (const auto* route : {empty_route, ignored_payload_route}) {
      const http_response_for response(std::filesystem::path(socket.string()), route);
      BOOST_CHECK(response.status() == http::status::no_content);
      BOOST_CHECK(response.body().empty());
   }
   const http_response_for response(std::filesystem::path(socket.string()), absent_payload_route);
   BOOST_CHECK(response.status() == http::status::ok);
   BOOST_CHECK_EQUAL(response.body(), "{}");
}

BOOST_FIXTURE_TEST_CASE(valid_category_addresses, http_plugin_test_fixture) {
   fc::temp_directory dir;
   auto               data_dir = dir.path() / "data";
   const std::string ro_port = test_http_port(category_ro_index);
   const std::string rw_port = test_http_port(category_rw_index);
   const std::string ro_loopback = test_http_endpoint("127.0.0.1", category_ro_index);
   const std::string ro_localhost = test_http_endpoint("localhost", category_ro_index);
   const std::string rw_loopback = test_http_endpoint("127.0.0.1", category_rw_index);
   const std::string rw_any = ":" + rw_port;
   const std::string snapshot_loopback = test_http_endpoint("127.0.0.1", category_snapshot_index);
   const std::string chain_ro = "chain_ro," + ro_loopback;
   const std::string chain_rw = "chain_rw," + rw_any;
   const std::string net_ro = "net_ro," + ro_loopback;
   const std::string net_rw = "net_rw," + rw_any;
   const std::string snapshot = "snapshot_ro," + snapshot_loopback;
   const std::string ipv6_rw = "[::1]:" + rw_port;

   // clang-format off
   auto http_plugin = init({bu::framework::current_test_case().p_name->c_str(),
                      "--data-dir", data_dir.c_str(),
                      "--plugin=sysio::chain_api_plugin",
                      "--plugin=sysio::net_api_plugin",
                      "--plugin=sysio::producer_api_plugin",
                      "--plugin=sysio::snapshot_api_plugin",
                      "--http-server-address", "http-category-address",
                      "--http-category-address", chain_ro.c_str(),
                      "--http-category-address", chain_rw.c_str(),
                      "--http-category-address", net_ro.c_str(),
                      "--http-category-address", net_rw.c_str(),
                      "--http-category-address", snapshot.c_str(),
                      "--http-category-address", "producer_ro,./producer_ro.sock",
                      "--http-category-address", "producer_rw,../producer_rw.sock"
                      });
   // clang-format on

   BOOST_REQUIRE(http_plugin);

   http_plugin->add_api({{std::string("/v1/node/hello"), api_category::node,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             cb(200, fc::variant("world!"));
                          }},
                         {std::string("/v1/chain_ro/hello"), api_category::chain_ro,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             cb(200, fc::variant("world!"));
                          }},
                         {std::string("/v1/chain_rw/hello"), api_category::chain_rw,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             cb(200, fc::variant("world!"));
                          }},
                         {std::string("/v1/net_ro/hello"), api_category::net_ro,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             cb(200, fc::variant("world!"));
                          }},
                         {std::string("/v1/net_rw/hello"), api_category::net_rw,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             cb(200, fc::variant("world!"));
                          }},
                         {std::string("/v1/producer_ro/hello"), api_category::producer_ro,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             cb(200, fc::variant("world!"));
                          }},
                         {std::string("/v1/producer_rw/hello"), api_category::producer_rw,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             cb(200, fc::variant("world!"));
                          }},
                         {std::string("/v1/snapshot/hello"), api_category::snapshot_ro,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             cb(200, fc::variant("world!"));
                          }}},
                        appbase::exec_queue::read_write);

   BOOST_CHECK(http_plugin->is_on_loopback(api_category::chain_ro));
   BOOST_CHECK(http_plugin->is_on_loopback(api_category::net_ro));
   BOOST_CHECK(http_plugin->is_on_loopback(api_category::producer_ro));
   BOOST_CHECK(http_plugin->is_on_loopback(api_category::producer_rw));
   BOOST_CHECK(http_plugin->is_on_loopback(api_category::snapshot_ro));
   BOOST_CHECK(!http_plugin->is_on_loopback(api_category::chain_rw));
   BOOST_CHECK(!http_plugin->is_on_loopback(api_category::net_rw));

   std::string world_string = "\"world!\"";

   BOOST_CHECK_EQUAL(http_response_for(ro_loopback.c_str(), "/v1/node/hello").body(), world_string);
   BOOST_CHECK_EQUAL(http_response_for(rw_loopback.c_str(), "/v1/node/hello").body(), world_string);

   bool ip_v6_enabled = [] {
      try {
         net::io_context ioc;
         tcp::socket s(ioc, tcp::endpoint{net::ip::make_address("::1"),
                                          sysio::testing::get_port(sysio::testing::port_category::ipv6_probe,
                                                                   ipv6_probe_index)});
         return true;
      } catch (...) {
         return false;
      }
   }();

   if (ip_v6_enabled) {
      BOOST_CHECK_EQUAL(http_response_for(ipv6_rw.c_str(), "/v1/node/hello").body(), world_string);
   }

   BOOST_CHECK_EQUAL(http_response_for(ro_loopback.c_str(), "/v1/chain_ro/hello").body(), world_string);
   BOOST_CHECK_EQUAL(http_response_for(ro_localhost.c_str(), "/v1/chain_ro/hello").status(), http::status::bad_request);
   BOOST_CHECK_EQUAL(http_response_for(ro_loopback.c_str(), "/v1/net_ro/hello").body(), world_string);
   BOOST_CHECK_EQUAL(http_response_for(ro_loopback.c_str(), "/v1/chain_rw/hello").status(), http::status::not_found);
   BOOST_CHECK_EQUAL(http_response_for(ro_loopback.c_str(), "/v1/net_rw/hello").status(), http::status::not_found);

   BOOST_CHECK_EQUAL(http_response_for(rw_loopback.c_str(), "/v1/chain_ro/hello").status(), http::status::not_found);
   BOOST_CHECK_EQUAL(http_response_for(rw_loopback.c_str(), "/v1/net_ro/hello").status(), http::status::not_found);
   BOOST_CHECK_EQUAL(http_response_for(rw_loopback.c_str(), "/v1/chain_rw/hello").body(), world_string);
   BOOST_CHECK_EQUAL(http_response_for(rw_loopback.c_str(), "/v1/net_rw/hello").body(), world_string);

   // The dedicated snapshot category serves its own listener (where node
   // handlers stay reachable, node being all categories) and is NOT served
   // by other category-isolated listeners.
   BOOST_CHECK_EQUAL(http_response_for(snapshot_loopback.c_str(), "/v1/snapshot/hello").body(), world_string);
   BOOST_CHECK_EQUAL(http_response_for(snapshot_loopback.c_str(), "/v1/node/hello").body(), world_string);
   BOOST_CHECK_EQUAL(http_response_for(ro_loopback.c_str(), "/v1/snapshot/hello").status(), http::status::not_found);
   BOOST_CHECK_EQUAL(http_response_for(rw_loopback.c_str(), "/v1/snapshot/hello").status(), http::status::not_found);

   BOOST_CHECK_EQUAL(http_response_for(data_dir / "./producer_ro.sock", "/v1/producer_ro/hello").body(), world_string);
   BOOST_CHECK_EQUAL(http_response_for(data_dir / "../producer_rw.sock", "/v1/producer_rw/hello").body(), world_string);

   BOOST_CHECK_EQUAL(http_response_for(ro_loopback.c_str(), "/v1/node/get_supported_apis").body(),
                     R"({"apis":["/v1/chain_ro/hello","/v1/net_ro/hello","/v1/node/hello"]})");

   BOOST_CHECK_EQUAL(http_response_for(rw_loopback.c_str(), "/v1/node/get_supported_apis").body(),
                     R"({"apis":["/v1/chain_rw/hello","/v1/net_rw/hello","/v1/node/hello"]})");

   BOOST_CHECK_EQUAL(http_response_for(snapshot_loopback.c_str(), "/v1/node/get_supported_apis").body(),
                     R"({"apis":["/v1/node/hello","/v1/snapshot/hello"]})");
}

bool on_loopback(std::initializer_list<const char*> args){
   appbase::scoped_app app;
   BOOST_REQUIRE(app->initialize<http_plugin>(args.size(), const_cast<char**>(args.begin())));
   return app->get_plugin<http_plugin>().is_on_loopback(api_category::chain_rw);
}

BOOST_AUTO_TEST_CASE(test_on_loopback) {
   const std::string loopback_default = test_http_endpoint("127.0.0.1", default_http_index);
   const std::string localhost_default = test_http_endpoint("localhost", default_http_index);
   const std::string any_default = ":" + test_http_port(default_http_index);
   const std::string external_default = test_http_endpoint("example.com", default_http_index);

   BOOST_CHECK(on_loopback({"test", "--plugin=sysio::http_plugin", "--http-server-address", "", "--unix-socket-path=a"}));
   BOOST_CHECK(on_loopback({"test", "--plugin=sysio::http_plugin", "--http-server-address", loopback_default.c_str()}));
   BOOST_CHECK(on_loopback({"test", "--plugin=sysio::http_plugin", "--http-server-address", localhost_default.c_str()}));
   BOOST_CHECK(!on_loopback({"test", "--plugin=sysio::http_plugin", "--http-server-address", any_default.c_str()}));
   BOOST_CHECK(!on_loopback({"test", "--plugin=sysio::http_plugin", "--http-server-address", external_default.c_str()}));
}

BOOST_FIXTURE_TEST_CASE(bytes_in_flight, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", bytes_in_flight_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(bytes_in_flight_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-max-bytes-in-flight-mb=64"});
   BOOST_REQUIRE(http_plugin);

   http_plugin->add_api({{std::string("/4megabyte"), api_category::node,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             fc::blob b;
                             b.data.resize(4*1024*1024);
                             fc::rand_bytes(b.data.data(), b.data.size());
                             cb(200, b);
                          }}}, appbase::exec_queue::read_write);

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   std::list<boost::asio::ip::tcp::socket> connections;

   auto send_4mb_requests = [&](unsigned count) {
      for(unsigned i = 0; i < count; ++i) {
         boost::asio::ip::tcp::socket& s = connections.emplace_back(ctx, boost::asio::ip::tcp::v4());
         //we can't control http_plugin's send buffer, but at least we can control our receive buffer size to help increase
         // chance of server blocking
         s.set_option(boost::asio::socket_base::receive_buffer_size(8*1024));
         boost::asio::connect(s, resolver.resolve("127.0.0.1", port));
         boost::beast::http::request<boost::beast::http::empty_body> req(boost::beast::http::verb::get, "/4megabyte", 11);
         req.keep_alive(true);
         req.set(http::field::host, endpoint);
         boost::beast::http::write(s, req);
      }
   };

   auto drain_http_replies = [&](unsigned max = std::numeric_limits<unsigned>::max()) {
      std::unordered_map<boost::beast::http::status, size_t> count_of_status_replies;
      while(connections.size() && max--) {
         boost::beast::http::response<boost::beast::http::string_body> resp;
         boost::beast::flat_buffer buffer;
         boost::beast::http::read(connections.front(), buffer, resp);

         count_of_status_replies[resp.result()]++;

         connections.erase(connections.begin());
      }
      return count_of_status_replies;
   };

   auto wait_for_no_bytes_in_flight = [&](uint16_t max = std::numeric_limits<uint16_t>::max()) {
      // bytes_in_flight is only increased when sending a response, need to make sure all requests are done
      // so that we can then verify no bytes in flight. If we checked bytes_in_flight in the while loop then it is
      // possible we can catch it with 0 bytes in flight even though there are requests still being processed that
      // will shortly increase the bytes in flight when they respond to the request.
      while (http_plugin->requests_in_flight() > 0 && --max)
         std::this_thread::sleep_for(std::chrono::milliseconds(5));
      BOOST_CHECK(max > 0);
      BOOST_CHECK(http_plugin->bytes_in_flight() == 0);
   };

   auto wait_for_requests = [&](uint16_t num_requests, uint16_t max = std::numeric_limits<uint16_t>::max()) {
      while (http_plugin->requests_in_flight() < num_requests && --max)
         std::this_thread::sleep_for(std::chrono::milliseconds(5));
      BOOST_CHECK(max > 0);
   };

   //send a single request to start with
   send_4mb_requests(1u);
   std::unordered_map<boost::beast::http::status, size_t> r = drain_http_replies();
   BOOST_REQUIRE_EQUAL(r[boost::beast::http::status::ok], 1u);

   //load up 32, this should exceed max
   send_4mb_requests(32u);
   r = drain_http_replies();
   BOOST_REQUIRE_GT(r[boost::beast::http::status::ok], 0u);
   BOOST_REQUIRE_GT(r[boost::beast::http::status::service_unavailable], 0u);
   BOOST_REQUIRE_EQUAL(r[boost::beast::http::status::service_unavailable] + r[boost::beast::http::status::ok], 32u);

   //send some more requests
   send_4mb_requests(10u);
   r = drain_http_replies();
   BOOST_REQUIRE_EQUAL(r[boost::beast::http::status::ok], 10u);

   //load up some more requests that exceed max
   send_4mb_requests(32u);
   //make sure got to the point http threads had responses queued
   wait_for_requests(32u);
   //now rip these connections out before the responses are completely sent
   connections.clear();
   wait_for_no_bytes_in_flight();
   //send some requests that should work still
   send_4mb_requests(8u);
   r = drain_http_replies();
   for (const auto& e : r) {
      ilog( "response: {}, count: {}", std::string(obsolete_reason(e.first)), e.second );
   }
   BOOST_REQUIRE_EQUAL(r[boost::beast::http::status::ok], 8u);
}

// Regression test for request bodies queued to the app thread being accounted against the bytes_in_flight
// budget. The handler samples the budget while the request body is still in flight (after admission/reservation
// but before the response payload is accounted), so the sampled value reflects only the request-body
// reservation. Without request-body accounting that sample is 0; with it, the in-flight body is counted. The
// budget must also return to zero after the request completes, proving the reservation is released exactly once.
BOOST_FIXTURE_TEST_CASE(request_body_bytes_in_flight, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", request_body_bytes_in_flight_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(request_body_bytes_in_flight_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-max-bytes-in-flight-mb=64"});
   BOOST_REQUIRE(http_plugin);

   std::atomic<size_t> observed_body_size{0};
   std::atomic<size_t> observed_bytes_in_flight{0};
   http_plugin->add_api({{std::string("/echo_body"), api_category::node,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             // Sampled on the app thread while the request body is still in flight and before the
                             // response payload is accounted, so it reflects only the request-body reservation.
                             observed_body_size.store(body.size());
                             observed_bytes_in_flight.store(http_plugin->bytes_in_flight());
                             cb(200, "ok");
                          }}}, appbase::exec_queue::read_write);

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   // 1 MiB body: comfortably under the 2 MiB max-body-size and the 64 MiB in-flight budget.
   const size_t body_size = 1024 * 1024;

   auto post_body = [&]() {
      boost::asio::ip::tcp::socket s(ctx, boost::asio::ip::tcp::v4());
      boost::asio::connect(s, resolver.resolve("127.0.0.1", port));
      boost::beast::http::request<boost::beast::http::string_body> req(boost::beast::http::verb::post, "/echo_body", 11);
      req.keep_alive(true);
      req.set(http::field::host, endpoint);
      req.body() = std::string(body_size, 'x');
      req.prepare_payload();
      boost::beast::http::write(s, req);

      boost::beast::http::response<boost::beast::http::string_body> resp;
      boost::beast::flat_buffer buffer;
      boost::beast::http::read(s, buffer, resp);
      return resp.result();
   };

   auto wait_for_no_bytes_in_flight = [&]() {
      uint16_t max = std::numeric_limits<uint16_t>::max();
      while (http_plugin->requests_in_flight() > 0 && --max)
         std::this_thread::sleep_for(std::chrono::milliseconds(5));
      BOOST_CHECK(max > 0);
      BOOST_CHECK_EQUAL(http_plugin->bytes_in_flight(), 0u);
   };

   BOOST_REQUIRE(post_body() == boost::beast::http::status::ok);
   // The handler saw the full request body...
   BOOST_CHECK_EQUAL(observed_body_size.load(), body_size);
   // ...and the body was reserved against bytes_in_flight while in flight. This is the regression assertion:
   // it reads 0 on the unfixed code (request bodies uncounted) and >= body_size with the fix.
   BOOST_CHECK_GE(observed_bytes_in_flight.load(), body_size);
   // The reservation is released once the request completes.
   wait_for_no_bytes_in_flight();

   // A second request must be admitted and counted the same way: no leaked or double-counted reservation.
   observed_bytes_in_flight.store(0);
   BOOST_REQUIRE(post_body() == boost::beast::http::status::ok);
   BOOST_CHECK_GE(observed_bytes_in_flight.load(), body_size);
   wait_for_no_bytes_in_flight();
}

BOOST_FIXTURE_TEST_CASE(requests_in_flight, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", requests_in_flight_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(requests_in_flight_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-max-in-flight-requests=16"});
   BOOST_REQUIRE(http_plugin);

   http_plugin->add_api({{std::string("/doit"), api_category::node,
                          [&](string&&, string&& body, url_response_callback&& cb) {
                             cb(200, "hello");
                          }}}, appbase::exec_queue::read_write);

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   std::list<boost::asio::ip::tcp::socket> connections;

   auto send_requests = [&](unsigned count) {
      for(unsigned i = 0; i < count; ++i) {
         boost::asio::ip::tcp::socket& s = connections.emplace_back(ctx, boost::asio::ip::tcp::v4());
         boost::asio::connect(s, resolver.resolve("127.0.0.1", port));
         boost::beast::http::request<boost::beast::http::empty_body> req(boost::beast::http::verb::get, "/doit", 11);
         req.keep_alive(true);
         req.set(http::field::host, endpoint);
         boost::beast::http::write(s, req);
      }
   };

   auto scan_http_replies = [&]() {
      std::unordered_map<boost::beast::http::status, size_t> count_of_status_replies;
      for(boost::asio::ip::tcp::socket& c : connections) {
         boost::beast::http::response<boost::beast::http::string_body> resp;
         boost::beast::flat_buffer buffer;
         boost::beast::http::read(c, buffer, resp);

         count_of_status_replies[resp.result()]++;

         if(resp.result() == boost::beast::http::status::ok)
            BOOST_REQUIRE(resp.keep_alive());
      }
      return count_of_status_replies;
   };

   auto wait_for_no_requests_in_flight = [&](uint16_t max = std::numeric_limits<uint16_t>::max()) {
      while (http_plugin->requests_in_flight() > 0 && --max)
         std::this_thread::sleep_for(std::chrono::milliseconds(5));
      BOOST_CHECK(max > 0);
   };

   //8 requests to start with
   send_requests(8u);
   std::unordered_map<boost::beast::http::status, size_t> r = scan_http_replies();
   BOOST_REQUIRE_EQUAL(r[boost::beast::http::status::ok], 8u);
   connections.clear();
   wait_for_no_requests_in_flight();

   //24 requests will exceed threshold
   send_requests(24u);
   r = scan_http_replies();
   BOOST_REQUIRE_GT(r[boost::beast::http::status::ok], 0u);
   BOOST_REQUIRE_GT(r[boost::beast::http::status::service_unavailable], 0u);
   BOOST_REQUIRE_EQUAL(r[boost::beast::http::status::service_unavailable] + r[boost::beast::http::status::ok], 24u);
   connections.clear();
   wait_for_no_requests_in_flight();

   //requests should still work
   send_requests(8u);
   r = scan_http_replies();
   for (const auto& e : r) {
      ilog( "response: {}, count: {}", std::string(obsolete_reason(e.first)), e.second );
   }
   BOOST_REQUIRE_EQUAL(r[boost::beast::http::status::ok], 8u);
   connections.clear();
   wait_for_no_requests_in_flight();
}

// A plaintext handler's string response is sent as-is; an error object it reports is sent as JSON with its own status,
// not failed as a 500 on a closed connection.
BOOST_FIXTURE_TEST_CASE(plaintext_responses, http_plugin_test_fixture) {
   const std::string endpoint       = test_http_endpoint("127.0.0.1", plaintext_response_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string plaintext_body = "metric 1\n";

   http_plugin* http_plugin = init({bu::framework::current_test_case().p_name->c_str(), server_address.c_str(),
                                    "--http-validate-host", "false"});
   BOOST_REQUIRE(http_plugin);

   http_plugin->add_async_api({{std::string("/v1/node/plaintext_ok"), api_category::node,
                                [plaintext_body](string&&, string&&, url_response_callback&& cb) {
                                   cb(200, fc::variant(plaintext_body));
                                }},
                               {std::string("/v1/node/plaintext_error"), api_category::node,
                                [](string&&, string&&, url_response_callback&& cb) {
                                   const auto error =
                                      fc::mutable_variant_object()("code", 400)("message", "Bad Request");
                                   cb(400, fc::variant(error));
                                }},
                               {std::string("/v1/node/plaintext_empty"), api_category::node,
                                [](string&&, string&&, url_response_callback&& cb) { cb(200, std::nullopt); }}},
                              http_content_type::plaintext);

   {
      http_response_for resp(endpoint.c_str(), "/v1/node/plaintext_ok");
      BOOST_CHECK_EQUAL(resp.status(), http::status::ok);
      BOOST_CHECK_EQUAL(std::string(resp.response[http::field::content_type]), "text/plain");
      BOOST_CHECK_EQUAL(resp.body(), plaintext_body);
   }
   {
      http_response_for resp(endpoint.c_str(), "/v1/node/plaintext_error");
      BOOST_CHECK_EQUAL(resp.status(), http::status::bad_request);
      BOOST_CHECK_EQUAL(std::string(resp.response[http::field::content_type]), "application/json");
      const auto body = fc::json::from_string(resp.body());
      BOOST_CHECK_EQUAL(body["code"].as_int64(), 400);
   }
   {
      http_response_for resp(endpoint.c_str(), "/v1/node/plaintext_empty");
      BOOST_CHECK_EQUAL(resp.status(), http::status::ok);
      BOOST_CHECK_EQUAL(std::string(resp.response[http::field::content_type]), "application/json");
      BOOST_CHECK_EQUAL(resp.body(), "{}");
   }
}

/// Turns logging off until destroyed.
struct quiet_log {
   const fc::log_level level = fc::logger::default_logger().get_log_level();
   quiet_log() { fc::logger::default_logger().set_log_level(fc::log_level::off); }
   ~quiet_log() { fc::logger::default_logger().set_log_level(level); }
};

/// Keeps logging off until the application has stopped: a session logs every exception it reports.
struct quiet_http_plugin_test_fixture : quiet_log, http_plugin_test_fixture {
   static constexpr auto http_thread_route = "/v1/node/throw_on_http_thread";
   static constexpr auto app_thread_route = "/v1/node/throw_on_app_thread";
   static constexpr auto metrics_route = "/v1/node/throw_in_metrics_observer";
   static constexpr std::array failing_routes{http_thread_route, app_thread_route, metrics_route};

   fc::temp_directory directory;
   const std::filesystem::path socket = directory.path() / "http-exception.sock";

   /// Add the failing routes, which throw `message`: from the handler on an http thread or the app thread, or from the
   /// metrics observer before the handler runs.
   void add_failing_routes(http_plugin& plugin, const std::string& message) {
      const auto fail = [message](string&&, string&&, url_response_callback&&) { throw std::runtime_error(message); };
      const auto succeed = [](string&&, string&&, url_response_callback&& cb) { cb(200, fc::variant("unreached")); };
      plugin.add_async_api(
         {{http_thread_route, api_category::node, fail}, {metrics_route, api_category::node, succeed}});
      plugin.add_api({{app_thread_route, api_category::node, fail}}, appbase::exec_queue::read_write);
      plugin.register_update_metrics([message](http_plugin::metrics metrics) {
         if (metrics.target == metrics_route)
            throw std::runtime_error(message);
      });
   }

   /// Whether every session has ended and released its bytes in flight, waiting a while for the last to finish.
   static bool all_released(const http_plugin& plugin) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (plugin.requests_in_flight() != 0 || plugin.bytes_in_flight() != 0) {
         if (std::chrono::steady_clock::now() > deadline)
            return false;
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return true;
   }

   /// Listen on the unix socket with default options.
   http_plugin* init_on_socket() {
      return init({bu::framework::current_test_case().p_name->c_str(), "--data-dir", directory.path().c_str(),
                   "--http-server-address", "", "--unix-socket-path", socket.c_str()});
   }
};

/// Requests sent one after another on one unix-socket connection, each once the previous response is in, given up
/// after a deadline. With `read_past`, it also reads past the last response.
struct unix_exchange {
   std::vector<http::response<http::string_body>> responses;
   beast::error_code error; ///< why a response did not arrive whole, if one did not
   beast::error_code after; ///< result of reading past the last response; unset if error or not read

   unix_exchange(const std::filesystem::path& socket, std::vector<const char*> routes, unsigned version = 11,
                 bool read_past = true) {
      net::io_context ioc;
      unix_stream stream(ioc);
      beast::flat_buffer buffer;
      http::request<http::string_body> request;
      http::response<http::string_body> next;
      std::function<void(size_t)> send = [&](size_t i) {
         if (i == routes.size()) {
            if (read_past)
               http::async_read(stream, buffer, next, [&](beast::error_code past_ec, size_t) { after = past_ec; });
            return;
         }
         request = http::request<http::string_body>{http::verb::post, routes[i], version};
         http::async_write(stream, request, [&, i](beast::error_code write_ec, size_t) {
            error = write_ec;
            if (error)
               return;
            http::async_read(stream, buffer, responses.emplace_back(), [&, i](beast::error_code read_ec, size_t) {
               error = read_ec;
               if (!error)
                  send(i + 1);
            });
         });
      };
      stream.expires_after(std::chrono::seconds(30));
      stream.async_connect(net::local::stream_protocol::endpoint(socket.string()), [&](beast::error_code connect_ec) {
         error = connect_ec;
         if (!error)
            send(0);
      });
      ioc.run();
   }

   unix_exchange(const std::filesystem::path& socket, const char* route, unsigned version = 11)
      : unix_exchange(socket, std::vector{route}, version) {}

   /// The first response.
   const http::response<http::string_body>& response() const { return responses.front(); }

   /// A whole 500 response, then end of stream.
   bool reported_and_closed() const {
      return !error && responses.size() == 1 && response().result() == http::status::internal_server_error &&
             after == http::error::end_of_stream;
   }
};

/// An exception response is written whole before its connection closes, wherever the request failed, and carries the
/// configured Server header.
BOOST_FIXTURE_TEST_CASE(exception_response_is_complete_before_close, quiet_http_plugin_test_fixture) {
   // Far more than a socket send buffer holds, so the response cannot be written in one non-blocking send.
   constexpr size_t message_size = 1024 * 1024;
   const std::string message(message_size, 'x');
   // No response deadline and verbose errors, so the response carries the whole message whatever the load.
   auto* plugin = init({bu::framework::current_test_case().p_name->c_str(), "--data-dir", directory.path().c_str(),
                        "--http-server-address", "", "--unix-socket-path", socket.c_str(),
                        "--http-max-response-time-ms", "-1", "--verbose-http-errors"});
   BOOST_REQUIRE(plugin);
   add_failing_routes(*plugin, message);

   for (const auto* route : failing_routes) {
      for (const unsigned version : {10u, 11u}) {
         const unix_exchange exchange(socket, route, version);
         BOOST_REQUIRE_MESSAGE(!exchange.error, route << " HTTP " << version << ": " << exchange.error.message());
         BOOST_CHECK(exchange.response().result() == http::status::internal_server_error);
         BOOST_CHECK(!exchange.response().keep_alive());
         BOOST_CHECK(exchange.response().body().find(message) != std::string::npos);
         BOOST_CHECK_EQUAL(std::string(exchange.response()[http::field::server]), http_plugin::get_server_header());
         BOOST_CHECK_MESSAGE(exchange.after == http::error::end_of_stream, exchange.after.message());
      }
   }
   BOOST_CHECK(all_released(*plugin));
}

/// Requests failing at once on several threads each get their whole error response on a connection that then closes.
BOOST_FIXTURE_TEST_CASE(concurrent_exception_responses, quiet_http_plugin_test_fixture) {
   constexpr uint32_t clients = 8;
   constexpr uint32_t requests_per_client = 64;
   auto* plugin = init({bu::framework::current_test_case().p_name->c_str(), "--data-dir", directory.path().c_str(),
                        "--http-server-address", "", "--unix-socket-path", socket.c_str(), "--http-threads", "4"});
   BOOST_REQUIRE(plugin);
   add_failing_routes(*plugin, "failed");

   std::atomic<uint32_t> reported{0};
   std::mutex failure_mutex;
   std::string first_failure; ///< what the first failed exchange got, as the server's logs are off
   std::vector<std::thread> threads;
   for (uint32_t client = 0; client < clients; ++client)
      threads.emplace_back([&, client] {
         for (uint32_t request = 0; request < requests_per_client; ++request) {
            const auto* route = failing_routes[(client + request) % failing_routes.size()];
            const unix_exchange exchange(socket, route);
            if (!exchange.reported_and_closed()) {
               std::ostringstream failure;
               failure << route << ": error '" << exchange.error.message() << "', " << exchange.responses.size()
                       << " response(s)";
               if (!exchange.responses.empty())
                  failure << ", the first " << exchange.response().result_int();
               failure << ", then '" << exchange.after.message() << "'";
               const std::lock_guard lock(failure_mutex);
               if (first_failure.empty())
                  first_failure = failure.str();
               return;
            }
            ++reported;
         }
      });
   for (auto& thread : threads)
      thread.join();
   BOOST_CHECK_MESSAGE(reported.load() == clients * requests_per_client,
                       reported.load() << " of " << clients * requests_per_client << " reported; " << first_failure);
   BOOST_CHECK(all_released(*plugin));
}

/// A request is answered once even when its handler responds and then throws or reports an exception, or responds
/// twice: the connection carries only the first response and goes on to serve the next request.
BOOST_FIXTURE_TEST_CASE(each_request_is_answered_once, quiet_http_plugin_test_fixture) {
   constexpr auto respond_then_throw_http = "/v1/node/respond_then_throw_on_http_thread";
   constexpr auto respond_then_throw_app = "/v1/node/respond_then_throw_on_app_thread";
   constexpr auto respond_twice = "/v1/node/respond_twice";
   constexpr auto raw_respond_then_throw = "/v1/node/raw_respond_then_throw";
   constexpr auto raw_respond_then_report = "/v1/node/raw_respond_then_report";
   constexpr auto raw_respond_twice = "/v1/node/raw_respond_twice";
   constexpr auto hello = "/v1/node/hello";
   auto* plugin = init_on_socket();
   BOOST_REQUIRE(plugin);
   const auto respond_then_throw = [](string&&, string&&, url_response_callback&& cb) {
      cb(200, fc::variant("first"));
      throw std::runtime_error("after responding");
   };
   plugin->add_async_api({{respond_then_throw_http, api_category::node, respond_then_throw},
                          {respond_twice, api_category::node,
                           [](string&&, string&&, url_response_callback&& cb) {
                              cb(200, fc::variant("first"));
                              cb(200, fc::variant("second"));
                           }},
                          {hello, api_category::node,
                           [](string&&, string&&, url_response_callback&& cb) { cb(200, fc::variant("world")); }}});
   plugin->add_api({{respond_then_throw_app, api_category::node, respond_then_throw}}, appbase::exec_queue::read_write);
   plugin->add_raw_handler(raw_respond_then_throw, api_category::node,
                           [](detail::abstract_conn_ptr conn, string&&, string&&) {
                              conn->send_response(R"("first")", 200);
                              throw std::runtime_error("after responding");
                           });
   plugin->add_raw_handler(raw_respond_then_report, api_category::node,
                           [](detail::abstract_conn_ptr conn, string&&, string&&) {
                              conn->send_response(R"("first")", 200);
                              try {
                                 throw std::runtime_error("after responding");
                              } catch (...) {
                                 conn->handle_exception();
                              }
                           });
   plugin->add_raw_handler(raw_respond_twice, api_category::node,
                           [](detail::abstract_conn_ptr conn, string&&, string&&) {
                              conn->send_response(R"("first")", 200);
                              conn->send_response(R"("second")", 200);
                           });

   for (const auto* route : {respond_then_throw_http, respond_then_throw_app, respond_twice, raw_respond_then_throw,
                             raw_respond_then_report, raw_respond_twice}) {
      const unix_exchange exchange(socket, {route, hello}, 11, false);
      BOOST_REQUIRE_MESSAGE(!exchange.error, route << ": " << exchange.error.message());
      BOOST_REQUIRE_EQUAL(exchange.responses.size(), 2u);
      BOOST_CHECK(exchange.responses[0].result() == http::status::ok);
      BOOST_CHECK_EQUAL(exchange.responses[0].body(), R"("first")");
      BOOST_CHECK_EQUAL(exchange.responses[1].body(), R"("world")");
   }
}

/// A raw handler's exception, thrown, reported through its connection or raised by a send, is answered like any API
/// call's: a malformed request is a 400, any other failure a 500, and the connection stays open. Once it closes, no
/// request or byte is left in flight.
BOOST_FIXTURE_TEST_CASE(raw_handler_exceptions_are_api_errors, quiet_http_plugin_test_fixture) {
   constexpr auto invalid_request = "/v1/node/raw_invalid_request";
   constexpr auto reported_invalid_request = "/v1/node/raw_reported_invalid_request";
   constexpr auto internal_error = "/v1/node/raw_internal_error";
   constexpr auto send_throws = "/v1/node/raw_send_throws";
   constexpr auto hello = "/v1/node/hello";
   auto* plugin = init_on_socket();
   BOOST_REQUIRE(plugin);
   plugin->add_raw_handler(invalid_request, api_category::node, [](detail::abstract_conn_ptr, string&&, string&&) {
      SYS_THROW(chain::invalid_http_request, "malformed request");
   });
   plugin->add_raw_handler(reported_invalid_request, api_category::node,
                           [](detail::abstract_conn_ptr conn, string&&, string&&) {
                              try {
                                 SYS_THROW(chain::invalid_http_request, "malformed request");
                              } catch (...) {
                                 conn->handle_exception();
                              }
                           });
   plugin->add_raw_handler(internal_error, api_category::node, [](detail::abstract_conn_ptr, string&&, string&&) {
      throw std::runtime_error("broken");
   });
   plugin->add_raw_handler(send_throws, api_category::node, [](detail::abstract_conn_ptr conn, string&&, string&&) {
      conn->send_response("a body a 204 cannot carry", 204);
   });
   plugin->add_async_api({{hello, api_category::node, [](string&&, string&&, url_response_callback&& cb) {
                              cb(200, fc::variant("world"));
                           }}});

   const std::vector<std::pair<const char*, http::status>> cases{
      {invalid_request, http::status::bad_request},
      {reported_invalid_request, http::status::bad_request},
      {internal_error, http::status::internal_server_error},
      {send_throws, http::status::internal_server_error}};
   for (const auto& [route, status] : cases) {
      const unix_exchange exchange(socket, {route, hello}, 11, false);
      BOOST_REQUIRE_MESSAGE(!exchange.error, route << ": " << exchange.error.message());
      BOOST_REQUIRE_EQUAL(exchange.responses.size(), 2u);
      BOOST_CHECK(exchange.responses[0].result() == status);
      BOOST_CHECK(exchange.responses[0].keep_alive());
      BOOST_CHECK_EQUAL(fc::json::from_string(exchange.responses[0].body())["code"].as_uint64(),
                        magic_enum::enum_integer(status));
      BOOST_CHECK(exchange.responses[1].result() == http::status::ok);
      BOOST_CHECK_EQUAL(exchange.responses[1].body(), R"("world")");
      // Receiving a response does not mean its server callback has returned: it can still hold an accounted
      // payload while another HTTP thread handles the next request. Check for leaks after the connection closes.
      const bool released = all_released(*plugin);
      BOOST_REQUIRE_MESSAGE(released, route << ": " << plugin->requests_in_flight() << " requests and "
                                            << plugin->bytes_in_flight() << " bytes remain in flight");
   }
}

namespace {
   /// Minimal api handle for driving bind_stream's stored-exception delivery paths end-to-end.
   /// Both methods deliver a DERIVED exception through the stored fc::exception_ptr alternative
   /// (never a live throw), which is the path where a `throw *ptr` would slice the dynamic type
   /// and defeat classify_current_exception's specific-type -> status-code mapping.
   struct stored_exception_api {
      /// dispatch::async -- stored tx_duplicate must classify as 409 Conflict.
      void duplicate_trx(sysio::chain::plugin_interface::next_function<int> next) {
         next(sysio::chain::plugin_interface::next_function_variant<int>{
            fc::exception_ptr{std::make_shared<sysio::chain::tx_duplicate>(
               FC_LOG_MESSAGE(error, "duplicate transaction"))}});
      }

      /// dispatch::post -- Phase-2 call returning a stored unknown_block_exception must
      /// classify as 400 Unknown Block.  Captures nothing, hence a zero retained size.
      sysio::chain::deferred_call<int> missing_block() {
         return sysio::chain::deferred_call<int>{
            []() -> sysio::chain::t_or_exception<int> {
               return fc::exception_ptr{std::make_shared<sysio::chain::unknown_block_exception>(
                  FC_LOG_MESSAGE(error, "no such block"))};
            },
            0};
      }
   };
}

// Stored exceptions delivered through bind_stream (async next_function and post http_fwd
// alternatives) must reach classify_current_exception with their dynamic type intact so the
// specific-type catches map them to the right HTTP status. A regression to `throw *ptr`
// (slicing to fc::exception) turns both of these into 500 Internal Service Error.
BOOST_FIXTURE_TEST_CASE(stream_stored_exception_status_codes, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", stream_status_codes_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(stream_status_codes_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin", server_address.c_str()});
   BOOST_REQUIRE(http_plugin);

   http_plugin->add_api_stream({
      bind_stream<&stored_exception_api::duplicate_trx, dispatch::async>(
         *http_plugin, stored_exception_api{}, "/v1/test/duplicate_trx",
         api_category::node, http_params_types::no_params, 200),
      bind_stream<&stored_exception_api::missing_block, dispatch::post>(
         *http_plugin, stored_exception_api{}, "/v1/test/missing_block",
         api_category::node, http_params_types::no_params, 200),
   }, appbase::exec_queue::read_write);

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   auto get_status = [&](const char* path) {
      boost::asio::ip::tcp::socket s(ctx, boost::asio::ip::tcp::v4());
      boost::asio::connect(s, resolver.resolve("127.0.0.1", port));
      boost::beast::http::request<boost::beast::http::empty_body> req(boost::beast::http::verb::get, path, 11);
      req.set(http::field::host, endpoint);
      boost::beast::http::write(s, req);

      boost::beast::http::response<boost::beast::http::string_body> resp;
      boost::beast::flat_buffer buffer;
      boost::beast::http::read(s, buffer, resp);
      return resp.result();
   };

   BOOST_CHECK(get_status("/v1/test/duplicate_trx") == boost::beast::http::status::conflict);     // 409
   BOOST_CHECK(get_status("/v1/test/missing_block") == boost::beast::http::status::bad_request);  // 400
}

// Streaming-path twin of request_body_bytes_in_flight: request bodies queued to the app thread
// through the streaming-cb registration path (make_app_thread_url_handler_stream) must take the
// same bytes_in_flight reservation the variant path takes. Without it the sampled value below
// reads 0 (queued streaming bodies uncounted -- the WSA-176 vector); with it, >= body size. The
// budget must also drain to zero afterwards, proving the reservation releases exactly once.
BOOST_FIXTURE_TEST_CASE(request_body_bytes_in_flight_stream, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", stream_request_body_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(stream_request_body_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-max-bytes-in-flight-mb=64"});
   BOOST_REQUIRE(http_plugin);

   std::atomic<size_t> observed_body_size{0};
   std::atomic<size_t> observed_bytes_in_flight{0};
   http_plugin->add_api_stream({{std::string("/echo_body_stream"), api_category::node,
                                 [&](string&&, string&& body, url_response_stream_callback&& cb) {
                                    // Sampled on the app thread while the request body is still in
                                    // flight and before any response bytes are accounted, so it
                                    // reflects only the request-body reservation.
                                    observed_body_size.store(body.size());
                                    observed_bytes_in_flight.store(http_plugin->bytes_in_flight());
                                    cb(200, [](fc::json_writer& w) { w.value_string("ok"); });
                                 }}}, appbase::exec_queue::read_write);

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   // 1 MiB body: comfortably under the 2 MiB max-body-size and the 64 MiB in-flight budget.
   const size_t body_size = 1024 * 1024;

   auto post_body = [&]() {
      boost::asio::ip::tcp::socket s(ctx, boost::asio::ip::tcp::v4());
      boost::asio::connect(s, resolver.resolve("127.0.0.1", port));
      boost::beast::http::request<boost::beast::http::string_body> req(boost::beast::http::verb::post, "/echo_body_stream", 11);
      req.keep_alive(true);
      req.set(http::field::host, endpoint);
      req.body() = std::string(body_size, 'x');
      req.prepare_payload();
      boost::beast::http::write(s, req);

      boost::beast::http::response<boost::beast::http::string_body> resp;
      boost::beast::flat_buffer buffer;
      boost::beast::http::read(s, buffer, resp);
      return resp.result();
   };

   auto wait_for_no_bytes_in_flight = [&]() {
      uint16_t max = std::numeric_limits<uint16_t>::max();
      while (http_plugin->requests_in_flight() > 0 && --max)
         std::this_thread::sleep_for(std::chrono::milliseconds(5));
      BOOST_CHECK(max > 0);
      BOOST_CHECK_EQUAL(http_plugin->bytes_in_flight(), 0u);
   };

   BOOST_REQUIRE(post_body() == boost::beast::http::status::ok);
   BOOST_CHECK_EQUAL(observed_body_size.load(), body_size);
   // Regression assertion: 0 without the reservation in make_app_thread_url_handler_stream.
   BOOST_CHECK_GE(observed_bytes_in_flight.load(), body_size);
   wait_for_no_bytes_in_flight();

   // A second request must be admitted and counted the same way: no leaked or double-counted
   // reservation.
   observed_bytes_in_flight.store(0);
   BOOST_REQUIRE(post_body() == boost::beast::http::status::ok);
   BOOST_CHECK_GE(observed_bytes_in_flight.load(), body_size);
   wait_for_no_bytes_in_flight();
}

// Streaming responses must be charged against bytes_in_flight WHILE the emitter runs, not only
// after the body is fully materialized: json_writer's growth guard settles the buffer into the
// budget every stride. The emitter below samples the budget mid-emission; without incremental
// accounting the sampled value reads 0 (the response was invisible to the budget until complete).
BOOST_FIXTURE_TEST_CASE(stream_response_bytes_in_flight, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", stream_response_bytes_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(stream_response_bytes_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-max-bytes-in-flight-mb=64"});
   BOOST_REQUIRE(http_plugin);

   // 32 x 64KiB chunks ~= 2 MiB emitted; sample the budget halfway through emission.
   constexpr size_t chunk_size  = 64 * 1024;
   constexpr size_t chunk_count = 32;
   std::atomic<size_t> observed_bytes_in_flight{0};
   http_plugin->add_api_stream({{std::string("/2megabyte_stream"), api_category::node,
                                 [&](string&&, string&&, url_response_stream_callback&& cb) {
                                    cb(200, [&](fc::json_writer& w) {
                                       w.begin_array();
                                       for (size_t i = 0; i < chunk_count; ++i) {
                                          w.value_string(std::string(chunk_size, 'x'));
                                          if (i == chunk_count / 2)
                                             observed_bytes_in_flight.store(http_plugin->bytes_in_flight());
                                       }
                                       w.end_array();
                                    });
                                 }}}, appbase::exec_queue::read_write);

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   auto get_response = [&]() {
      boost::asio::ip::tcp::socket s(ctx, boost::asio::ip::tcp::v4());
      boost::asio::connect(s, resolver.resolve("127.0.0.1", port));
      boost::beast::http::request<boost::beast::http::empty_body> req(boost::beast::http::verb::get, "/2megabyte_stream", 11);
      req.set(http::field::host, endpoint);
      boost::beast::http::write(s, req);

      boost::beast::http::response<boost::beast::http::string_body> resp;
      boost::beast::flat_buffer buffer;
      boost::beast::http::read(s, buffer, resp);
      return resp;
   };

   auto resp = get_response();
   BOOST_REQUIRE(resp.result() == boost::beast::http::status::ok);
   BOOST_CHECK_GT(resp.body().size(), chunk_size * chunk_count);
   // Regression assertion: 0 without the growth guard's incremental settle. At the halfway
   // sample ~1 MiB has been emitted; charging lags by at most one stride, so half that is a
   // safely conservative floor.
   BOOST_CHECK_GE(observed_bytes_in_flight.load(), chunk_size * chunk_count / 4);

   uint16_t max = std::numeric_limits<uint16_t>::max();
   while (http_plugin->requests_in_flight() > 0 && --max)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   BOOST_CHECK(max > 0);
   BOOST_CHECK_EQUAL(http_plugin->bytes_in_flight(), 0u);
}

// Over-budget streaming responses must abort DURING emission (503 busy) instead of
// materializing in full and only then being rejected: the growth guard throws once
// verify_max_bytes_in_flight rejects the incrementally-charged buffer. The chunk counter
// proves early abort; the follow-up request proves the budget released and the plugin
// still serves.
BOOST_FIXTURE_TEST_CASE(stream_response_over_budget_aborts_early, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", stream_over_budget_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(stream_over_budget_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-max-bytes-in-flight-mb=1"});
   BOOST_REQUIRE(http_plugin);

   // Attempts 32 MiB against a 1 MiB budget; the guard must stop it within a stride or two
   // past the budget.
   constexpr size_t chunk_size  = 64 * 1024;
   constexpr size_t chunk_count = 512;
   std::atomic<size_t> chunks_emitted{0};
   http_plugin->add_api_stream({{std::string("/32megabyte_stream"), api_category::node,
                                 [&](string&&, string&&, url_response_stream_callback&& cb) {
                                    cb(200, [&](fc::json_writer& w) {
                                       w.begin_array();
                                       for (size_t i = 0; i < chunk_count; ++i) {
                                          w.value_string(std::string(chunk_size, 'x'));
                                          chunks_emitted.fetch_add(1);
                                       }
                                       w.end_array();
                                    });
                                 },
                                },
                                {std::string("/small_stream"), api_category::node,
                                 [&](string&&, string&&, url_response_stream_callback&& cb) {
                                    cb(200, [](fc::json_writer& w) { w.value_string("ok"); });
                                 }}}, appbase::exec_queue::read_write);

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   auto get_status = [&](const char* path) {
      boost::asio::ip::tcp::socket s(ctx, boost::asio::ip::tcp::v4());
      boost::asio::connect(s, resolver.resolve("127.0.0.1", port));
      boost::beast::http::request<boost::beast::http::empty_body> req(boost::beast::http::verb::get, path, 11);
      req.set(http::field::host, endpoint);
      boost::beast::http::write(s, req);

      boost::beast::http::response<boost::beast::http::string_body> resp;
      boost::beast::flat_buffer buffer;
      boost::beast::http::read(s, buffer, resp);
      return resp.result();
   };

   BOOST_CHECK(get_status("/32megabyte_stream") == boost::beast::http::status::service_unavailable);
   // Early abort: the guard threw within a couple of strides past the 1 MiB budget, far
   // short of the 512 chunks the emitter would produce unguarded.
   BOOST_CHECK_LT(chunks_emitted.load(), 64u);
   BOOST_CHECK_GT(chunks_emitted.load(), 0u);

   uint16_t max = std::numeric_limits<uint16_t>::max();
   while (http_plugin->requests_in_flight() > 0 && --max)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   BOOST_CHECK(max > 0);
   BOOST_CHECK_EQUAL(http_plugin->bytes_in_flight(), 0u);

   // The budget drained: a small streaming response is admitted and served normally.
   BOOST_CHECK(get_status("/small_stream") == boost::beast::http::status::ok);
}

// A budget abort stays a 503 whatever the emitter does with it: one emitter swallows it and
// finishes with a fallback small enough to fit (before the abort was sticky, a 200 with the
// fallback), the other rethrows it wrapped in an fc::exception (a 500).
BOOST_FIXTURE_TEST_CASE(stream_response_absorbed_abort_is_busy, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", stream_absorbed_abort_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(stream_absorbed_abort_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-max-bytes-in-flight-mb=1"});
   BOOST_REQUIRE(http_plugin);

   // 32 MiB attempted against the 1 MiB budget.
   constexpr size_t chunk_size  = 64 * 1024;
   constexpr size_t chunk_count = 512;
   auto emit_oversized = [](fc::json_writer& w) {
      for (size_t i = 0; i < chunk_count; ++i)
         w.value_string(std::string(chunk_size, 'x'));
   };
   http_plugin->add_api_stream({{std::string("/swallowed_abort"), api_category::node,
                                 [&](string&&, string&&, url_response_stream_callback&& cb) {
                                    cb(200, [&](fc::json_writer& w) {
                                       w.begin_array();
                                       const auto cp = w.checkpoint();
                                       try {
                                          emit_oversized(w);
                                       } catch (...) {
                                          w.rewind(cp); // fall back to an empty array
                                       }
                                       w.end_array();
                                    });
                                 }},
                                {std::string("/wrapped_abort"), api_category::node,
                                 [&](string&&, string&&, url_response_stream_callback&& cb) {
                                    cb(200, [&](fc::json_writer& w) {
                                       try {
                                          w.begin_array();
                                          emit_oversized(w);
                                          w.end_array();
                                       } FC_RETHROW_EXCEPTIONS(error, "wrapped serializer failure")
                                    });
                                 }}}, appbase::exec_queue::read_write);

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);
   auto get_status = [&](const char* path) {
      boost::asio::ip::tcp::socket s(ctx, boost::asio::ip::tcp::v4());
      boost::asio::connect(s, resolver.resolve("127.0.0.1", port));
      boost::beast::http::request<boost::beast::http::empty_body> req(boost::beast::http::verb::get, path, 11);
      req.set(http::field::host, endpoint);
      boost::beast::http::write(s, req);

      boost::beast::http::response<boost::beast::http::string_body> resp;
      boost::beast::flat_buffer buffer;
      boost::beast::http::read(s, buffer, resp);
      return resp.result();
   };

   BOOST_CHECK(get_status("/swallowed_abort") == boost::beast::http::status::service_unavailable);
   BOOST_CHECK(get_status("/wrapped_abort") == boost::beast::http::status::service_unavailable);

   uint16_t max = std::numeric_limits<uint16_t>::max();
   while (http_plugin->requests_in_flight() > 0 && --max)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   BOOST_CHECK(max > 0);
   BOOST_CHECK_EQUAL(http_plugin->bytes_in_flight(), 0u);
}

// One oversized token -- a single 32 MiB string value -- must abort mid-token against the
// budget: the growth guard fires inside the escape loop, so the emitter's post-token
// statement never runs. Distinguishes intra-token enforcement from token-boundary-only
// checks, which would materialize the full token first and only then reject (same 503, but
// token_completed would read true).
BOOST_FIXTURE_TEST_CASE(stream_response_single_token_over_budget, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", stream_single_token_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(stream_single_token_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-max-bytes-in-flight-mb=1"});
   BOOST_REQUIRE(http_plugin);

   std::atomic<bool> token_completed{false};
   http_plugin->add_api_stream({{std::string("/one_giant_token"), api_category::node,
                                 [&](string&&, string&&, url_response_stream_callback&& cb) {
                                    cb(200, [&](fc::json_writer& w) {
                                       w.value_string(std::string(32u * 1024 * 1024, 'x'));
                                       token_completed.store(true);
                                    });
                                 }}}, appbase::exec_queue::read_write);

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   auto get_status = [&](const char* path) {
      boost::asio::ip::tcp::socket s(ctx, boost::asio::ip::tcp::v4());
      boost::asio::connect(s, resolver.resolve("127.0.0.1", port));
      boost::beast::http::request<boost::beast::http::empty_body> req(boost::beast::http::verb::get, path, 11);
      req.set(http::field::host, endpoint);
      boost::beast::http::write(s, req);

      boost::beast::http::response<boost::beast::http::string_body> resp;
      boost::beast::flat_buffer buffer;
      boost::beast::http::read(s, buffer, resp);
      return resp.result();
   };

   BOOST_CHECK(get_status("/one_giant_token") == boost::beast::http::status::service_unavailable);
   BOOST_CHECK(!token_completed.load());

   uint16_t max = std::numeric_limits<uint16_t>::max();
   while (http_plugin->requests_in_flight() > 0 && --max)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   BOOST_CHECK(max > 0);
   BOOST_CHECK_EQUAL(http_plugin->bytes_in_flight(), 0u);
}

namespace {
   /// FC_REFLECT'd payload big enough that its packed-size reservation is unmistakable in
   /// the bytes_in_flight sample (and comfortably below the test budget).
   struct queued_payload_result {
      std::vector<char> data;
   };

   /// Minimal api handle for driving bind_stream's dispatch::sync queued-payload
   /// reservation end-to-end.  The method blocks (on the app thread) until the test has
   /// stalled the single http pool thread, so the emitter -- and the typed result it
   /// captures -- must sit queued behind the stall the moment it is produced.
   struct queued_payload_api {
      std::atomic<bool>* entered;
      std::atomic<bool>* proceed;
      size_t             payload_size;

      queued_payload_result make_big() {
         entered->store(true);
         while (!proceed->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
         return queued_payload_result{std::vector<char>(payload_size, 'q')};
      }
   };
}
FC_REFLECT(queued_payload_result, (data))

// Typed results captured into a stream_emitter and queued to the http pool must be charged
// against bytes_in_flight for the time they sit in the queue -- the variant path charges its
// in_flight_sizeof estimate before dispatch, and the streaming path must not be blinder.
// With --http-threads=1, /stall_stream parks the only pool thread inside a latched emitter
// while the bind_stream endpoint's result is produced on the app thread; the peak sampled
// below reads ~0 without the source-size reservation and >= the payload size with it.  The
// budget must then drain to zero, proving the reservation releases exactly once.
BOOST_FIXTURE_TEST_CASE(stream_queued_payload_reserved, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", stream_queued_payload_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(stream_queued_payload_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-threads=1",
                                    "--http-max-bytes-in-flight-mb=64"});
   BOOST_REQUIRE(http_plugin);

   constexpr size_t payload_size = 1024 * 1024;
   std::atomic<bool> entered{false};
   std::atomic<bool> proceed{false};
   std::atomic<bool> stalled{false};
   std::atomic<bool> release{false};

   http_plugin->add_api_stream({
      bind_stream<&queued_payload_api::make_big, dispatch::sync>(
         *http_plugin, queued_payload_api{&entered, &proceed, payload_size},
         "/v1/test/queued_payload", api_category::node, http_params_types::no_params, 200),
   }, appbase::exec_queue::read_write);

   // Async streaming handlers run inline on the pool thread that read the request, and the
   // response dispatch short-circuits on that same thread -- so this emitter's latch parks
   // the pool's only thread.
   http_plugin->add_async_api_stream({{std::string("/stall_stream"), api_category::node,
                                       [&](string&&, string&&, url_response_stream_callback&& cb) {
                                          cb(200, [&](fc::json_writer& w) {
                                             stalled.store(true);
                                             while (!release.load())
                                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                             w.value_string("ok");
                                          });
                                       }}});

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   auto send_request = [&](const char* path) {
      auto s = std::make_shared<boost::asio::ip::tcp::socket>(ctx, boost::asio::ip::tcp::v4());
      boost::asio::connect(*s, resolver.resolve("127.0.0.1", port));
      boost::beast::http::request<boost::beast::http::empty_body> req(boost::beast::http::verb::get, path, 11);
      req.set(http::field::host, endpoint);
      boost::beast::http::write(*s, req);
      return s;
   };
   auto read_response = [&](boost::asio::ip::tcp::socket& s) {
      boost::beast::http::response_parser<boost::beast::http::string_body> parser;
      parser.body_limit(16 * 1024 * 1024); // hex-encoded payload is ~2x its byte size
      boost::beast::flat_buffer buffer;
      boost::beast::http::read(s, buffer, parser);
      return parser.release();
   };
   auto wait_for = [](auto&& pred) { // ~10s cap: pool scheduling is slow under sanitizers
      for (int i = 0; i < 10000; ++i) {
         if (pred())
            return true;
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return pred();
   };

   auto payload_socket = send_request("/v1/test/queued_payload");
   BOOST_REQUIRE(wait_for([&] { return entered.load(); }));  // request read; app thread inside make_big
   auto stall_socket = send_request("/stall_stream");
   BOOST_REQUIRE(wait_for([&] { return stalled.load(); }));  // pool's only thread now parked
   proceed.store(true);                                      // result produced -> emitter queued behind the stall

   // Regression sample: reads ~0 without the reservation riding the queued emitter closure.
   size_t peak = 0;
   wait_for([&] {
      peak = std::max(peak, http_plugin->bytes_in_flight());
      return peak >= payload_size;
   });
   BOOST_CHECK_GE(peak, payload_size);

   release.store(true);
   auto stall_resp = read_response(*stall_socket);
   BOOST_CHECK(stall_resp.result() == boost::beast::http::status::ok);
   auto payload_resp = read_response(*payload_socket);
   BOOST_CHECK(payload_resp.result() == boost::beast::http::status::ok);
   BOOST_CHECK_GT(payload_resp.body().size(), 2 * payload_size); // hex-encoded vector<char>

   // requests_in_flight tracks session lifetime, so the keep-alive sockets must close
   // before the drain below can reach zero.
   stall_socket.reset();
   payload_socket.reset();

   uint16_t max = std::numeric_limits<uint16_t>::max();
   while (http_plugin->requests_in_flight() > 0 && --max)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   BOOST_CHECK(max > 0);
   BOOST_CHECK_EQUAL(http_plugin->bytes_in_flight(), 0u);
}

namespace {
   /// Minimal api handle for driving bind_stream's dispatch::post reservation end-to-end.
   /// Phase 1 "collects" raw bytes the way the real two-phase endpoints do (copied ABI blobs,
   /// table rows, a block, a trace) and hands them to the deferred call, which then has to
   /// wait on the http pool queue holding all of it.  Phase 1 blocks until the test has
   /// stalled the pool's only thread, so the call is guaranteed to be queued, never inlined.
   struct deferred_payload_api {
      std::atomic<bool>* entered;
      std::atomic<bool>* proceed;
      size_t             payload_size;

      sysio::chain::deferred_call<queued_payload_result> make_big_deferred() {
         std::vector<char> collected(payload_size, 'd');
         const size_t retained = collected.size();

         entered->store(true);
         while (!proceed->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

         return sysio::chain::deferred_call<queued_payload_result>{
            [collected = std::move(collected)]() mutable
               -> sysio::chain::t_or_exception<queued_payload_result> {
                  return queued_payload_result{std::move(collected)};
               },
            retained};
      }
   };
}

// The Phase-1 data a two-phase endpoint captures into its deferred call is charged for the whole
// time the call sits on the http thread pool queue.  That data is exactly what the two-phase split
// moved off the read-only queue -- copied ABI bytes, collected rows, a fetched block, a trace -- and
// it is invisible to the result-side estimate, since a type-erased callable cannot be measured.
// With --http-threads=1, /stall_stream parks the only pool thread while the deferred call is posted
// behind it; the peak sampled below reads ~0 without the reservation and >= the retained size with
// it.  The budget must then drain to zero, proving the reservation releases exactly once.
BOOST_FIXTURE_TEST_CASE(stream_deferred_payload_reserved, http_plugin_test_fixture) {
   const std::string endpoint = test_http_endpoint("127.0.0.1", stream_deferred_payload_index);
   const std::string server_address = "--http-server-address=" + endpoint;
   const std::string port = test_http_port(stream_deferred_payload_index);

   http_plugin* http_plugin = init({"--plugin=sysio::http_plugin",
                                    server_address.c_str(),
                                    "--http-threads=1",
                                    "--http-max-bytes-in-flight-mb=64"});
   BOOST_REQUIRE(http_plugin);

   constexpr size_t payload_size = 1024 * 1024;
   std::atomic<bool> entered{false};
   std::atomic<bool> proceed{false};
   std::atomic<bool> stalled{false};
   std::atomic<bool> release{false};

   http_plugin->add_api_stream({
      bind_stream<&deferred_payload_api::make_big_deferred, dispatch::post>(
         *http_plugin, deferred_payload_api{&entered, &proceed, payload_size},
         "/v1/test/deferred_payload", api_category::node, http_params_types::no_params, 200),
   }, appbase::exec_queue::read_write);

   http_plugin->add_async_api_stream({{std::string("/stall_stream"), api_category::node,
                                       [&](string&&, string&&, url_response_stream_callback&& cb) {
                                          cb(200, [&](fc::json_writer& w) {
                                             stalled.store(true);
                                             while (!release.load())
                                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                             w.value_string("ok");
                                          });
                                       }}});

   boost::asio::io_context ctx;
   boost::asio::ip::tcp::resolver resolver(ctx);

   auto send_request = [&](const char* path) {
      auto s = std::make_shared<boost::asio::ip::tcp::socket>(ctx, boost::asio::ip::tcp::v4());
      boost::asio::connect(*s, resolver.resolve("127.0.0.1", port));
      boost::beast::http::request<boost::beast::http::empty_body> req(boost::beast::http::verb::get, path, 11);
      req.set(http::field::host, endpoint);
      boost::beast::http::write(*s, req);
      return s;
   };
   auto read_response = [&](boost::asio::ip::tcp::socket& s) {
      boost::beast::http::response_parser<boost::beast::http::string_body> parser;
      parser.body_limit(16 * 1024 * 1024); // hex-encoded payload is ~2x its byte size
      boost::beast::flat_buffer buffer;
      boost::beast::http::read(s, buffer, parser);
      return parser.release();
   };
   auto wait_for = [](auto&& pred) { // ~10s cap: pool scheduling is slow under sanitizers
      for (int i = 0; i < 10000; ++i) {
         if (pred())
            return true;
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return pred();
   };

   auto payload_socket = send_request("/v1/test/deferred_payload");
   BOOST_REQUIRE(wait_for([&] { return entered.load(); }));  // request read; app thread inside Phase 1
   auto stall_socket = send_request("/stall_stream");
   BOOST_REQUIRE(wait_for([&] { return stalled.load(); }));  // pool's only thread now parked
   proceed.store(true);                                      // Phase 1 returns -> call queued behind the stall

   // Regression sample: reads ~0 without the reservation riding the posted lambda.
   size_t peak = 0;
   wait_for([&] {
      peak = std::max(peak, http_plugin->bytes_in_flight());
      return peak >= payload_size;
   });
   BOOST_CHECK_GE(peak, payload_size);

   release.store(true);
   auto stall_resp = read_response(*stall_socket);
   BOOST_CHECK(stall_resp.result() == boost::beast::http::status::ok);
   auto payload_resp = read_response(*payload_socket);
   BOOST_CHECK(payload_resp.result() == boost::beast::http::status::ok);
   BOOST_CHECK_GT(payload_resp.body().size(), 2 * payload_size); // hex-encoded vector<char>

   stall_socket.reset();
   payload_socket.reset();

   uint16_t max = std::numeric_limits<uint16_t>::max();
   while (http_plugin->requests_in_flight() > 0 && --max)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   BOOST_CHECK(max > 0);
   BOOST_CHECK_EQUAL(http_plugin->bytes_in_flight(), 0u);
}

//A warning for future tests: destruction of http_plugin_test_fixture sometimes does not destroy http_plugin's listeners. Tests
// added in the future should avoid reusing ports of other tests in http_plugin_unit_tests.

namespace {
/// How long a test waits for the server to do something it is expected to do soon.
constexpr std::chrono::seconds server_reaction_limit{30};

/// Poll `condition` until it holds or `limit` passes; whether it held.
bool wait_until(const std::function<bool()>& condition,
                std::chrono::steady_clock::duration limit = server_reaction_limit) {
   const auto deadline = std::chrono::steady_clock::now() + limit;
   while (!condition()) {
      if (std::chrono::steady_clock::now() > deadline)
         return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
   return true;
}

/// Read from `socket`, discarding what arrives, until the read fails or `limit` passes. The read's error, or
/// net::error::timed_out if the socket was still open after `limit`.
beast::error_code read_until_closed(net::io_context& ctx, tcp::socket& socket,
                                    std::chrono::steady_clock::duration limit = server_reaction_limit) {
   std::array<char, 4096> buffer;
   std::optional<beast::error_code> failed;
   std::function<void()> read = [&] {
      socket.async_read_some(net::buffer(buffer), [&](beast::error_code ec, size_t) {
         if (ec)
            failed = ec;
         else
            read();
      });
   };
   read();
   ctx.restart();
   ctx.run_for(limit);
   if (failed)
      return *failed;
   socket.cancel();
   ctx.restart();
   ctx.run(); // the cancelled read completes before its buffer goes out of scope
   return net::error::timed_out;
}

/// Whether a read error means the server closed the connection.
bool closed_by_server(const beast::error_code& ec) {
   return ec == net::error::eof || ec == net::error::connection_reset;
}

/// A client of the `/body_size` route, which answers with the size of the request body it received.
struct body_size_client {
   static constexpr auto route = "/body_size";

   net::io_context& ctx;
   std::string      endpoint;
   std::string      port;

   /// Add the route to `plugin`, on the app thread like the chain APIs.
   static void add_route(http_plugin& plugin) {
      plugin.add_api({{std::string(route), api_category::node,
                       [](string&&, string&& body, url_response_callback&& cb) { cb(200, fc::variant(body.size())); }}},
                     appbase::exec_queue::read_write);
   }

   tcp::socket connect() const {
      tcp::socket socket(ctx, tcp::v4());
      tcp::resolver resolver(ctx);
      net::connect(socket, resolver.resolve("127.0.0.1", port));
      return socket;
   }

   /// A POST header for the route, with `fields` (each ending in CRLF) added.
   std::string header(const std::string& fields) const {
      return std::string("POST ") + route + " HTTP/1.1\r\nHost: " + endpoint + "\r\n" + fields + "\r\n";
   }

   static std::string content_length(size_t size) { return "Content-Length: " + std::to_string(size) + "\r\n"; }

   static http::response<http::string_body> read_response(tcp::socket& socket) {
      http::response<http::string_body> response;
      beast::flat_buffer buffer;
      http::read(socket, buffer, response);
      return response;
   }
};
} // namespace

/// A request body is reserved against bytes_in_flight from the moment its header is read, so the budget bounds bodies
/// that are still arriving. A body that does not fit is refused with a 503 before any of it is read, in place of the
/// 100 Continue a client may have asked for; a chunked body reserves max-body-size; the reservation ends when the body
/// reaches its handler or the connection drops.
BOOST_FIXTURE_TEST_CASE(request_body_reserved_while_read, http_plugin_test_fixture) {
   constexpr size_t body_size = 2 * 1024 * 1024; // the default max-body-size
   constexpr auto   budget = "--http-max-bytes-in-flight-mb=5"; // room for two such bodies, not three
   const std::string endpoint = test_http_endpoint("127.0.0.1", request_body_read_index);
   const std::string server_address = "--http-server-address=" + endpoint;

   http_plugin* plugin = init({"--plugin=sysio::http_plugin", server_address.c_str(), budget});
   BOOST_REQUIRE(plugin);
   body_size_client::add_route(*plugin);

   net::io_context ctx;
   const body_size_client client{ctx, endpoint, test_http_port(request_body_read_index)};
   const auto in_flight_becomes = [&](size_t bytes) {
      return wait_until([&] { return plugin->bytes_in_flight() == bytes; });
   };
   const std::string length_field = body_size_client::content_length(body_size);
   const std::string half_body(body_size / 2, 'x');

   // Bodies still arriving hold their declared length.
   auto first = client.connect();
   net::write(first, net::buffer(client.header(length_field)));
   net::write(first, net::buffer(half_body));
   BOOST_REQUIRE(in_flight_becomes(body_size));
   auto second = client.connect();
   net::write(second, net::buffer(client.header(length_field)));
   net::write(second, net::buffer(half_body));
   BOOST_REQUIRE(in_flight_becomes(2 * body_size));

   // A third does not fit: refused from its header alone, then closed.
   auto third = client.connect();
   net::write(third, net::buffer(client.header(length_field)));
   const auto refused = body_size_client::read_response(third);
   BOOST_CHECK(refused.result() == http::status::service_unavailable);
   BOOST_CHECK(refused.body().find("Too many bytes in flight") != std::string::npos);
   BOOST_CHECK(!refused.keep_alive());
   BOOST_CHECK(closed_by_server(read_until_closed(ctx, third)));

   // A client waiting for 100 Continue gets the 503 instead.
   auto expecting = client.connect();
   net::write(expecting, net::buffer(client.header(length_field + "Expect: 100-continue\r\n")));
   BOOST_CHECK(body_size_client::read_response(expecting).result() == http::status::service_unavailable);
   BOOST_CHECK(closed_by_server(read_until_closed(ctx, expecting)));
   BOOST_CHECK_EQUAL(plugin->bytes_in_flight(), 2 * body_size);

   // The first body completes, reaches its handler whole, and its reservation ends.
   net::write(first, net::buffer(half_body));
   const auto completed = body_size_client::read_response(first);
   BOOST_CHECK(completed.result() == http::status::ok);
   BOOST_CHECK_EQUAL(completed.body(), std::to_string(body_size));
   BOOST_REQUIRE(in_flight_becomes(body_size));

   // A chunked body reserves max-body-size, its most, since its length is unknown until it ends.
   constexpr std::string_view chunk_data = "chunk";
   const std::string chunked_body = "5\r\n" + std::string(chunk_data) + "\r\n0\r\n\r\n";
   auto chunked = client.connect();
   net::write(chunked, net::buffer(client.header("Transfer-Encoding: chunked\r\n")));
   BOOST_REQUIRE(in_flight_becomes(2 * body_size));
   net::write(chunked, net::buffer(chunked_body));
   const auto chunked_response = body_size_client::read_response(chunked);
   BOOST_CHECK(chunked_response.result() == http::status::ok);
   BOOST_CHECK_EQUAL(chunked_response.body(), std::to_string(chunk_data.size()));
   BOOST_REQUIRE(in_flight_becomes(body_size));

   // A client that drops mid-body leaves no reservation behind.
   second.close();
   BOOST_CHECK(in_flight_becomes(0));
   for (auto* socket : {&first, &third, &expecting, &chunked})
      socket->close();
   BOOST_CHECK(wait_until([&] { return plugin->requests_in_flight() == 0; }));
}

/// A body has a fixed allowance plus its size at the minimum rate to arrive.
BOOST_AUTO_TEST_CASE(request_body_read_timeout_scales_with_size) {
   using namespace std::chrono_literals;
   BOOST_CHECK(request_body_read_timeout(0) == request_body_read_allowance);
   BOOST_CHECK(request_body_read_timeout(request_body_min_bytes_per_second) == request_body_read_allowance + 1s);
   BOOST_CHECK(request_body_read_timeout(2 * 1024 * 1024) == 42s); // the default max-body-size
}

/// A body that does not arrive in time closes its connection and releases its reservation, whether the client sent
/// only the header or waited for 100 Continue, and the larger the body the longer it has; one that pauses but arrives
/// in time is answered. While stalled bodies hold the budget, a body that does not fit is refused; once their
/// connections close it is served.
BOOST_FIXTURE_TEST_CASE(stalled_request_body_closed_at_deadline, http_plugin_test_fixture) {
   using namespace std::chrono_literals;
   using steady_clock = std::chrono::steady_clock;
   constexpr size_t budget_bytes = 1024 * 1024;
   constexpr auto   budget = "--http-max-bytes-in-flight-mb=1";
   constexpr size_t small_body = 1;
   constexpr size_t large_body = 2 * request_body_min_bytes_per_second; // two seconds past the allowance
   constexpr size_t paused_body = 2;
   constexpr size_t stalled_bytes = small_body + large_body + small_body;
   constexpr size_t too_large_while_stalled = budget_bytes - large_body / 2;
   static_assert(stalled_bytes + too_large_while_stalled > budget_bytes && too_large_while_stalled < budget_bytes);
   constexpr auto pause = 5s;
   static_assert(pause < request_body_read_allowance);
   const std::string endpoint = test_http_endpoint("127.0.0.1", request_body_deadline_index);
   const std::string server_address = "--http-server-address=" + endpoint;

   http_plugin* plugin = init({"--plugin=sysio::http_plugin", server_address.c_str(), budget});
   BOOST_REQUIRE(plugin);
   body_size_client::add_route(*plugin);

   net::io_context ctx;
   const body_size_client client{ctx, endpoint, test_http_port(request_body_deadline_index)};
   const auto in_flight_becomes = [&](size_t bytes) {
      return wait_until([&] { return plugin->bytes_in_flight() == bytes; });
   };

   auto small = client.connect();
   auto large = client.connect();
   auto expecting = client.connect();
   auto paused = client.connect();
   // Taken before any header is sent, so no deadline can have started earlier.
   const auto start = steady_clock::now();
   net::write(small, net::buffer(client.header(body_size_client::content_length(small_body))));
   net::write(large, net::buffer(client.header(body_size_client::content_length(large_body))));
   net::write(expecting, net::buffer(client.header(body_size_client::content_length(small_body) +
                                                   "Expect: 100-continue\r\n")));
   BOOST_CHECK(body_size_client::read_response(expecting).result() == http::status::continue_);
   net::write(paused, net::buffer(client.header(body_size_client::content_length(paused_body)) + "x"));
   BOOST_REQUIRE(in_flight_becomes(stalled_bytes + paused_body));

   // The stalled bodies hold the budget, so one that does not fit beside them is refused.
   auto refused = client.connect();
   net::write(refused, net::buffer(client.header(body_size_client::content_length(too_large_while_stalled))));
   BOOST_CHECK(body_size_client::read_response(refused).result() == http::status::service_unavailable);
   BOOST_CHECK(closed_by_server(read_until_closed(ctx, refused)));

   // A pause short of the deadline does not cut a body off.
   std::this_thread::sleep_until(start + pause);
   net::write(paused, net::buffer(std::string("y")));
   const auto answered = body_size_client::read_response(paused);
   BOOST_CHECK(answered.result() == http::status::ok);
   BOOST_CHECK_EQUAL(answered.body(), std::to_string(paused_body));
   BOOST_REQUIRE(in_flight_becomes(stalled_bytes));

   // Each stalled body's connection closes at its deadline, not before.
   const auto closed_after = [&](tcp::socket& socket, size_t body_size) {
      const auto deadline = request_body_read_timeout(body_size);
      BOOST_CHECK(closed_by_server(read_until_closed(ctx, socket, deadline + server_reaction_limit)));
      BOOST_CHECK_MESSAGE(steady_clock::now() - start >= deadline,
                          "a " << body_size << " byte body was cut off before its deadline");
   };
   closed_after(small, small_body);
   closed_after(expecting, small_body);
   closed_after(large, large_body);
   BOOST_CHECK(in_flight_becomes(0));

   // With the reservations released, the body refused before now fits.
   auto served = client.connect();
   net::write(served, net::buffer(client.header(body_size_client::content_length(too_large_while_stalled))));
   net::write(served, net::buffer(std::string(too_large_while_stalled, 'x')));
   const auto response = body_size_client::read_response(served);
   BOOST_CHECK(response.result() == http::status::ok);
   BOOST_CHECK_EQUAL(response.body(), std::to_string(too_large_while_stalled));

   for (auto* socket : {&small, &large, &expecting, &paused, &refused, &served})
      socket->close();
   BOOST_CHECK(wait_until([&] { return plugin->requests_in_flight() == 0; }));
}
