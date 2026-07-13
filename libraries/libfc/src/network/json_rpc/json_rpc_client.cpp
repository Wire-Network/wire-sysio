// Implementation file for JSON-RPC client
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/task/deadline.hpp>

#include <ares.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace fc::network::json_rpc {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

constexpr int HTTP_VERSION = 11;

constexpr std::string_view HTTP_SCHEME        = "http";
constexpr std::string_view HTTPS_SCHEME       = "https";
constexpr std::string_view DEFAULT_HTTP_PATH  = "/";
constexpr std::string_view OP_RESOLVE         = "JSON-RPC resolve";
constexpr std::string_view OP_CONNECT         = "JSON-RPC connect";
constexpr std::string_view OP_TLS_SNI         = "JSON-RPC TLS SNI";
constexpr std::string_view OP_TLS_HANDSHAKE   = "JSON-RPC TLS handshake";
constexpr std::string_view OP_TLS_SHUTDOWN    = "JSON-RPC TLS shutdown";
constexpr std::string_view OP_HTTP_WRITE      = "JSON-RPC HTTP write";
constexpr std::string_view OP_HTTP_READ       = "JSON-RPC HTTP read";
constexpr std::uint64_t    MAX_RESPONSE_BODY_BYTES = 1ULL * 1024ULL * 1024ULL;

/// Return the explicit URL port or the scheme's default JSON-RPC transport port.
std::string default_port_for(const fc::url& url, std::string_view scheme) {
   return std::to_string(url.port().value_or(scheme == HTTPS_SCHEME ? 443 : 80));
}

/// Return the caller-scoped transport deadline, treating maximum as unbounded.
std::optional<fc::time_point> active_transport_deadline() {
   auto deadline = fc::task::current_deadline();
   if (deadline && *deadline < fc::time_point::maximum()) {
      return deadline;
   }
   // Preserve legacy behavior for generic callers; outpost paths install a
   // deadline_scope before entering retry and JSON-RPC transport code.
   return std::nullopt;
}

/// Throw the fc timeout type expected by retry and batch-job callers.
void throw_transport_timeout(std::string_view op_label) {
   FC_THROW_EXCEPTION(fc::timeout_exception, "{} timed out", std::string(op_label));
}

/// Compute remaining time for an async operation or throw if the budget expired.
std::chrono::microseconds remaining_until(fc::time_point deadline_abs,
                                          std::string_view op_label) {
   const auto now = fc::time_point::now();
   if (now >= deadline_abs) {
      throw_transport_timeout(op_label);
   }
   return std::chrono::microseconds((deadline_abs - now).count());
}

/// Identify timeout error codes returned by Beast/Asio cancellation paths.
bool is_timeout_error(const boost::system::error_code& ec) {
   return ec == beast::error::timeout ||
          ec == asio::error::timed_out ||
          ec == boost::system::errc::make_error_code(boost::system::errc::timed_out);
}

/// Identify Beast parser admission failures for oversized response bodies.
bool is_body_limit_error(const boost::system::error_code& ec) {
   return ec == http::error::body_limit;
}

/// Convert a low-level transport error into an fc exception.
void throw_transport_error(const boost::system::error_code& ec,
                           std::string_view op_label) {
   if (is_timeout_error(ec)) {
      throw_transport_timeout(op_label);
   }
   if (is_body_limit_error(ec)) {
      FC_THROW("{} exceeded {} byte response body limit",
               std::string(op_label),
               MAX_RESPONSE_BODY_BYTES);
   }
   FC_THROW("{} failed: {}", std::string(op_label), ec.message());
}

using async_complete_fn = std::function<void(const boost::system::error_code&)>;

/** Own the process-wide c-ares library initialization. */
class cares_library {
public:
   cares_library() noexcept
      : _status(ares_library_init(ARES_LIB_INIT_ALL))
      , _thread_safe(_status == ARES_SUCCESS && ares_threadsafety()) {}

   cares_library(const cares_library&) = delete;
   cares_library& operator=(const cares_library&) = delete;

   ~cares_library() {
      if (_status == ARES_SUCCESS) {
         ares_library_cleanup();
      }
   }

