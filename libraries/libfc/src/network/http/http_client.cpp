#include <fc/network/http/http_client.hpp>
#include <fc/io/json.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/static_variant.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/local/stream_protocol.hpp>

using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
namespace http = boost::beast::http;    // from <boost/beast/http.hpp>
namespace local = boost::asio::local;

namespace fc {

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
   using deadline_type = boost::posix_time::ptime;

   http_client_impl()
   :_ioc()
   {
      set_verify_peers(true);
   }

   void add_cert(const std::string& cert_pem_string) {}

   void set_verify_peers(bool enabled) {}

   template<typename SyncReadStream, typename Fn, typename CancelFn>
   error_code sync_do_with_deadline( SyncReadStream& s, deadline_type deadline, Fn f, CancelFn cf ) {
      bool timer_expired = false;
      boost::asio::deadline_timer timer(_ioc);

      timer.expires_at(deadline);
      bool timer_cancelled = false;
      timer.async_wait([&timer_expired, &timer_cancelled] (const error_code&) {
         // the only non-success error_code this is called with is operation_aborted but since
         // we could have queued "success" when we cancelled the timer, we set a flag at the
         // safer scope and only respect that.
         if (!timer_cancelled) {
            timer_expired = true;
         }
      });

      std::optional<error_code> f_result;
      f(f_result);

      _ioc.restart();
      while (_ioc.run_one())
      {
         if (f_result) {
            timer_cancelled = true;
            timer.cancel();
         } else if (timer_expired) {
            cf();
         }
      }

      if (!timer_expired) {
         return *f_result;
      } else {
         return error_code(boost::system::errc::timed_out, boost::system::system_category());
      }
   }

   template<typename SyncReadStream, typename Fn>
   error_code sync_do_with_deadline( SyncReadStream& s, deadline_type deadline, Fn f) {
      return sync_do_with_deadline(s, deadline, f, [&s](){
         s.lowest_layer().cancel();
      });
   };

   template<typename SyncReadStream>
   error_code sync_connect_with_timeout( SyncReadStream& s, const std::string& host, const std::string& port,  const deadline_type& deadline ) {
      tcp::resolver local_resolver(_ioc);
      bool cancelled = false;

      auto res = sync_do_with_deadline(s, deadline, [&local_resolver, &cancelled, &s, &host, &port](std::optional<error_code>& final_ec){
         local_resolver.async_resolve(host, port, [&cancelled, &s, &final_ec](const error_code& ec, tcp::resolver::results_type resolved ){
            if (ec) {
               final_ec.emplace(ec);
               return;
            }

            if (!cancelled) {
               boost::asio::async_connect(s, resolved.begin(), resolved.end(), [&final_ec](const error_code& ec, tcp::resolver::iterator ){
                  final_ec.emplace(ec);
               });
            }
         });
      },[&local_resolver, &cancelled](){
         cancelled = true;
         local_resolver.cancel();
      });

      return res;
   };

   template<typename SyncReadStream>
   error_code sync_write_with_timeout(SyncReadStream& s, http::request<http::string_body>& req, const deadline_type& deadline ) {
      return sync_do_with_deadline(s, deadline, [&s, &req](std::optional<error_code>& final_ec){
         http::async_write(s, req, [&final_ec]( const error_code& ec, std::size_t ) {
            final_ec.emplace(ec);
         });
      });
   }

   template<typename SyncReadStream>
   error_code sync_read_with_timeout(SyncReadStream& s, boost::beast::flat_buffer& buffer, http::response<http::string_body>& res, const deadline_type& deadline ) {
      return sync_do_with_deadline(s, deadline, [&s, &buffer, &res](std::optional<error_code>& final_ec){
         http::async_read(s, buffer, res, [&final_ec]( const error_code& ec, std::size_t ) {
            final_ec.emplace(ec);
         });
      });
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
      FC_ASSERT(!ec, "Failed to connect: ${message}", ("message",ec.message()));

      auto res = _connections.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(key),
                                      std::forward_as_tuple(std::move(socket)));

