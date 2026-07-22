#include <fc/network/http/http_client.hpp>
#include <fc/io/json.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/static_variant.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/system_timer.hpp>
#include <boost/asio/local/stream_protocol.hpp>

using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
namespace http = boost::beast::http;    // from <boost/beast/http.hpp>
namespace local = boost::asio::local;

namespace fc {

namespace {

// Beast limits an individual parser read to roughly 64 KiB, so a larger body buffer is unused.
constexpr size_t download_read_buffer_bytes = 64 * 1024;
constexpr size_t download_parser_buffer_bytes = 64 * 1024;
constexpr size_t max_error_response_body_bytes = 200;
constexpr uint64_t disk_space_check_interval_bytes = 64 * 1024 * 1024;
constexpr uint64_t disk_space_concurrency_margin_bytes = 64 * 1024 * 1024;
constexpr auto disk_space_check_interval = std::chrono::seconds(5);
constexpr auto operation_monitor_interval = std::chrono::milliseconds(100);
constexpr auto download_status_interval = std::chrono::seconds(5);

} // namespace

/**
 * mapping of protocols to their standard ports
 */
static const std::map<std::string,uint16_t> default_proto_ports = {
   {"http", 80}
};

class http_client_impl {
public:
   using host_key = std::tuple<std::string, std::string, uint16_t>;
   using raw_socket_ptr = std::unique_ptr<tcp::socket>;
   using unix_socket_ptr = std::unique_ptr<local::stream_protocol::socket>;
   using connection = std::variant<raw_socket_ptr, unix_socket_ptr>;
   using connection_map = std::map<host_key, connection>;
   using unix_url_split_map = std::map<std::string, fc::url>;
   using error_code = boost::system::error_code;
   using deadline_type = std::chrono::system_clock::time_point;
   using operation_monitor = std::function<void()>;

   http_client_impl()
   :_ioc()
   {
      set_verify_peers(true);
   }

   void add_cert(const std::string& cert_pem_string) {}

   void set_verify_peers(bool enabled) {}

   void set_cancel_check(std::function<bool()> cancel_check) {
      _cancel_check = std::move(cancel_check);
   }

   /** Return whether the caller requested cancellation of the active HTTP operation. */
   bool is_cancelled() const {
      return _cancel_check && _cancel_check();
   }

   template<typename SyncReadStream, typename Fn, typename CancelFn>
   error_code sync_do_with_deadline_impl(SyncReadStream& s, deadline_type deadline, Fn f, CancelFn cf,
                                         const operation_monitor& monitor = {}) {
      if (is_cancelled()) {
         return boost::asio::error::operation_aborted;
      }

      const bool has_deadline = deadline != deadline_type::max();
      bool deadline_expired = false;
      bool deadline_cancelled = false;
      boost::asio::system_timer deadline_timer(_ioc);
      if (has_deadline) {
         deadline_timer.expires_at(deadline);
         deadline_timer.async_wait([&deadline_expired, &deadline_cancelled](const error_code&) {
            // The only non-success error_code expected here is operation_aborted. A success
            // completion may already be queued when cancellation occurs, so use the explicit
            // flag to decide whether the deadline still applies.
            if (!deadline_cancelled) {
               deadline_expired = true;
            }
         });
      }

      bool operation_cancelled = false;
      boost::asio::steady_timer monitor_timer(_ioc);
      std::optional<error_code> f_result;
      std::function<void()> arm_monitor;
      if (_cancel_check || monitor) {
         arm_monitor = [&]() {
            monitor_timer.expires_after(operation_monitor_interval);
            monitor_timer.async_wait([&](const error_code& ec) {
               if (ec == boost::asio::error::operation_aborted || f_result) {
                  return;
               }
               if (is_cancelled()) {
                  operation_cancelled = true;
                  cf();
                  return;
               }
               if (monitor) {
                  monitor();
               }
               arm_monitor();
            });
         };
         arm_monitor();
      }

      f(f_result);

      _ioc.restart();
      while (_ioc.run_one()) {
         if (f_result) {
            deadline_cancelled = true;
            if (has_deadline) {
               deadline_timer.cancel();
            }
            monitor_timer.cancel();
         } else if (deadline_expired) {
            cf();
         }
      }

      if (operation_cancelled) {
         return boost::asio::error::operation_aborted;
      }
      if (deadline_expired) {
         return error_code(boost::system::errc::timed_out, boost::system::system_category());
      }
      return *f_result;
   }