   /** Throw a normal transport exception if deadline-aware resolution is unavailable. */
   void require_event_thread_support() const {
      if (_status != ARES_SUCCESS) {
         FC_THROW("c-ares library initialization failed: {}", ares_strerror(_status));
      }
      if (!_thread_safe) {
         FC_THROW("c-ares was built without thread-safe event support");
      }
   }

private:
   int  _status;
   bool _thread_safe;
};

// c-ares requires global initialization before the process starts any other thread.
const cares_library process_cares_library;

/** Destroy a c-ares channel after its query and event thread have stopped. */
struct cares_channel_deleter {
   void operator()(ares_channel_t* channel) const {
      if (channel) {
         ares_destroy(channel);
      }
   }
};

using cares_channel_ptr = std::unique_ptr<ares_channel_t, cares_channel_deleter>;

/** Completion state shared with the c-ares event-thread callback. */
struct cares_resolution_state {
   std::mutex                 mutex;
   std::condition_variable    completed;
   bool                       done = false;
   int                        status = ARES_EDESTRUCTION;
   std::exception_ptr         error;
   std::vector<tcp::endpoint> endpoints;
};

/** Copy a c-ares address result into Boost.Asio endpoints and wake the waiting caller. */
void complete_cares_resolution(void* arg, int status, int, ares_addrinfo* result) noexcept {
   std::unique_ptr<ares_addrinfo, decltype(&ares_freeaddrinfo)> result_owner(result, &ares_freeaddrinfo);
   auto& state = *static_cast<cares_resolution_state*>(arg);

   {
      std::lock_guard lock(state.mutex);
      state.status = status;
      try {
         if (status == ARES_SUCCESS && result) {
            for (auto* node = result->nodes; node; node = node->ai_next) {
               if (node->ai_family != AF_INET && node->ai_family != AF_INET6) {
                  continue;
               }

               tcp::endpoint endpoint;
               endpoint.resize(static_cast<std::size_t>(node->ai_addrlen));
               std::memcpy(endpoint.data(), node->ai_addr, node->ai_addrlen);
               state.endpoints.push_back(endpoint);
            }
         }
      } catch (...) {
         state.error = std::current_exception();
      }
      state.done = true;
   }
   state.completed.notify_one();
}

/** Create a c-ares channel whose internal event thread can be cancelled without waiting on getaddrinfo. */
cares_channel_ptr create_cares_channel(const std::optional<std::string>& resolver_servers_override) {
   process_cares_library.require_event_thread_support();

   ares_options options{};
   options.evsys = ARES_EVSYS_DEFAULT;

   ares_channel_t* raw_channel = nullptr;
   const auto init_status = ares_init_options(&raw_channel, &options, ARES_OPT_EVENT_THREAD);
   if (init_status != ARES_SUCCESS) {
      FC_THROW("c-ares channel initialization failed: {}", ares_strerror(init_status));
   }

   cares_channel_ptr channel(raw_channel);
   if (resolver_servers_override) {
      const auto server_status = ares_set_servers_ports_csv(channel.get(), resolver_servers_override->c_str());
      if (server_status != ARES_SUCCESS) {
         FC_THROW("c-ares DNS server override failed: {}", ares_strerror(server_status));
      }
   }
   return channel;
}

/// Run one async operation synchronously, cancelling it when the deadline expires.
template <typename StartFn, typename CancelFn>
void run_async_op(asio::io_context& ioc,
                  const std::optional<fc::time_point>& deadline_abs,
                  std::string_view op_label,
                  StartFn&& start,
                  CancelFn&& cancel) {
   boost::system::error_code op_ec;
   bool                      op_done    = false;
   bool                      timer_done = !deadline_abs;
   bool                      timed_out  = false;

   ioc.restart();
   std::optional<asio::steady_timer> timer;
   if (deadline_abs) {
      timer.emplace(ioc);
      timer->expires_after(remaining_until(*deadline_abs, op_label));
      timer->async_wait([&](const boost::system::error_code& ec) {
         timer_done = true;
         if (!ec && !op_done) {
            timed_out = true;
            cancel();
         }
      });
   }

   start([&](const boost::system::error_code& ec) {
      op_done = true;
      op_ec   = ec;
      if (timer) {
         timer->cancel();
      }
   });

   while (!(op_done && timer_done)) {
      ioc.run_one();
   }

   if (timed_out) {
      throw_transport_timeout(op_label);
   }
   if (op_ec) {
      throw_transport_error(op_ec, op_label);
   }
}

