#include <sysio/chain/application.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/http_plugin/common.hpp>

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

#include "test_port_shard.hpp"

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
// Keeps unsharded IPv6 probe coverage on the historical port 9999.
constexpr uint32_t ipv6_probe_index = 2;

/** Return the string form of a node HTTP port from this test's shard. */
std::string test_http_port(uint32_t index) {
   return std::to_string(sysio::testing::get_port(sysio::testing::port_category::node_http, index));
}

/** Return host:port with this test's port shard applied. */
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
/// call's: a malformed request is a 400, any other failure a 500, and the connection stays open with no bytes left in
/// flight.
BOOST_FIXTURE_TEST_CASE(raw_handler_exceptions_are_api_errors, quiet_http_plugin_test_fixture) {
   constexpr auto invalid_request = "/v1/node/raw_invalid_request";
   constexpr auto reported_invalid_request = "/v1/node/raw_reported_invalid_request";
   constexpr auto internal_error = "/v1/node/raw_internal_error";
   constexpr auto send_throws = "/v1/node/raw_send_throws";
   constexpr auto in_flight = "/v1/node/bytes_in_flight";
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
   plugin->add_async_api({{in_flight, api_category::node, [plugin](string&&, string&&, url_response_callback&& cb) {
                              cb(200, fc::variant(plugin->bytes_in_flight()));
                           }}});

   const std::vector<std::pair<const char*, http::status>> cases{
      {invalid_request, http::status::bad_request},
      {reported_invalid_request, http::status::bad_request},
      {internal_error, http::status::internal_server_error},
      {send_throws, http::status::internal_server_error}};
   for (const auto& [route, status] : cases) {
      const unix_exchange exchange(socket, {route, in_flight}, 11, false);
      BOOST_REQUIRE_MESSAGE(!exchange.error, route << ": " << exchange.error.message());
      BOOST_REQUIRE_EQUAL(exchange.responses.size(), 2u);
      BOOST_CHECK(exchange.responses[0].result() == status);
      BOOST_CHECK(exchange.responses[0].keep_alive());
      BOOST_CHECK_EQUAL(fc::json::from_string(exchange.responses[0].body())["code"].as_uint64(),
                        magic_enum::enum_integer(status));
      BOOST_CHECK_EQUAL(exchange.responses[1].body(), "0");
   }
}

//A warning for future tests: destruction of http_plugin_test_fixture sometimes does not destroy http_plugin's listeners. Tests
// added in the future should avoid reusing ports of other tests in http_plugin_unit_tests.
