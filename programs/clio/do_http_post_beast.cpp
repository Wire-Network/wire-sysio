#include "do_http_post.hpp"
#include <sysio/chain/exceptions.hpp>

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace sysio { namespace client { namespace http {

namespace {
   namespace beast = boost::beast;
   namespace asio  = boost::asio;
   namespace bhttp = boost::beast::http;
   using tcp       = asio::ip::tcp;
   namespace local = asio::local;

   constexpr std::string_view scheme_http      = "http://";
   constexpr std::string_view scheme_https     = "https://";
   constexpr std::string_view scheme_unix      = "unix://";
   constexpr uint16_t         default_http_port  = 80;
   constexpr uint16_t         default_https_port = 443;
   constexpr std::string_view content_type_json = "application/json";

   /// HTTP/1.1, matching curl's CURLOPT_HTTP_VERSION setting in the prior implementation.
   constexpr int http_version_1_1 = 11;

   /// Synthetic Host header value used when talking over a unix-domain socket;
   /// matches the prior libcurl implementation which rewrote the URL to
   /// `http://localhost<path>` whenever CURLOPT_UNIX_SOCKET_PATH was set.
   constexpr std::string_view unix_host_header = "localhost";

   enum class transport { http, https, unix_sock };

   /// Result of splitting a clio --url value into the parts the Beast layer needs.
   /// `host` holds either a DNS hostname (http/https) or the unix socket path.
   /// `path_prefix` preserves any path component the user included on --url
   /// (e.g. `--url http://example.com/api`) so that requests still hit
   /// `/api/v1/chain/...` rather than `/v1/chain/...`.
   struct parsed_target {
      transport   xport;
      std::string host;
      uint16_t    port;
      std::string path_prefix;
   };

   parsed_target parse_base_uri(const std::string& base_uri) {
      auto starts_with = [&](std::string_view s) {
         return base_uri.size() >= s.size() &&
                base_uri.compare(0, s.size(), s.data(), s.size()) == 0;
      };

      parsed_target out{};
      if (starts_with(scheme_unix)) {
         out.xport = transport::unix_sock;
         out.host  = base_uri.substr(scheme_unix.size());
         SYS_ASSERT(!out.host.empty(), connection_exception,
                    "Empty unix socket path in URL: {}", base_uri);
         return out;
      }

      std::string_view rest;
      if (starts_with(scheme_https)) {
         out.xport = transport::https;
         out.port  = default_https_port;
         rest      = std::string_view(base_uri).substr(scheme_https.size());
      } else if (starts_with(scheme_http)) {
         out.xport = transport::http;
         out.port  = default_http_port;
         rest      = std::string_view(base_uri).substr(scheme_http.size());
      } else {
         SYS_THROW(connection_exception,
                   "Unsupported URL scheme in `{}` (expected http://, https://, or unix://)",
                   base_uri);
      }

      const auto slash = rest.find('/');
      const auto authority = rest.substr(0, slash);
      SYS_ASSERT(!authority.empty(), connection_exception,
                 "URL `{}` has no host component", base_uri);

      const auto colon = authority.find(':');
      if (colon == std::string_view::npos) {
         out.host = std::string(authority);
      } else {
         out.host = std::string(authority.substr(0, colon));
         const auto port_sv = authority.substr(colon + 1);
         try {
            const auto port_val = std::stoi(std::string(port_sv));
            SYS_ASSERT(port_val > 0 && port_val <= 65535, connection_exception,
                       "Port out of range in `{}`", base_uri);
            out.port = static_cast<uint16_t>(port_val);
         } catch (const std::exception&) {
            SYS_THROW(connection_exception, "Could not parse port from `{}`", base_uri);
         }
      }

      if (slash != std::string_view::npos) {
         out.path_prefix = std::string(rest.substr(slash));
         // If the prefix is just "/", drop it so we don't double-up the leading slash
         // when concatenating with `path` (which itself always starts with '/').
         if (out.path_prefix == "/") {
            out.path_prefix.clear();
         }
      }
      return out;
   }

   /// Build the Host header value, omitting the port when it matches the default
   /// for the scheme (matches the libcurl implementation's behavior).
   std::string make_host_header(const parsed_target& t) {
      if (t.xport == transport::unix_sock) {
         return std::string(unix_host_header);
      }
      const bool is_default = (t.xport == transport::http  && t.port == default_http_port)
                           || (t.xport == transport::https && t.port == default_https_port);
      if (is_default) {
         return t.host;
      }
      return t.host + ":" + std::to_string(t.port);
   }

   /// Apply caller-supplied `Name: Value` headers to the request. Inputs are taken
   /// verbatim from clio's `--header` flag, so silently skip malformed entries
   /// rather than aborting the call (matches the prior libcurl behavior, which
   /// passed the raw line through curl_slist_append).
   void apply_extra_headers(bhttp::request<bhttp::string_body>& req,
                            const std::vector<std::string>& headers) {
      for (const auto& h : headers) {
         const auto pos = h.find(':');
         if (pos == std::string::npos) continue;
         const auto name = h.substr(0, pos);
         auto value_start = pos + 1;
         while (value_start < h.size() &&
                (h[value_start] == ' ' || h[value_start] == '\t')) {
            ++value_start;
         }
         req.set(name, h.substr(value_start));
      }
   }

   bhttp::request<bhttp::string_body>
   build_request(const parsed_target& t, const std::string& path,
                 const std::vector<std::string>& headers,
                 const std::string& postjson) {
      bhttp::request<bhttp::string_body> req{
         bhttp::verb::post, t.path_prefix + path, http_version_1_1};
      req.set(bhttp::field::host,         make_host_header(t));
      req.set(bhttp::field::user_agent,   BOOST_BEAST_VERSION_STRING);
      req.set(bhttp::field::content_type, content_type_json);
      apply_extra_headers(req, headers);
      req.body() = postjson;
      req.prepare_payload();
      return req;
   }

   /// Mirror of curl's CURLOPT_VERBOSE / CURLOPT_DEBUGFUNCTION output. Beast's
   /// stream insertion operators emit headers + body in human-readable form;
   /// for `trace` mode we currently produce the same output as `verbose` rather
   /// than a byte-level hex dump (regression vs. the libcurl path, accepted for
   /// the initial migration).
   void log_io(const bhttp::request<bhttp::string_body>& req,
               const bhttp::response<bhttp::string_body>& res) {
      std::cerr << req << res;
   }

   template<typename Stream>
   std::tuple<unsigned int, std::string>
   write_and_read(Stream& stream,
                  const bhttp::request<bhttp::string_body>& req,
                  bool verbose, bool trace) {
      bhttp::write(stream, req);

      beast::flat_buffer                       buffer;
      bhttp::response_parser<bhttp::string_body> parser;
      // Disable Beast's 1 MiB default — clio routinely fetches larger payloads
      // (e.g. get_block) and curl had no body cap.
      parser.body_limit(boost::none);
      bhttp::read(stream, buffer, parser);

      auto& res = parser.get();
      if (verbose || trace) {
         log_io(req, res);
      }
      const auto status = static_cast<unsigned int>(res.result_int());
      return { status, std::move(res.body()) };
   }

   std::tuple<unsigned int, std::string>
   do_post_unix(const parsed_target& t, const std::string& path,
                const std::vector<std::string>& headers,
                const std::string& postjson, bool verbose, bool trace) {
      asio::io_context                 ioc;
      local::stream_protocol::socket   socket(ioc);
      socket.connect(local::stream_protocol::endpoint(t.host));

      const auto req = build_request(t, path, headers, postjson);
      return write_and_read(socket, req, verbose, trace);
   }

   std::tuple<unsigned int, std::string>
   do_post_http(const parsed_target& t, const std::string& path,
                const std::vector<std::string>& headers,
                const std::string& postjson, bool verbose, bool trace) {
      asio::io_context  ioc;
      tcp::resolver     resolver(ioc);
      const auto        endpoints = resolver.resolve(t.host, std::to_string(t.port));

      beast::tcp_stream stream(ioc);
      stream.connect(endpoints);

      const auto req = build_request(t, path, headers, postjson);
      auto       result = write_and_read(stream, req, verbose, trace);

      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);
      // ignore shutdown errors; the response is already in hand
      return result;
   }

   std::tuple<unsigned int, std::string>
   do_post_https(const parsed_target& t, const std::string& path,
                 const std::vector<std::string>& headers,
                 const std::string& postjson,
                 bool verify_cert, bool verbose, bool trace) {
      asio::io_context   ioc;
      asio::ssl::context ssl_ctx{asio::ssl::context::tlsv12_client};
      ssl_ctx.set_default_verify_paths();

      tcp::resolver resolver(ioc);
      const auto    endpoints = resolver.resolve(t.host, std::to_string(t.port));

      beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
      if (verify_cert) {
         stream.set_verify_mode(asio::ssl::verify_peer);
         stream.set_verify_callback(asio::ssl::host_name_verification(t.host));
      } else {
         stream.set_verify_mode(asio::ssl::verify_none);
      }
      if (!SSL_set_tlsext_host_name(stream.native_handle(), t.host.c_str())) {
         throw beast::system_error(
            beast::error_code(static_cast<int>(::ERR_get_error()),
                              asio::error::get_ssl_category()));
      }

      beast::get_lowest_layer(stream).connect(endpoints);
      stream.handshake(asio::ssl::stream_base::client);

      const auto req = build_request(t, path, headers, postjson);
      auto       result = write_and_read(stream, req, verbose, trace);

      beast::error_code ec;
      stream.shutdown(ec);
      // eof / stream_truncated are benign — many servers close without a clean
      // TLS shutdown (matches the wire_eth_maintenance_plugin handling).
      return result;
   }

   /// Map Asio/Beast connection-class failures to the connection_exception that
   /// the libcurl path used to surface (CURLE_COULDNT_CONNECT / URL_MALFORMAT).
   /// Anything else becomes a generic chain::http_exception.
   [[noreturn]] void rethrow_as_clio_exception(const boost::system::system_error& e) {
      const auto& ec = e.code();
      if (ec == asio::error::connection_refused
       || ec == asio::error::host_not_found
       || ec == asio::error::host_not_found_try_again
       || ec == asio::error::network_unreachable
       || ec == asio::error::host_unreachable
       || ec == asio::error::no_data
       || ec == asio::error::timed_out) {
         SYS_THROW(connection_exception, "{}", e.what());
      }
      SYS_THROW(chain::http_exception, "{}", e.what());
   }
}

std::tuple<unsigned int, std::string> do_http_post(const std::string&              base_uri,
                                                   const std::string&              path,
                                                   const std::vector<std::string>& headers,
                                                   const std::string&              postjson,
                                                   bool                            verify_cert,
                                                   bool                            verbose,
                                                   bool                            trace) {
   const auto target = parse_base_uri(base_uri);
   try {
      switch (target.xport) {
         case transport::unix_sock:
            return do_post_unix(target, path, headers, postjson, verbose, trace);
         case transport::http:
            return do_post_http(target, path, headers, postjson, verbose, trace);
         case transport::https:
            return do_post_https(target, path, headers, postjson, verify_cert, verbose, trace);
      }
   } catch (const boost::system::system_error& e) {
      rethrow_as_clio_exception(e);
   }
   SYS_THROW(chain::http_exception, "unreachable transport in do_http_post");
}

}}} // namespace sysio::client::http