/** Resolve DNS through c-ares so an in-flight query can be cancelled at the active deadline. */
tcp::resolver::results_type resolve_endpoints_with_deadline(
   const std::string& host,
   const std::string& port,
   fc::time_point deadline_abs,
   const std::optional<std::string>& resolver_servers_override) {
   (void)remaining_until(deadline_abs, OP_RESOLVE);

   cares_resolution_state state;
   auto                   channel = create_cares_channel(resolver_servers_override);
   ares_addrinfo_hints     hints{};
   hints.ai_flags    = ARES_AI_NUMERICSERV;
   hints.ai_family   = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;

   (void)remaining_until(deadline_abs, OP_RESOLVE);
   ares_getaddrinfo(channel.get(), host.c_str(), port.c_str(), &hints, complete_cares_resolution, &state);

   std::unique_lock lock(state.mutex);
   if (!state.completed.wait_for(lock, remaining_until(deadline_abs, OP_RESOLVE), [&] { return state.done; })) {
      lock.unlock();
      ares_cancel(channel.get());
      throw_transport_timeout(OP_RESOLVE);
   }

   if (state.error) {
      std::rethrow_exception(state.error);
   }
   if (state.status != ARES_SUCCESS) {
      FC_THROW("{} failed: {}", std::string(OP_RESOLVE), ares_strerror(state.status));
   }
   if (state.endpoints.empty()) {
      FC_THROW("{} failed: resolver returned no endpoints", std::string(OP_RESOLVE));
   }

   return tcp::resolver::results_type::create(state.endpoints.begin(), state.endpoints.end(), host, port);
}

/// Resolve DNS for the long-lived client cache, using c-ares only when the caller supplied a deadline.
tcp::resolver::results_type resolve_endpoints(asio::io_context& ioc,
                                              const std::string& host,
                                              const std::string& port,
                                              const std::optional<fc::time_point>& deadline_abs,
                                              const std::optional<std::string>& resolver_servers_override) {
   if (deadline_abs) {
      return resolve_endpoints_with_deadline(host, port, *deadline_abs, resolver_servers_override);
   }

   tcp::resolver resolver{ioc};
   tcp::resolver::results_type results;

   run_async_op(ioc, deadline_abs, OP_RESOLVE,
      [&](const async_complete_fn& complete) {
         resolver.async_resolve(host, port,
            [&results, complete](const boost::system::error_code& ec, tcp::resolver::results_type resolved) {
               if (!ec) {
                  results = std::move(resolved);
               }
               complete(ec);
            });
      },
      [&] { resolver.cancel(); });

   return results;
}

/// Convert the public HTTP verb enum into the Boost.Beast request verb.
http::verb to_beast_verb(http_verb v) {
   switch (v) {
      case http_verb::GET:     return http::verb::get;
      case http_verb::PUT:     return http::verb::put;
      case http_verb::POST:    return http::verb::post;
      case http_verb::DELETE_: return http::verb::delete_;
   }
   FC_THROW("Unknown http_verb value: {}", static_cast<int>(v));
}

/// Run a connection attempt and mark cached endpoints stale if the peer set fails.
template <typename StartFn, typename CancelFn, typename MarkStaleFn>
void connect_with_deadline(asio::io_context& ioc,
                           const std::optional<fc::time_point>& deadline_abs,
                           StartFn&& start,
                           CancelFn&& cancel,
                           MarkStaleFn&& mark_stale) {
   try {
      run_async_op(ioc,
                   deadline_abs,
                   OP_CONNECT,
                   std::forward<StartFn>(start),
                   std::forward<CancelFn>(cancel));
   } catch (const fc::timeout_exception&) {
      throw;
   } catch (const fc::exception&) {
      mark_stale();
      throw;
   }
}