   template<typename SyncReadStream, typename Fn>
   error_code sync_do_with_deadline(SyncReadStream& s, deadline_type deadline, Fn f,
                                    const operation_monitor& monitor = {}) {
      return sync_do_with_deadline_impl(s, deadline, f, [&s]() {
         error_code ec;
         s.lowest_layer().cancel(ec);
      }, monitor);
   }

   template<typename SyncReadStream>
   error_code sync_connect_with_timeout(SyncReadStream& s, const std::string& host, const std::string& port,
                                        const deadline_type& deadline, const operation_monitor& monitor = {}) {
      tcp::resolver local_resolver(_ioc);
      bool cancelled = false;

      auto res = sync_do_with_deadline_impl(
         s, deadline,
         [&local_resolver, &cancelled, &s, &host, &port](std::optional<error_code>& final_ec) {
            local_resolver.async_resolve(
               host, port,
               [&cancelled, &s, &final_ec](const error_code& ec, tcp::resolver::results_type resolved) {
                  if (ec) {
                     final_ec.emplace(ec);
                     return;
                  }

                  if (!cancelled) {
                     boost::asio::async_connect(
                        s, resolved.begin(), resolved.end(),
                        [&final_ec](const error_code& ec, auto) { final_ec.emplace(ec); });
                  }
               });
         },
         [&local_resolver, &cancelled, &s]() {
            cancelled = true;
            local_resolver.cancel();
            error_code ec;
            s.lowest_layer().cancel(ec);
         },
         monitor);

      return res;
   };

   template<typename SyncReadStream>
   error_code sync_write_with_timeout(SyncReadStream& s, http::request<http::string_body>& req,
                                      const deadline_type& deadline, const operation_monitor& monitor = {}) {
      return sync_do_with_deadline(s, deadline, [&s, &req](std::optional<error_code>& final_ec){
         http::async_write(s, req, [&final_ec]( const error_code& ec, std::size_t ) {
            final_ec.emplace(ec);
         });
      }, monitor);
   }

   template<typename SyncReadStream>
   error_code sync_read_with_timeout(SyncReadStream& s, boost::beast::flat_buffer& buffer,
                                     http::response<http::string_body>& res, const deadline_type& deadline,
                                     const operation_monitor& monitor = {}) {
      return sync_do_with_deadline(s, deadline, [&s, &buffer, &res](std::optional<error_code>& final_ec){
         http::async_read(s, buffer, res, [&final_ec]( const error_code& ec, std::size_t ) {
            final_ec.emplace(ec);
         });
      }, monitor);
   }

   /// Read only the response header into @p parser, honoring @p deadline. Used by the
   /// streaming download path to inspect the status line before committing the body to a file.
   template<typename SyncReadStream, typename Parser>
   error_code sync_read_header_with_timeout(SyncReadStream& s, boost::beast::flat_buffer& buffer, Parser& parser,
                                            const deadline_type& deadline,
                                            const operation_monitor& monitor = {}) {
      return sync_do_with_deadline(s, deadline, [&s, &buffer, &parser](std::optional<error_code>& final_ec){
         http::async_read_header(s, buffer, parser, [&final_ec]( const error_code& ec, std::size_t ) {
            final_ec.emplace(ec);
         });
      }, monitor);
   }

   /// Read one increment of a streaming response body into @p parser, honoring @p deadline.
   template<typename SyncReadStream, typename Parser>
   error_code sync_read_parser_some_with_timeout(SyncReadStream& s, boost::beast::flat_buffer& buffer,
                                                 Parser& parser, const deadline_type& deadline,
                                                 const operation_monitor& monitor = {}) {
      return sync_do_with_deadline(s, deadline, [&s, &buffer, &parser](std::optional<error_code>& final_ec) {
         http::async_read_some(s, buffer, parser, [&final_ec](const error_code& ec, std::size_t) {
            final_ec.emplace(ec);
         });
      }, monitor);
   }

   /**
    * Read one decoded response-body increment into caller-owned storage.
    *
    * @p ec retains transport and parser failures for the caller, while need_buffer is normalized
    * because it only means the supplied buffer_body storage was filled successfully.
    */
   size_t read_body_increment(connection& conn, boost::beast::flat_buffer& buffer,
                              http::response_parser<http::buffer_body>& parser, char* sink, size_t sink_size,
                              const operation_monitor& monitor, error_code& ec) {
      parser.get().body().data = sink;
      parser.get().body().size = sink_size;
      ec = std::visit([&](auto& stream) {
         return sync_read_parser_some_with_timeout(
            *stream, buffer, parser, deadline_type::max(), monitor);
      }, conn);
      if (ec == http::error::need_buffer) {
         ec = {};
      }
      return sink_size - parser.get().body().size;
   }

