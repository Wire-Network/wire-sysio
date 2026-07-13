#pragma once

#include <sysio/chain/thread_utils.hpp>// for thread pool
#include <sysio/http_plugin/http_plugin.hpp>

#include <fc/io/json_stream.hpp>
#include <fc/io/raw.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/time.hpp>
#include <fc/utility.hpp>
#include <fc/network/listener.hpp>
#include <fc/scoped_exit.hpp>

#include <boost/asio.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/make_unique.hpp>
#include <boost/optional.hpp>

#include <boost/asio/basic_socket_acceptor.hpp>
#include <boost/asio/basic_socket_iostream.hpp>
#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/detail/config.hpp>

#include <atomic>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>


namespace sysio {
static uint16_t const uri_default_port = 80;
/// Default port for wss://
static uint16_t const uri_default_secure_port = 443;

using std::map;
using std::set;
using std::string;

namespace beast = boost::beast;     // from <boost/beast.hpp>
namespace http = boost::beast::http;// from <boost/beast/http.hpp>
namespace asio = boost::asio;
using boost::asio::ip::tcp;// from <boost/asio/ip/tcp.hpp>


namespace detail {
/**
* virtualized wrapper for the various underlying connection functions needed in req/resp processng
*/
struct abstract_conn {
   virtual ~abstract_conn() = default;
   virtual std::string verify_max_bytes_in_flight(size_t extra_bytes) = 0;
   virtual std::string verify_max_requests_in_flight() = 0;

   /// Reserve sz bytes against the bytes_in_flight budget. Used to account for request/response payload
   /// memory held while work is in flight. Must be paired with exactly one decrement_bytes_in_flight(sz).
   virtual void increment_bytes_in_flight(size_t sz) = 0;

   /// Release sz bytes previously reserved with increment_bytes_in_flight.
   virtual void decrement_bytes_in_flight(size_t sz) = 0;
   virtual void send_busy_response(std::string&& what) = 0;
   virtual void handle_exception() = 0;

   virtual void send_response(std::string&& json_body, unsigned int code) = 0;

   /// Send a file as the HTTP response body using zero-copy I/O.
   /// If byte_range is set, sends a 206 Partial Content response for the given [start, end] inclusive range.
   virtual void send_file_response(const std::filesystem::path& file_path,
                                   unsigned int code,
                                   std::string_view content_type,
                                   std::optional<std::pair<uint64_t,uint64_t>> byte_range = {}) = 0;

   /// Return the value of an HTTP request header, or empty string if not present.
   virtual std::string get_request_header(std::string_view field_name) const = 0;
};

using abstract_conn_ptr = std::shared_ptr<abstract_conn>;

/**
* internal url handler that contains more parameters than the handlers provided by external systems
*/
using internal_url_handler_fn = std::function<void(abstract_conn_ptr, string&&, string&&, url_response_callback&&)>;
struct internal_url_handler {
   internal_url_handler_fn fn;
   api_category category;
   http_content_type content_type = http_content_type::json;
};

/**
* Streaming counterpart of internal_url_handler.  Endpoints registered via
* http_plugin::add_handler_stream / add_async_handler_stream live in the parallel
* url_handlers_stream map; the beast dispatch checks both maps and routes to whichever
* one owns the URL.  No content_type slot - the streaming cb always emits
* application/json directly into the response buffer.
*/
using internal_url_handler_stream_fn = std::function<void(abstract_conn_ptr, string&&, string&&, url_response_stream_callback&&)>;
struct internal_url_handler_stream {
   internal_url_handler_stream_fn fn;
   api_category category;
};
/**
* Helper method to calculate the "in flight" size of a fc::variant
* This is an estimate based on fc::raw::pack if that process can be successfully executed
*
* @param v - the fc::variant
* @return in flight size of v
*/
static size_t in_flight_sizeof(const fc::variant& v) {
   try {
      return fc::raw::pack_size(v);
   } catch(...) {}
   return 0;
}

/**
* Helper method to calculate the "in flight" size of a std::optional<T>
* When the optional doesn't contain value, it will return the size of 0
*
* @param o - the std::optional<T> where T is typename
* @return in flight size of o
*/
template<typename T>
static size_t in_flight_sizeof(const std::optional<T>& o) {
   if(o) {
      return in_flight_sizeof(*o);
   }
   return 0;
}

}// namespace detail

// key -> priority, url_handler
typedef map<string, detail::internal_url_handler> url_handlers_type;

// Streaming-cb counterpart of url_handlers_type.  Disjoint URL-set from the variant
// path; an endpoint registers either form, never both.
typedef map<string, detail::internal_url_handler_stream> url_handlers_stream_type;

struct http_plugin_state {
   string access_control_allow_origin;
   string access_control_allow_headers;
   string access_control_max_age;
   bool access_control_allow_credentials = false;
   size_t max_body_size{2 * 1024 * 1024};

   std::atomic<size_t> bytes_in_flight{0};
   std::atomic<int32_t> requests_in_flight{0};
   size_t max_bytes_in_flight = 0;
   int32_t max_requests_in_flight = -1;
   fc::microseconds max_response_time{30 * 1000};

   bool validate_host = true;
   set<string> valid_hosts;

   string server_header;

   url_handlers_type url_handlers;
   url_handlers_stream_type url_handlers_stream;
   bool keep_alive = false;

   uint16_t thread_pool_size = 2;
   struct http; // http is a namespace so use an embedded type for the named_thread_pool tag
   sysio::chain::named_thread_pool<http> thread_pool;

   fc::logger& logger;
   std::function<void(http_plugin::metrics)> update_metrics;

   fc::logger& get_logger() { return logger; }