/// Read an HTTP response through a parser with a fixed response body limit.
template <typename Stream, typename CancelFn>
http::response<http::string_body> read_response_with_deadline(
   asio::io_context& ioc,
   const std::optional<fc::time_point>& deadline_abs,
   Stream& stream,
   beast::flat_buffer& buffer,
   CancelFn&& cancel) {
   http::response_parser<http::string_body> parser;
   parser.body_limit(MAX_RESPONSE_BODY_BYTES);

   run_async_op(ioc, deadline_abs, OP_HTTP_READ,
      [&](const async_complete_fn& complete) {
         http::async_read(stream, buffer, parser,
            [complete](const boost::system::error_code& ec, std::size_t) {
               complete(ec);
            });
      },
      std::forward<CancelFn>(cancel));

   return parser.release();
}

/// Execute a single HTTP/HTTPS request and return the raw Beast response.
template <typename MarkStaleFn>
http::response<http::string_body> send_request(asio::io_context& ioc,
                                               const fc::url& url,
                                               const tcp::resolver::results_type& dest,
                                               http::request<http::string_body>& req,
                                               MarkStaleFn&& mark_stale) {
   const auto scheme = url.proto();
   FC_ASSERT(scheme == HTTP_SCHEME || scheme == HTTPS_SCHEME, "Unsupported URL scheme: {}", scheme);
   FC_ASSERT(url.host(), "JSON-RPC URL is missing host");

   const auto host = *url.host();
   const auto deadline_abs = active_transport_deadline();

   beast::flat_buffer                buffer;
   http::response<http::string_body> res;

   if (scheme == HTTPS_SCHEME) {
      asio::ssl::context ctx{asio::ssl::context::tlsv12_client};
      beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

      if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
         auto ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                                     asio::error::get_ssl_category());
         throw_transport_error(ec, OP_TLS_SNI);
      }

      connect_with_deadline(ioc, deadline_abs,
         [&](const async_complete_fn& complete) {
            beast::get_lowest_layer(stream).async_connect(dest,
               [complete](const boost::system::error_code& ec, const tcp::endpoint&) {
                  complete(ec);
               });
         },
         [&] { beast::get_lowest_layer(stream).close(); },
         mark_stale);
      run_async_op(ioc, deadline_abs, OP_TLS_HANDSHAKE,
         [&](const async_complete_fn& complete) {
            stream.async_handshake(asio::ssl::stream_base::client, complete);
         },
         [&] { beast::get_lowest_layer(stream).close(); });
      run_async_op(ioc, deadline_abs, OP_HTTP_WRITE,
         [&](const async_complete_fn& complete) {
            http::async_write(stream, req,
               [complete](const boost::system::error_code& ec, std::size_t) {
                  complete(ec);
               });
         },
         [&] { beast::get_lowest_layer(stream).close(); });
      res = read_response_with_deadline(ioc, deadline_abs, stream, buffer,
         [&] { beast::get_lowest_layer(stream).close(); });

      try {
         run_async_op(ioc, deadline_abs, OP_TLS_SHUTDOWN,
            [&](const async_complete_fn& complete) {
               stream.async_shutdown(complete);
            },
            [&] { beast::get_lowest_layer(stream).close(); });
      } catch (const fc::exception&) {
         // The response has already been read; shutdown only needs to be bounded.
      }
      beast::get_lowest_layer(stream).close();
   } else {
      beast::tcp_stream stream{ioc};

      connect_with_deadline(ioc, deadline_abs,
         [&](const async_complete_fn& complete) {
            stream.async_connect(dest,
               [complete](const boost::system::error_code& ec, const tcp::endpoint&) {
                  complete(ec);
               });
         },
         [&] { stream.close(); },
         mark_stale);
      run_async_op(ioc, deadline_abs, OP_HTTP_WRITE,
         [&](const async_complete_fn& complete) {
            http::async_write(stream, req,
               [complete](const boost::system::error_code& ec, std::size_t) {
                  complete(ec);
               });
         },
         [&] { stream.close(); });
      res = read_response_with_deadline(ioc, deadline_abs, stream, buffer,
         [&] { stream.close(); });

      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);
      stream.close();
   }

   return res;
}

} // namespace

// json_rpc_error
json_rpc_error::json_rpc_error(const std::string& message) : json_rpc_error(0, message, {}) {}