   /// Return whether @p ec can indicate that a cached peer connection closed before reuse.
   static bool is_stale_connection_error(const error_code& ec) {
      return ec == boost::asio::error::eof ||
             ec == boost::asio::error::connection_reset ||
             ec == boost::asio::error::connection_aborted ||
             ec == boost::asio::error::broken_pipe ||
             ec == boost::asio::error::not_connected ||
             ec == boost::asio::error::shut_down ||
             ec == http::error::end_of_stream;
   }

   /// Return a path on the destination filesystem that can be queried for free space.
   static std::filesystem::path space_query_path(const std::filesystem::path& destination) {
      if (destination.has_parent_path()) {
         return destination.parent_path();
      }

      std::error_code ec;
      auto current = std::filesystem::current_path(ec);
      FC_ASSERT(!ec, "Failed to resolve current directory for download disk-space check: {}", ec.message());
      return current;
   }

   /**
    * Require enough free space for @p next_write_bytes while retaining @p reserved_bytes.
    *
    * @return Bytes currently available for writes after retaining the requested reserve.
    */
   static uint64_t require_disk_space(const std::filesystem::path& destination, uint64_t next_write_bytes,
                                      uint64_t reserved_bytes) {
      const auto query_path = space_query_path(destination);
      std::error_code ec;
      const auto space = std::filesystem::space(query_path, ec);
      FC_ASSERT(!ec, "Failed to query free disk space at {}: {}", query_path.string(), ec.message());
      FC_ASSERT(space.available >= reserved_bytes && next_write_bytes <= space.available - reserved_bytes,
                "Insufficient disk space at {} for HTTP download: {} bytes available, {} bytes required for the "
                "next write, and {} bytes reserved as headroom",
                query_path.string(), space.available, next_write_bytes, reserved_bytes);
      return space.available - reserved_bytes;
   }

   /**
    * Verify and return the next cached write budget.
    *
    * Each budget is capped at 64 MiB and requires a separate 64 MiB concurrency margin.
    * The margin bounds headroom exposure if another process consumes space between probes;
    * slow transfers also refresh the probe every five seconds.
    */
   static uint64_t refresh_disk_space_write_budget(const std::filesystem::path& destination,
                                                   uint64_t remaining_body_bytes,
                                                   uint64_t reserved_bytes) {
      const auto write_budget = std::min(remaining_body_bytes, disk_space_check_interval_bytes);
      if (write_budget == 0) {
         require_disk_space(destination, 0, reserved_bytes);
         return 0;
      }

      require_disk_space(destination, write_budget + disk_space_concurrency_margin_bytes, reserved_bytes);
      return write_budget;
   }

   host_key url_to_host_key( const url& dest ) {
      FC_ASSERT(dest.host(), "Provided URL has no host");
      uint16_t port = 80;
      if (dest.port()) {
         port = *dest.port();
      }

      return std::make_tuple(dest.proto(), *dest.host(), port);
   }

   connection_map::iterator create_unix_connection( const url& dest, const deadline_type& deadline) {
      auto key = url_to_host_key(dest);
      auto socket = std::make_unique<local::stream_protocol::socket>(_ioc);

      error_code ec;
      socket->connect(local::stream_protocol::endpoint(*dest.host()), ec);
      FC_ASSERT(!ec, "Failed to connect: {}", ec.message());

      auto res = _connections.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(key),
                                      std::forward_as_tuple(std::move(socket)));

      return res.first;
   }

   connection_map::iterator create_raw_connection(const url& dest, const deadline_type& deadline,
                                                  const operation_monitor& monitor = {}) {
      auto key = url_to_host_key(dest);
      auto socket = std::make_unique<tcp::socket>(_ioc);

      error_code ec = sync_connect_with_timeout(
         *socket, *dest.host(), dest.port() ? std::to_string(*dest.port()) : "80", deadline, monitor);
      FC_ASSERT(!ec, "Failed to connect: {}", ec.message());

      auto res = _connections.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(key),
                                      std::forward_as_tuple(std::move(socket)));