      return res.first;
   }

   connection_map::iterator create_raw_connection( const url& dest, const deadline_type& deadline ) {
      auto key = url_to_host_key(dest);
      auto socket = std::make_unique<tcp::socket>(_ioc);

      error_code ec = sync_connect_with_timeout(*socket, *dest.host(), dest.port() ? std::to_string(*dest.port()) : "80", deadline);
      FC_ASSERT(!ec, "Failed to connect: ${message}", ("message",ec.message()));

      auto res = _connections.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(key),
                                      std::forward_as_tuple(std::move(socket)));

      return res.first;
   }

   connection_map::iterator create_connection( const url& dest, const deadline_type& deadline ) {
      if (dest.proto() == "http") {
         return create_raw_connection(dest, deadline);
      } else if (dest.proto() == "unix") {
         return create_unix_connection(dest, deadline);
      } else {
         FC_THROW("Unknown protocol ${proto}", ("proto", dest.proto()));
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

   connection_map::iterator get_connection( const url& dest, const deadline_type& deadline ) {
      auto key = url_to_host_key(dest);
      auto conn_itr = _connections.find(key);
      if (conn_itr == _connections.end() || check_closed(conn_itr)) {
         return create_connection(dest, deadline);
      } else {
         return conn_itr;
      }
   }

   struct write_request_visitor : visitor<error_code> {
      write_request_visitor(http_client_impl* that, http::request<http::string_body>& req, const deadline_type& deadline)
      :that(that)
      ,req(req)
      ,deadline(deadline)
      {}

      template<typename S>
      error_code operator() ( S& stream ) const {
         return that->sync_write_with_timeout(*stream, req, deadline);
      }

      http_client_impl*                 that;
      http::request<http::string_body>& req;
      const deadline_type&              deadline;
   };

   struct read_response_visitor : visitor<error_code> {
      read_response_visitor(http_client_impl* that, boost::beast::flat_buffer& buffer, http::response<http::string_body>& res, const deadline_type& deadline)
      :that(that)
      ,buffer(buffer)
      ,res(res)
      ,deadline(deadline)
      {}

      template<typename S>
      error_code operator() ( S& stream ) const {
         return that->sync_read_with_timeout(*stream, buffer, res, deadline);
      }

      http_client_impl*                  that;
      boost::beast::flat_buffer&         buffer;
      http::response<http::string_body>& res;
      const deadline_type&               deadline;
   };

   variant post_sync(const url& dest, const variant& payload, const fc::time_point& _deadline) {
      static const deadline_type epoch(boost::gregorian::date(1970, 1, 1));
      auto deadline = epoch + boost::posix_time::microseconds(_deadline.time_since_epoch().count());
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
      FC_ASSERT(!ec, "Failed to send request: ${message}", ("message",ec.message()));

      // This buffer is used for reading and must be persisted
      boost::beast::flat_buffer buffer;

      // Declare a container to hold the response
      http::response<http::string_body> res;

      // Receive the HTTP response
      ec = std::visit(read_response_visitor(this, buffer, res, deadline), conn_iter->second);
      FC_ASSERT(!ec, "Failed to read response: ${message}", ("message",ec.message()));

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
                  excp->append_log(FC_LOG_MESSAGE(error, dvar.get_object()["message"].as_string()));
               }
            }
         } catch( ... ) {

         }

         if (excp) {
            throw *excp;
         } else {
            FC_THROW("Request failed with 500 response, but response was not parseable");
         }
      } else if (res.result() == http::status::not_found) {
         FC_THROW("URL not found: ${url}", ("url", (std::string)dest));
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
         FC_THROW_EXCEPTION( parse_error_exception, "socket url cannot be relative (${url})", ("url", socket_file.string()));
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

   boost::asio::io_context  _ioc;
   connection_map           _connections;
   unix_url_split_map       _unix_url_paths;
};


http_client::http_client()
:_my(new http_client_impl())
{

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