json_rpc_error::json_rpc_error(int code_in, const std::string& message, const variant& data_in)
   : fc::exception(code_in, "json_rpc_error", message)
     , code(code_in)
     , data(data_in) {
   // Push the server-supplied message into the fc::exception log_messages
   // collection so `top_message()` and the FC_LOG_AND_RETHROW family see
   // the actual reason rather than an empty string. fc::exception stores
   // the constructor `message` as the *description*, but `top_message()`
   // reads from `log_messages` — without this append, downstream callers
   // that diagnose JSON-RPC failures get `code=… message=''` and lose the
   // server's diagnostic text.
   append_log( FC_LOG_MESSAGE( error, "{}", message ) );
}

json_rpc_client json_rpc_client::create(const std::variant<std::string, fc::url>& source) {
   fc::url url;
   if (std::holds_alternative<std::string>(source)) {
      url = fc::url(std::get<std::string>(source));
   } else {
      url = std::get<fc::url>(source);
   }
   FC_ASSERT(url.proto() == "http" || url.proto() == "https");

   return json_rpc_client(url);
}

// json_rpc_client
json_rpc_client::json_rpc_client(fc::url url, const std::optional<std::string>& user_agent,
                                 endpoint_refresh_policy refresh_policy)
   : json_rpc_client(std::move(url), user_agent, refresh_policy, std::nullopt) {}

json_rpc_client::json_rpc_client(fc::url url, const std::optional<std::string>& user_agent,
                                 endpoint_refresh_policy refresh_policy,
                                 std::optional<std::string> resolver_servers_override)
   : _url(std::move(url))
   , _host()
   , _port()
   , _user_agent(user_agent.value_or(BOOST_BEAST_VERSION_STRING))
   , _next_id(1)
   , _resolved_endpoints()
   , _resolved_endpoints_stale(false)
   , _refresh_policy(refresh_policy)
   , _resolver_servers_override(std::move(resolver_servers_override)) {
   const auto scheme = _url.proto();
   FC_ASSERT(scheme == HTTP_SCHEME || scheme == HTTPS_SCHEME, "Unsupported URL scheme: {}", scheme);
   FC_ASSERT(_url.host(), "JSON-RPC URL is missing host");

   _host = *_url.host();
   _port = default_port_for(_url, scheme);
   refresh_resolved_endpoints();
}

void json_rpc_client::refresh_resolved_endpoints() {
   _resolved_endpoints =
      resolve_endpoints(_io_ctx, _host, _port, active_transport_deadline(), _resolver_servers_override);
   _resolved_endpoints_stale = false;
}

void json_rpc_client::mark_resolved_endpoints_stale() {
   if (_refresh_policy == endpoint_refresh_policy::on_connection_failure) {
      _resolved_endpoints_stale = true;
   }
}

const json_rpc_client::tcp::resolver::results_type& json_rpc_client::resolved_endpoints() {
   if (_resolved_endpoints_stale || _resolved_endpoints.empty()) {
      refresh_resolved_endpoints();
   }
   return _resolved_endpoints;
}

variant json_rpc_client::call(const std::string& method, const fc::variant& params) {
   const auto id = _next_id++;

   mutable_variant_object obj;
   obj("jsonrpc", "2.0")("method", std::string{method})("params", params)("id", id);

   variant response = send_json(variant(obj));

   validate_basic_response(response);

   // Ensure id matches
   const auto& ro = response.get_object();
   if (!ro.contains("id")) {
      FC_THROW("JSON-RPC: missing 'id' in response");
   }
   const auto& idv    = ro["id"];
   int64_t     got_id = 0;
   if (idv.is_int64())
      got_id = idv.as_int64();
   else if (idv.is_uint64())
      got_id = static_cast<int64_t>(idv.as_uint64());
   else {
      FC_THROW("JSON-RPC: invalid 'id' type in response");
   }

   if (got_id != id) {
      FC_THROW("JSON-RPC: response 'id' does not match request 'id'");
   }

   // Handle error
   if (ro.contains("error")) {
      const auto& e    = ro["error"];
      int         code = 0;
      std::string msg  = "JSON-RPC error";
      variant     data;
      if (e.is_object()) {
         const auto& eo = e.get_object();
         if (eo.contains("code")) {
            const auto& cv = eo["code"];
            if (cv.is_int64())
               code = static_cast<int>(cv.as_int64());
            else if (cv.is_uint64())
               code = static_cast<int>(cv.as_uint64());
         }
         if (eo.contains("message")) {
            msg = eo["message"].as_string();
         }
         if (eo.contains("data")) {
            data = eo["data"];
         }
      }
      throw json_rpc_error(code, msg, data);
   }

   if (!ro.contains("result")) {
      FC_THROW("JSON-RPC: response missing 'result'");
   }

   return ro["result"];
}