      return res.first;
   }

   connection_map::iterator create_connection(const url& dest, const deadline_type& deadline,
                                              const operation_monitor& monitor = {}) {
      if (dest.proto() == "http") {
         return create_raw_connection(dest, deadline, monitor);
      } else if (dest.proto() == "unix") {
         return create_unix_connection(dest, deadline);
      } else {
         FC_THROW("Unknown protocol {}", dest.proto());
      }
   }

   struct check_closed_visitor : public visitor<bool> {
      bool operator() ( const raw_socket_ptr& ptr ) const {
         return !ptr->is_open();
      }

      bool operator() ( const unix_socket_ptr& ptr) const {
         return !ptr->is_open();
      }
   };

   bool check_closed( const connection_map::iterator& conn_itr ) {
      if (std::visit(check_closed_visitor(), conn_itr->second)) {
         _connections.erase(conn_itr);
         return true;
      } else {
         return false;
      }
   }

   connection_map::iterator get_connection(const url& dest, const deadline_type& deadline,
                                           bool* reused_connection = nullptr,
                                           const operation_monitor& monitor = {}) {
      auto key = url_to_host_key(dest);
      auto conn_itr = _connections.find(key);
      if (conn_itr == _connections.end() || check_closed(conn_itr)) {
         if (reused_connection) {
            *reused_connection = false;
         }
         return create_connection(dest, deadline, monitor);
      } else {
         if (reused_connection) {
            *reused_connection = true;
         }
         return conn_itr;
      }
   }

   struct write_request_visitor : visitor<error_code> {
      write_request_visitor(http_client_impl* that, http::request<http::string_body>& req,
                            const deadline_type& deadline, const operation_monitor& monitor = {})
      :that(that)
      ,req(req)
      ,deadline(deadline)
      ,monitor(monitor)
      {}

      template<typename S>
      error_code operator() ( S& stream ) const {
         return that->sync_write_with_timeout(*stream, req, deadline, monitor);
      }

      http_client_impl*                 that;
      http::request<http::string_body>& req;
      const deadline_type&              deadline;
      operation_monitor                 monitor;
   };

   struct read_response_visitor : visitor<error_code> {
      read_response_visitor(http_client_impl* that, boost::beast::flat_buffer& buffer,
                            http::response<http::string_body>& res, const deadline_type& deadline,
                            const operation_monitor& monitor = {})
      :that(that)
      ,buffer(buffer)
      ,res(res)
      ,deadline(deadline)
      ,monitor(monitor)
      {}

      template<typename S>
      error_code operator() ( S& stream ) const {
         return that->sync_read_with_timeout(*stream, buffer, res, deadline, monitor);
      }

      http_client_impl*                  that;
      boost::beast::flat_buffer&         buffer;
      http::response<http::string_body>& res;
      const deadline_type&               deadline;
      operation_monitor                  monitor;
   };

   variant post_sync(const url& dest, const variant& payload, const fc::time_point& _deadline) {
      auto deadline = _deadline.to_system_clock();
      FC_ASSERT(dest.host(), "No host set on URL");

      std::string path = dest.path() ? dest.path()->generic_string() : "/";
      if (dest.query()) {
         path = path + "?" + *dest.query();
      }

      std::string host_str = *dest.host();
      if (dest.port()) {
         auto port = *dest.port();
         auto proto_iter = default_proto_ports.find(dest.proto());
         if (proto_iter != default_proto_ports.end() && proto_iter->second != port) {
            host_str = host_str + ":" + std::to_string(port);
         }
      }

      http::request<http::string_body> req{http::verb::post, path, 11};
      req.set(http::field::host, host_str);
      req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
      req.set(http::field::content_type, "application/json");
      req.keep_alive(true);
      req.body() = json::to_string(payload, _deadline);
      req.prepare_payload();

      auto conn_iter = get_connection(dest, deadline);
      auto eraser = make_scoped_exit([this, &conn_iter](){
         _connections.erase(conn_iter);
      });

      // Send the HTTP request to the remote host
      error_code ec = std::visit(write_request_visitor(this, req, deadline), conn_iter->second);
      FC_ASSERT(!ec, "Failed to send request: {}", ec.message());

      // This buffer is used for reading and must be persisted
      boost::beast::flat_buffer buffer;

      // Declare a container to hold the response
      http::response<http::string_body> res;

      // Receive the HTTP response
      ec = std::visit(read_response_visitor(this, buffer, res, deadline), conn_iter->second);
      FC_ASSERT(!ec, "Failed to read response: {}", ec.message());

      // if the connection can be kept open, keep it open
      if (res.keep_alive()) {
         eraser.cancel();
      }

      fc::variant result;
      if( !res.body().empty() ) {
         try {
            result = json::from_string( res.body() );
         } catch( ... ) {}
      }
      if (res.result() == http::status::internal_server_error) {
         fc::exception_ptr excp;
         try {
            auto err_var = result.get_object()["error"].get_object();
            excp = std::make_shared<fc::exception>(err_var["code"].as_int64(), err_var["name"].as_string(), err_var["what"].as_string());

            if (err_var.contains("details")) {
               for (const auto& dvar : err_var["details"].get_array()) {
                  excp->append_log(FC_LOG_MESSAGE(error, "{}", dvar.get_object()["message"].as_string()));
               }
            }
         } catch( ... ) {

         }

         if (excp) {
            excp->rethrow();
         } else {
            FC_THROW("Request failed with 500 response, but response was not parseable");
         }
      } else if (res.result() == http::status::not_found) {
         FC_THROW("URL not found: {}", (std::string)dest);
      }

      return result;
   }

   /*
      Unix URLs work a little special here. They'll originally be in the format of
      unix:///home/username/sysio-wallet/kiod.sock/v1/wallet/sign_digest
      for example. When the fc::url is given to http_client in post_sync(), this will
      have proto=unix and host=/home/username/sysio-wallet/kiod.sock/v1/wallet/sign_digest

      At this point we still don't know what part of the above string is the unix socket path
      and which part is the path to access on the server. This function discovers that
      host=/home/username/sysio-wallet/kiod.sock and path=/v1/wallet/sign_digest
      and creates another fc::url that will be used downstream of the http_client::post_sync()
      call.
   */
   const fc::url& get_unix_url(const std::string& full_url) {
      unix_url_split_map::const_iterator found = _unix_url_paths.find(full_url);
      if(found != _unix_url_paths.end())
         return found->second;

      std::filesystem::path socket_file(full_url);
      if(socket_file.is_relative())
         FC_THROW_EXCEPTION( parse_error_exception, "socket url cannot be relative ({})", socket_file.string());
      if(socket_file.empty())
         FC_THROW_EXCEPTION( parse_error_exception, "missing socket url");
      std::filesystem::path url_path;
      do {
         if(std::filesystem::is_socket(socket_file))
            break;
         url_path = socket_file.filename() / url_path;
         socket_file = socket_file.remove_filename();
      } while(!socket_file.empty());
      if(socket_file.empty())
         FC_THROW_EXCEPTION( parse_error_exception, "couldn't discover socket path");
      url_path = "/" / url_path;
      return _unix_url_paths.emplace(full_url, fc::url("unix", socket_file.string(), ostring(), ostring(), url_path.string(), ostring(), ovariant_object(), std::optional<uint16_t>())).first->second;
   }

   void post_to_file(const url& dest, const variant& payload, const std::filesystem::path& final_dest,
                     const http_file_download_options& options) {
      FC_ASSERT(dest.host(), "No host set on URL");

      std::string path = dest.path() ? dest.path()->generic_string() : "/";
      if (dest.query()) {
         path = path + "?" + *dest.query();
      }

      std::string host_str = *dest.host();
      if (dest.port()) {
         auto port = *dest.port();
         auto proto_iter = default_proto_ports.find(dest.proto());
         if (proto_iter != default_proto_ports.end() && proto_iter->second != port) {
            host_str = host_str + ":" + std::to_string(port);
         }
      }

      http::request<http::string_body> req{http::verb::post, path, 11};
      req.set(http::field::host, host_str);
      req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
      req.set(http::field::content_type, "application/json");
      req.keep_alive(false);
      req.body() = json::to_string(payload, fc::time_point::maximum());
      req.prepare_payload();

      const auto request_started = std::chrono::steady_clock::now();
      auto next_status_report = request_started;
      auto download_phase = http_file_download_phase::connecting;
      uint64_t downloaded_bytes = 0;
      std::optional<uint64_t> response_body_bytes;
      auto report_status = [&](bool force) {
         if (!options.status_callback) {
            return;
         }
         const auto now = std::chrono::steady_clock::now();
         if (!force && now < next_status_report) {
            return;
         }
         const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - request_started);
         options.status_callback(http_file_download_status{
            .phase = download_phase,
            .downloaded_bytes = downloaded_bytes,
            .total_bytes = response_body_bytes,
            .elapsed = fc::microseconds(elapsed.count()),
         });
         next_status_report = now + download_status_interval;
      };
      auto transition_to = [&](http_file_download_phase phase) {
         download_phase = phase;
         report_status(true);
      };
      operation_monitor monitor;
      if (options.status_callback) {
         monitor = [&]() { report_status(false); };
      }
      const auto no_deadline = deadline_type::max();

      bool reused_connection = false;
      transition_to(http_file_download_phase::connecting);
      auto conn_iter = get_connection(dest, no_deadline, &reused_connection, monitor);
      const auto connection_key = url_to_host_key(dest);
      auto eraser = make_scoped_exit([this, connection_key](){
         _connections.erase(connection_key);
      });

      std::unique_ptr<boost::beast::flat_buffer> buffer;
      std::unique_ptr<http::response_parser<http::buffer_body>> parser;
      error_code ec;
      bool retried_stale_connection = false;
      while (true) {
         transition_to(http_file_download_phase::sending_request);
         ec = std::visit(
            write_request_visitor(this, req, no_deadline, monitor), conn_iter->second);
         if (ec) {
            if (options.retry_failed_reused_connection && reused_connection && !retried_stale_connection &&
                is_stale_connection_error(ec)) {
               _connections.erase(conn_iter);
               transition_to(http_file_download_phase::connecting);
               conn_iter = get_connection(dest, no_deadline, &reused_connection, monitor);
               retried_stale_connection = true;
               continue;
            }
            FC_ASSERT(false, "Failed to send POST request: {}", ec.message());
         }

         // Parse into a bounded caller-owned buffer so every write can enforce the byte ceiling,
         // cancellation checks, and filesystem headroom before bytes reach disk.
         buffer = std::make_unique<boost::beast::flat_buffer>(download_parser_buffer_bytes);
         parser = std::make_unique<http::response_parser<http::buffer_body>>();
         parser->body_limit(options.max_response_body_bytes);

         transition_to(http_file_download_phase::waiting_for_response);
         ec = std::visit([&](auto& stream) {
            return sync_read_header_with_timeout(*stream, *buffer, *parser, no_deadline, monitor);
         }, conn_iter->second);
         if (!ec) {
            break;
         }
         if (ec == http::error::body_limit) {
            FC_THROW("HTTP response body exceeds configured maximum of {} bytes", options.max_response_body_bytes);
         }
         if (options.retry_failed_reused_connection && reused_connection && !retried_stale_connection &&
             is_stale_connection_error(ec)) {
            _connections.erase(conn_iter);
            transition_to(http_file_download_phase::connecting);
            conn_iter = get_connection(dest, no_deadline, &reused_connection, monitor);
            retried_stale_connection = true;
            continue;
         }
         FC_ASSERT(false, "Failed to read response header: {}", ec.message());
      }

      // Require exactly 200 OK: this client never sends Range requests, so 206 Partial Content
      // can only come from a broken or hostile endpoint/proxy, and renaming a partial body into
      // the final destination would hand the caller a truncated file.
      const auto status = parser->get().result();
      if (status != http::status::ok) {
         // Preserve a useful endpoint diagnostic without buffering an unbounded error response.
         // One parser increment is enough for the first 200 decoded bytes and remains cancellable.
         std::string error_body(max_error_response_body_bytes, '\0');
         if (!parser->is_done()) {
            error_body.resize(read_body_increment(conn_iter->second, *buffer, *parser, error_body.data(),
                                                  error_body.size(), monitor, ec));
         } else {
            error_body.clear();
         }
         if (error_body.empty()) {
            FC_THROW("HTTP POST failed with status {}", parser->get().result_int());
         }
         FC_THROW("HTTP POST failed with status {}: {}", parser->get().result_int(), error_body);
      }

      const auto content_length = parser->content_length();
      if (content_length) {
         response_body_bytes = *content_length;
      }
      uint64_t disk_space_write_budget = 0;
      if (content_length) {
         FC_ASSERT(*content_length <= std::numeric_limits<uint64_t>::max() - disk_space_concurrency_margin_bytes,
                   "HTTP response Content-Length {} is too large for the {}-byte disk safety margin",
                   *content_length, disk_space_concurrency_margin_bytes);
         const auto preflight_bytes = *content_length == 0
                                         ? 0
                                         : *content_length + disk_space_concurrency_margin_bytes;
         require_disk_space(final_dest, preflight_bytes, options.min_free_disk_space_bytes);
         disk_space_write_budget = std::min(*content_length, disk_space_check_interval_bytes);
      } else {
         disk_space_write_budget = refresh_disk_space_write_budget(
            final_dest, options.max_response_body_bytes, options.min_free_disk_space_bytes);
      }
      auto next_disk_space_check = std::chrono::steady_clock::now() + disk_space_check_interval;

      // Write to temp file then rename
      auto temp_path = final_dest;
      temp_path += ".downloading";
      auto cleanup = make_scoped_exit([&temp_path](){
         std::error_code rm_ec;
         std::filesystem::remove(temp_path, rm_ec);
      });

      std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
      FC_ASSERT(file.is_open(), "Failed to open temp file {} for writing", temp_path.string());

      std::vector<char> read_buffer(download_read_buffer_bytes);
      transition_to(http_file_download_phase::downloading);
      while (!parser->is_done()) {
         const auto chunk_bytes = read_body_increment(conn_iter->second, *buffer, *parser, read_buffer.data(),
                                                      read_buffer.size(), monitor, ec);
         if (ec == http::error::body_limit) {
            FC_THROW("HTTP response body exceeds configured maximum of {} bytes", options.max_response_body_bytes);
         }
         FC_ASSERT(!ec, "Failed to read response body: {}", ec.message());

         FC_ASSERT(chunk_bytes <= options.max_response_body_bytes &&
                      downloaded_bytes <= options.max_response_body_bytes - chunk_bytes,
                   "HTTP response body exceeds configured maximum of {} bytes", options.max_response_body_bytes);
         if (chunk_bytes == 0) {
            report_status(false);
            continue;
         }

         // Amortize filesystem-space queries while bounding concurrent-consumption exposure with
         // a separate safety margin. Recheck after 64 MiB or five seconds, whichever comes first.
         if (disk_space_write_budget < chunk_bytes ||
             std::chrono::steady_clock::now() >= next_disk_space_check) {
            const auto response_size_limit = content_length.value_or(options.max_response_body_bytes);
            const auto remaining_body_bytes = response_size_limit - downloaded_bytes;
            disk_space_write_budget = refresh_disk_space_write_budget(
               final_dest, remaining_body_bytes, options.min_free_disk_space_bytes);
            next_disk_space_check = std::chrono::steady_clock::now() + disk_space_check_interval;
         }
         file.write(read_buffer.data(), static_cast<std::streamsize>(chunk_bytes));
         FC_ASSERT(file.good(), "Failed to write {} bytes to temp file {}", chunk_bytes, temp_path.string());
         downloaded_bytes += chunk_bytes;
         disk_space_write_budget -= chunk_bytes;
         report_status(false);
      }

      FC_ASSERT(!is_cancelled(), "HTTP file download cancelled");
      file.close();
      FC_ASSERT(file.good(), "Failed to close temp file {} after HTTP download", temp_path.string());

      std::error_code rename_ec;
      std::filesystem::rename(temp_path, final_dest, rename_ec);
      FC_ASSERT(!rename_ec, "Failed to rename downloaded file: {}", rename_ec.message());
      cleanup.cancel();
      transition_to(http_file_download_phase::complete);
   }

   boost::asio::io_context  _ioc;
   connection_map           _connections;
   unix_url_split_map       _unix_url_paths;
   std::function<bool()>     _cancel_check;
};


http_client::http_client()
:_my(new http_client_impl())
{

}

void http_client::post_to_file(const url& dest, const variant& payload, const std::filesystem::path& output,
                               const http_file_download_options& options) {
   _my->post_to_file(dest, payload, output, options);
}

void http_client::set_cancel_check(std::function<bool()> cancel_check) {
   _my->set_cancel_check(std::move(cancel_check));
}

variant http_client::post_sync(const url& dest, const variant& payload, const fc::time_point& deadline) {
   if(dest.proto() == "unix")
      return _my->post_sync(_my->get_unix_url(*dest.host()), payload, deadline);
   else
      return _my->post_sync(dest, payload, deadline);
}

void http_client::add_cert(const std::string& cert_pem_string) {
   _my->add_cert(cert_pem_string);
}

void http_client::set_verify_peers(bool enabled) {
   _my->set_verify_peers(enabled);
}

http_client::~http_client() {

}

}