   explicit http_plugin_state(fc::logger& log)
       : logger(log) {}

};

/**
* Construct a lambda appropriate for url_response_callback that will
* JSON-stringify the provided response
*
* @param plugin_state - plugin state object, shared state of http_plugin
* @param session_ptr - beast_http_session object on which to invoke send_response
* @return lambda suitable for url_response_callback
*/
inline auto make_http_response_handler(http_plugin_state& plugin_state, detail::abstract_conn_ptr session_ptr, http_content_type content_type) {
   return [&plugin_state,
           session_ptr{std::move(session_ptr)}, content_type](int code, std::optional<fc::variant> response) mutable {
      auto payload_size = detail::in_flight_sizeof(response);
      plugin_state.bytes_in_flight += payload_size;

      // post back to an HTTP thread to allow the response handler to be called from any thread
      boost::asio::dispatch(plugin_state.thread_pool.get_executor(),
                        [&plugin_state, session_ptr{std::move(session_ptr)}, code, payload_size, response = std::move(response), content_type]() {
                           auto on_exit = fc::make_scoped_exit([&](){plugin_state.bytes_in_flight -= payload_size;});

                           if(auto error_str = session_ptr->verify_max_bytes_in_flight(0); !error_str.empty()) {
                              session_ptr->send_busy_response(std::move(error_str));
                              return;
                           }

                           try {
                              if (response.has_value()) {
                                 // plaintext path: the variant's string payload is the literal
                                 // response body.
                                 // json_raw path: the variant's string payload is pre-serialized
                                 // JSON (built via fc::json_writer) and passed through as-is.
                                 // json_raw still tolerates non-string variants (eg error
                                 // responses produced by http_plugin::handle_exception) by
                                 // falling through to fc::json::to_string; the content-type
                                 // header stays "application/json" either way.
                                 std::string json;
                                 if (content_type == http_content_type::plaintext) {
                                    json = response->as_string();
                                 } else if (content_type == http_content_type::json_raw && response->is_string()) {
                                    json = response->as_string();
                                 } else {
                                    json = fc::json::to_string(*response, fc::time_point::maximum());
                                 }
                                 if (auto error_str = session_ptr->verify_max_bytes_in_flight(json.size()); error_str.empty())
                                    session_ptr->send_response(std::move(json), code);
                                 else
                                    session_ptr->send_busy_response(std::move(error_str));
                              } else {
                                 session_ptr->send_response("{}", code);
                              }
                           } catch (...) {
                              session_ptr->handle_exception();
                           }
                        });
   };// end lambda

}

/**
* Construct a url_response_stream_callback that runs the emitter on the http_plugin
* thread pool and sends the produced JSON as the response body.
*
* The api lambda is responsible for capturing whatever it wants to serialize into
* the emitter closure (typically the typed result struct, by-move).  All JSON
* serialization happens inside the dispatched lambda on the http thread pool, so
* the api thread (read_only / read_write queue) never pays the per-field
* allocation cost the variant tree built.
*
* Backpressure note: bytes_in_flight tracking happens after the emitter runs (we
* don't know the body size until the buffer is full), so this path differs from
* the variant path's pre-post estimate.  Net effect is "verify_max_bytes_in_flight
* checks the actual produced body size before send_response," which is the same
* end-state check the variant path performs.
*/
inline auto make_http_stream_response_handler(http_plugin_state& plugin_state, detail::abstract_conn_ptr session_ptr) {
   return url_response_stream_callback{
      [&plugin_state, session_ptr{std::move(session_ptr)}]
      (int code, stream_emitter emitter) mutable {

         // Note on threading: boost::asio::dispatch on a thread_pool executor runs
         // the function inline when called from a thread already inside the pool
         // (impl/thread_pool.hpp `do_execute`: "Invoke immediately if the
         // blocking.possibly property is enabled and we are already inside the
         // thread pool").  So when this cb is fired from a `post_http_thread_pool`
         // task -- e.g. the Phase 2 closure of a `dispatch::post_direct` registration
         // -- the dispatch below is a function-call short-circuit, not a real post.
         // For sync / sync_void / async-immediate paths (cb invoked on read-only
         // or read-write queue), it correctly bounces JSON serialization + socket
         // I/O onto the http thread pool.
         boost::asio::dispatch(plugin_state.thread_pool.get_executor(),
            [&plugin_state, session_ptr{std::move(session_ptr)}, code, emitter{std::move(emitter)}]() mutable {
               try {
                  std::string body;
                  {
                     fc::json_writer w(body);
                     emitter(w);
                  }
                  plugin_state.bytes_in_flight += body.size();
                  auto on_exit = fc::make_scoped_exit([&, sz=body.size()]() { plugin_state.bytes_in_flight -= sz; });

                  if (auto error_str = session_ptr->verify_max_bytes_in_flight(0); !error_str.empty())
                     session_ptr->send_busy_response(std::move(error_str));
                  else
                     session_ptr->send_response(std::move(body), code);
               } catch (...) {
                  session_ptr->handle_exception();
               }
            });
      }
   };
}

inline bool host_is_valid(const http_plugin_state& plugin_state,
                   const std::string& header_host_port,
                   const asio::ip::address& addr) {
   if(!plugin_state.validate_host) {
      return true;
   }

   auto [hostname, port] = fc::split_host_port(header_host_port);
   boost::system::error_code ec;
   auto                      header_addr = boost::asio::ip::make_address(hostname, ec);
   if (ec)
      return plugin_state.valid_hosts.count(hostname);
   if (header_addr.is_v4() && addr.is_v6()) {
      header_addr = boost::asio::ip::make_address_v6(boost::asio::ip::v4_mapped_t(), header_addr.to_v4());
   }
   return header_addr == addr;
}
}// end namespace sysio