void json_rpc_client::notify(const std::string& method, const fc::variant& params) {
   mutable_variant_object obj;
   obj("jsonrpc", "2.0")("method", method)("params", params);

   // Send but ignore JSON body; only ensu.value()re HTTP transport succeeds.
   send_json(fc::variant(obj), false);
}

fc::variant json_rpc_client::call_batch(const std::vector<fc::variant>& requests) {
   // Insert ids where missing (only for "request", not "notification")
   variants payload_variants = requests | std::views::transform([&](auto& req) {
      if (!req.is_object()) {
         throw json_rpc_error("JSON-RPC batch: each element must be an object");
      }
      fc::mutable_variant_object o(req.get_object());
      if (!o.contains("jsonrpc")) {

         o("jsonrpc", "2.0");
      }

      if (!o.contains("method")) {
         throw json_rpc_error("JSON-RPC batch: missing 'method'");
      }

      return o;
   }) | std::ranges::to<std::vector<fc::variant>>();

   variant response = send_json(variant(payload_variants));

   if (!response.is_array()) {
      throw json_rpc_error("JSON-RPC batch: server did not return an array");
   }

   return response;
}

variant json_rpc_client::send_json(const variant& payload, bool expect_json_body) {
   auto  body = fc::json::to_string(payload, fc::json::yield_function_t{});

   std::string path   = _url.path().value_or(std::string(DEFAULT_HTTP_PATH));

   // Build HTTP request
   http::request<http::string_body> req{http::verb::post, path, HTTP_VERSION};
   req.set(http::field::host, _host);
   req.set(http::field::user_agent, _user_agent);
   req.set(http::field::content_type, "application/json");
   req.body() = body;
   req.prepare_payload();
   auto res = send_request(_io_ctx, _url, resolved_endpoints(), req, [this] {
      mark_resolved_endpoints_stale();
   });

   // Check HTTP status
   if (res.result() != http::status::ok) {
      FC_THROW("HTTP ERROR ({}): {}", static_cast<unsigned>(res.result()), res.reason());
   }

   if (!expect_json_body) {
      // For notifications, we ignore the response body.
      return variant();
   }

   if (res.body().empty()) {
      FC_THROW("Empty HTTP body, expected JSON-RPC response");
   }

   auto body_str = res.body();
   variant j = fc::json::from_string(body_str);

   return j;
}

// -----------------------------------------------------------------------
//  send_http — raw HTTP with configurable verb, path, and body.
//  Reuses the same Boost.Beast transport as send_json but does not
//  wrap in JSON-RPC envelope or validate JSON-RPC response structure.
// -----------------------------------------------------------------------

std::string json_rpc_client::send_http(http_verb verb, const std::string& path,
                                       const std::string& body,
                                       const std::string& content_type) {
   http::request<http::string_body> req{to_beast_verb(verb), path, HTTP_VERSION};
   req.set(http::field::host, _host);
   req.set(http::field::user_agent, _user_agent);
   if (!body.empty()) {
      req.set(http::field::content_type, content_type);
      req.body() = body;
   }
   req.prepare_payload();

   auto res = send_request(_io_ctx, _url, resolved_endpoints(), req, [this] {
      mark_resolved_endpoints_stale();
   });

   if (res.result() != http::status::ok) {
      FC_THROW("HTTP {} {} failed ({}): {}",
               std::string(http::to_string(to_beast_verb(verb))),
               path,
               static_cast<unsigned>(res.result()),
               res.reason());
   }

   return res.body();
}

void json_rpc_client::validate_basic_response(const variant& response) {
   if (!response.is_object()) {
      FC_THROW("JSON-RPC: response must be an object");
   }

   const auto& o = response.get_object();
   if (!o.contains("jsonrpc") || !o["jsonrpc"].is_string() ||
       o["jsonrpc"].get_string() != "2.0") {
      FC_THROW("JSON-RPC: invalid or missing 'jsonrpc' == \"2.0\"");
   }
}

} // namespace fc::network::json_rpc
