#pragma once

#include <magic_enum/magic_enum.hpp>

#include <sysio/http_plugin/http_plugin.hpp>

#include <fc/io/raw.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/thread_utils.hpp>
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
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

   /// Atomically check and reserve sz request-body bytes. Returns an error on refusal, or an empty string on
   /// success. A successful reservation must be paired with exactly one release_bytes_in_flight(sz).
   [[nodiscard]] virtual std::string try_reserve_request_body(size_t sz) = 0;

   /// Release sz bytes previously reserved with try_reserve_request_body.
   virtual void release_bytes_in_flight(size_t sz) = 0;
   virtual void send_busy_response(std::string&& what) = 0;

   /// Answer the exception being handled with an error response. Call only from a catch block, as the caller's last
   /// use of the connection.
   virtual void handle_exception() = 0;

   /// Set the Content-Type header of the response to be sent, replacing the type of the handler that serves it.
   virtual void set_content_type_header(http_content_type content_type) = 0;

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
   /// Whether the bytes-in-flight budget can refuse the handler's response.
   http_response_policy response_policy = http_response_policy::budgeted;
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

/// A request body has this long, plus its size at transfer_min_bytes_per_second, to arrive once its header is read,
/// and a response as long to be written once its write starts. Past that the connection is closed, which releases the
/// bytes the transfer holds against bytes_in_flight: a stalled peer cannot hold its share of the budget for longer,
/// while a slow one that keeps up is not cut off.
inline constexpr std::chrono::seconds transfer_allowance{10};
/// The average rate, in bytes per second, a request body or a response must at least keep up; see transfer_allowance.
inline constexpr uint64_t transfer_min_bytes_per_second = 64 * 1024;

/// The time a request body or a response of `size` bytes has to be transferred; see transfer_allowance.
inline std::chrono::microseconds transfer_timeout(size_t size) {
   constexpr uint64_t microseconds_per_second = 1'000'000;
   return transfer_allowance +
          std::chrono::microseconds(size * microseconds_per_second / transfer_min_bytes_per_second);
}

struct http_plugin_state {
   string access_control_allow_origin;
   string access_control_allow_headers;
   string access_control_max_age;
   bool access_control_allow_credentials = false;
   size_t max_body_size{2 * 1024 * 1024};

   std::atomic<int32_t> requests_in_flight{0};
   size_t max_bytes_in_flight = 0;
   int32_t max_requests_in_flight = -1;
   fc::microseconds max_response_time{30 * 1000};

   bool validate_host = true;
   set<string> valid_hosts;

   string server_header;

   url_handlers_type url_handlers;
   bool keep_alive = false;

   uint16_t thread_pool_size = 2;
   struct http; // http is a namespace so use an embedded type for the named_thread_pool tag
   fc::named_thread_pool<http> thread_pool;

   fc::logger& logger;
   std::function<void(http_plugin::metrics)> update_metrics;

   fc::logger& get_logger() { return logger; }

   explicit http_plugin_state(fc::logger& log)
       : logger(log) {}

   /// Return a snapshot of the bytes currently counted against the budget.
   size_t bytes_in_flight() const noexcept {
      return bytes_in_flight_.load();
   }

   /// Count response bytes, including responses that must be sent over budget.
   void account_response_bytes(size_t size) noexcept {
      bytes_in_flight_ += size;
   }

   /// Release bytes previously counted by request admission or response accounting.
   void release_bytes_in_flight(size_t size) noexcept {
      bytes_in_flight_ -= size;
   }

   /// Check whether extra response bytes fit without reserving them.
   std::string verify_max_bytes_in_flight(size_t extra_bytes) {
      const auto current = bytes_in_flight_.load(std::memory_order_relaxed);
      return bytes_fit(current, extra_bytes) ? std::string{} : bytes_in_flight_error(current, extra_bytes);
   }

   /// Check and reserve body bytes against the same counter value, retrying when another operation changes it.
   [[nodiscard]] std::string try_reserve_request_body(size_t size) {
      auto current = bytes_in_flight_.load(std::memory_order_relaxed);
      do {
         if (!bytes_fit(current, size))
            return bytes_in_flight_error(current, size);
      } while (!bytes_in_flight_.compare_exchange_weak(current, current + size, std::memory_order_relaxed));
      return {};
   }

private:
   std::atomic<size_t> bytes_in_flight_{0};

   /// Check before subtracting because response writes can put the shared counter over budget.
   bool bytes_fit(size_t current, size_t extra_bytes) const {
      return current <= max_bytes_in_flight && extra_bytes <= max_bytes_in_flight - current;
   }

   /// Report the refused total, saturated at the counter's maximum representable value.
   std::string bytes_in_flight_error(size_t current, size_t extra_bytes) {
      const auto total = current + std::min(extra_bytes, std::numeric_limits<size_t>::max() - current);
      fc_dlog(get_logger(), "503 - too many bytes in flight: {}", total);
      return "Too many bytes in flight: " + std::to_string(total);
   }
};

/**
* Construct a lambda appropriate for url_response_callback that will
* JSON-stringify the provided response
*
* @param plugin_state - plugin state object, shared state of http_plugin
* @param session_ptr - beast_http_session object on which to invoke send_response
* @param content_type - the content type of the handler that answers
* @param response_policy - whether a 503 replaces a response that does not fit the bytes-in-flight budget
* @return lambda suitable for url_response_callback
*/
inline auto make_http_response_handler(http_plugin_state& plugin_state, detail::abstract_conn_ptr session_ptr,
                                       http_content_type content_type, http_response_policy response_policy) {
   return [&plugin_state, session_ptr{std::move(session_ptr)}, content_type,
           response_policy](int code, std::optional<fc::variant> response) mutable {
      auto payload_size = detail::in_flight_sizeof(response);
      plugin_state.account_response_bytes(payload_size);

      // post back to an HTTP thread to allow the response handler to be called from any thread
      boost::asio::dispatch(plugin_state.thread_pool.get_executor(),
                        [&plugin_state, session_ptr{std::move(session_ptr)}, code, payload_size,
                         response = std::move(response), content_type, response_policy]() {
                           auto on_exit = fc::make_scoped_exit([&] {
                              plugin_state.release_bytes_in_flight(payload_size);
                           });

                           // Why the budget refuses the response with `extra_bytes` more in flight; empty to send it.
                           const auto refusal = [&](size_t extra_bytes) -> std::string {
                              if (response_policy == http_response_policy::always_sent)
                                 return {};
                              return session_ptr->verify_max_bytes_in_flight(extra_bytes);
                           };

                           if(auto error_str = refusal(0); !error_str.empty()) {
                              session_ptr->send_busy_response(std::move(error_str));
                              return;
                           }

                           try {
                              // A plaintext handler's string body is sent as-is; anything else it produces (an error
                              // object, or no value) is sent as JSON.
                              const bool plaintext_body = content_type == http_content_type::plaintext &&
                                                          response.has_value() && response->is_string();
                              if (content_type == http_content_type::plaintext && !plaintext_body)
                                 session_ptr->set_content_type_header(http_content_type::json);
                              // HTTP 204 never carries a body, including JSON-RPC notifications.
                              if (code == static_cast<int>(magic_enum::enum_integer(boost::beast::http::status::no_content))) {
                                 session_ptr->send_response({}, code);
                              } else if (response.has_value()) {
                                 std::string json = plaintext_body
                                                       ? response->as_string()
                                                       : fc::json::to_string(*response, fc::time_point::maximum());
                                 if (auto error_str = refusal(json.size()); error_str.empty())
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
