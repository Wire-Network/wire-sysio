#include "_http_transport_internal.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/version.hpp>

#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fc {
namespace http {

namespace {

/** Return whether @p directory contains a parseable OpenSSL hashed CA entry. */
bool contains_hashed_ca_certificate(const std::filesystem::path& directory) {
   std::error_code iteration_error;
   std::filesystem::directory_iterator entries(directory, iteration_error);
   if (iteration_error)
      return false;

   for (const auto& entry : entries) {
      std::error_code type_error;
      if (!entry.is_regular_file(type_error) || type_error)
         continue;
      const auto name = entry.path().filename().string();
      if (name.size() < 10 || name[8] != '.' ||
          !std::all_of(name.begin(), name.begin() + 8,
                       [](unsigned char character) { return std::isxdigit(character); }) ||
          !std::all_of(name.begin() + 9, name.end(), [](unsigned char character) { return std::isdigit(character); })) {
         continue;
      }

      ERR_clear_error();
      std::unique_ptr<BIO, decltype(&BIO_free)> input(BIO_new_file(entry.path().c_str(), "r"), &BIO_free);
      if (!input)
         continue;
      std::unique_ptr<X509, decltype(&X509_free)> certificate(PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr),
                                                              &X509_free);
      if (certificate) {
         ERR_clear_error();
         return true;
      }
   }
   ERR_clear_error();
   return false;
}

} // namespace

client_impl::client_impl(asio::any_io_executor executor, transport_options options_in,
                         detail::resolver_start_fn resolver_start_in)
   : strand(asio::make_strand(std::move(executor)))
   , options(std::move(options_in))
   , tls_context(asio::ssl::context::tls_client)
   , resolver_start(std::move(resolver_start_in)) {
   FC_ASSERT(!options.dns_cache_timeout || options.dns_cache_timeout->count() >= 0,
             "Outbound HTTP DNS cache timeout cannot be negative");
   FC_ASSERT(options.max_idle_connection_age.count() > 0, "Outbound HTTP idle connection age must be positive");
   tls_context.set_verify_mode(asio::ssl::verify_peer);
   error_code error;
   tls_context.set_default_verify_paths(error);
   FC_ASSERT(!error, "Outbound HTTPS failed to load the system trust roots: {}", error.message());

   const auto ca_file = options.additional_ca_file ? options.additional_ca_file->c_str() : nullptr;
   const auto ca_path = options.additional_ca_path ? options.additional_ca_path->c_str() : nullptr;
   if (options.additional_ca_path) {
      FC_ASSERT(contains_hashed_ca_certificate(*options.additional_ca_path),
                "Outbound HTTPS additional CA directory is empty, malformed, or unreadable");
   }
   if (ca_file || ca_path) {
      ERR_clear_error();
      FC_ASSERT(SSL_CTX_load_verify_locations(tls_context.native_handle(), ca_file, ca_path) == 1,
                "Outbound HTTPS additional CA configuration is malformed or unreadable");
      ERR_clear_error();
   }

   if (options.proxy) {
      const fc::url parsed(*options.proxy);
      FC_ASSERT(parsed.proto() == scheme_http, "Outbound HTTP proxy must use the http scheme");
      FC_ASSERT(parsed.host() && !parsed.host()->empty(), "Outbound HTTP proxy URL is missing a host");
      FC_ASSERT(is_safe_network_host(*parsed.host()), "Outbound HTTP proxy URL has an invalid host");
      FC_ASSERT(!parsed.user() && !parsed.pass(), "Outbound HTTP proxy credentials are not supported");
      proxy_host = *parsed.host();
      proxy_service = parsed.port() ? std::to_string(*parsed.port()) : std::string(default_http_service);
   }
   if (!resolver_start)
      resolver_start = start_platform_resolution;
}

client_impl::~client_impl() {
   for (auto& [key, connections] : idle_connections) {
      (void)key;
      for (auto& connection : connections)
         connection.connection->close();
   }
}

client_impl::target_info client_impl::normalize_target(const url& target) {
   FC_ASSERT(target.host(), "Outbound HTTP URL is missing a host");
   target_info result{
      .scheme = target.proto(),
      .host = *target.host(),
   };
   if (result.scheme == scheme_unix) {
      if (auto found = unix_target_cache.find(result.host); found != unix_target_cache.end()) {
         result = found->second;
         if (target.query())
            result.request_target += "?" + *target.query();
         return result;
      }

      const auto complete_path = std::filesystem::path(result.host);
      std::filesystem::path socket_file(result.host);
      FC_ASSERT(socket_file.is_absolute(), "Unix-socket URL cannot be relative");
      while (!socket_file.empty()) {
         std::error_code socket_error;
         if (std::filesystem::is_socket(socket_file, socket_error) && !socket_error) {
            break;
         }
         const auto parent = socket_file.parent_path();
         if (parent.empty() || parent == socket_file) {
            socket_file.clear();
            break;
         }
         socket_file = parent;
      }
      if (socket_file.empty()) {
         throw transport_failure(failure_kind::connect, "Unix socket path does not exist");
      }
      result.unix_socket_path = socket_file.string();
      result.host = "localhost";
      result.host_header = "localhost";
      const auto request_path = complete_path.lexically_relative(socket_file);
      const bool root_request =
         request_path.empty() || request_path == std::filesystem::path{filesystem_current_directory};
      result.request_target = root_request ? "/" : "/" + request_path.generic_string();
      result.connection_key = "unix|" + *result.unix_socket_path;
      unix_target_cache.emplace(*target.host(), result);
   } else {
      FC_ASSERT(result.scheme == scheme_http || result.scheme == scheme_https,
                "Unsupported outbound HTTP URL scheme: {}", result.scheme);
      if (!is_safe_network_host(result.host)) {
         throw transport_failure(failure_kind::request_limit, "request URL has an invalid host");
      }
      result.tls = result.scheme == scheme_https;
      result.service = target.port() ? std::to_string(*target.port())
                                     : std::string(result.tls ? default_https_service : default_http_service);
      result.host_header = authority_host(result.host);
      const bool default_port = !target.port() || (result.tls && *target.port() == default_https_port) ||
                                (!result.tls && *target.port() == default_http_port);
      if (!default_port)
         result.host_header += ":" + result.service;
      result.request_target = target.path() ? target.path()->generic_string() : "/";
      if (result.request_target.empty())
         result.request_target = "/";
      result.connection_key = result.scheme + "|" + result.host + "|" + result.service;
   }
   if (target.query())
      result.request_target += "?" + *target.query();
   return result;
}

asio::awaitable<void> client_impl::connect_tcp(const std::shared_ptr<connection_state>& connection,
                                               connection_state::tcp_stream& stream,
                                               const std::vector<tcp::endpoint>& endpoints, const std::string& host,
                                               const std::string& service, std::optional<operation_deadline> deadline,
                                               const std::shared_ptr<request_control>& control) {
   auto cancel_guard = begin_operation(connection, stream, deadline, control);
   error_code error;
   (void)co_await stream.async_connect(endpoints, asio::redirect_error(asio::use_awaitable, error));
   throw_if_operation_failed(error, deadline, control);
   if (error) {
      if (options.refresh_dns_on_connection_failure)
         dns_cache.erase(host + "|" + service);
      throw transport_failure(failure_kind::connect, "Failed to connect: " + error.message(), true);
   }
}

asio::awaitable<void> client_impl::establish_proxy_tunnel(const std::shared_ptr<connection_state>& connection,
                                                          connection_state::tcp_stream& stream,
                                                          const target_info& target, const request_options& policy,
                                                          std::optional<operation_deadline> connect_deadline,
                                                          const std::shared_ptr<request_control>& control) {
   const auto connect_authority = authority_host(target.host) + ":" + target.service;
   beast_http::request<beast_http::empty_body> connect_request{beast_http::verb::connect, connect_authority,
                                                               http_version_1_1};
   connect_request.set(beast_http::field::host, connect_authority);
   connect_request.set(beast_http::field::user_agent, BOOST_BEAST_VERSION_STRING);
   co_await write_request(connection, stream, connect_request, connect_deadline, control);

   beast::flat_buffer buffer(policy.max_response_header_bytes);
   // A proxy may send an interim response before 200 Connection Established, so this shares the
   // request path's skip loop rather than treating the first header as the tunnel's answer. The
   // buffer is retained across parser restarts because the following head may already be in it.
   std::optional<beast_http::response_parser<beast_http::empty_body>> parser;
   restart_response_parser(parser, policy);
   co_await read_final_header(connection, stream, buffer, parser, policy, connect_deadline, control);
   if (parser->get().result() != beast_http::status::ok) {
      throw transport_failure(failure_kind::connect,
                              "proxy tunnel failed with HTTP status " + std::to_string(parser->get().result_int()));
   }
}

asio::awaitable<std::shared_ptr<connection_state>>
client_impl::create_connection(const target_info& target, const request_options& policy,
                               const std::optional<time_point>& total_deadline,
                               const std::shared_ptr<request_control>& control) {
   const auto connect_deadline = phase_deadline(policy.timeouts.connect, failure_kind::timeout_connect, total_deadline);
   if (target.scheme == scheme_unix) {
      auto connection = std::make_shared<connection_state>(
         connection_state::stream_variant{std::make_unique<connection_state::unix_stream>(strand)});
      auto& stream = *std::get<std::unique_ptr<connection_state::unix_stream>>(connection->stream);
      auto cancel_guard = begin_operation(connection, stream, connect_deadline, control);
      error_code error;
      co_await stream.async_connect(local::stream_protocol::endpoint(*target.unix_socket_path),
                                    asio::redirect_error(asio::use_awaitable, error));
      throw_if_operation_failed(error, connect_deadline, control);
      if (error) {
         throw transport_failure(failure_kind::connect, "Unix-socket connection failed: " + error.message(), true);
      }
      co_return connection;
   }

   const auto connect_host = proxy_host.value_or(target.host);
   const auto connect_service = proxy_service.value_or(target.service);
   const auto endpoints = co_await resolve(connect_host, connect_service, connect_deadline, control);

   if (!target.tls) {
      auto connection = std::make_shared<connection_state>(
         connection_state::stream_variant{std::make_unique<connection_state::tcp_stream>(strand)});
      auto& stream = *std::get<std::unique_ptr<connection_state::tcp_stream>>(connection->stream);
      co_await connect_tcp(connection, stream, endpoints, connect_host, connect_service, connect_deadline, control);
      co_return connection;
   }

   auto connection = std::make_shared<connection_state>(
      connection_state::stream_variant{std::make_unique<connection_state::tls_stream>(strand, tls_context)});
   auto& stream = *std::get<std::unique_ptr<connection_state::tls_stream>>(connection->stream);
   co_await connect_tcp(connection, stream.next_layer(), endpoints, connect_host, connect_service, connect_deadline,
                        control);
   if (proxy_host) {
      co_await establish_proxy_tunnel(connection, stream.next_layer(), target, policy, connect_deadline, control);
   }

   error_code address_error;
   asio::ip::make_address(target.host, address_error);
   const bool ip_literal = !address_error;
   if (!ip_literal) {
      ERR_clear_error();
      if (SSL_set_tlsext_host_name(stream.native_handle(), target.host.c_str()) != 1) {
         ERR_clear_error();
         throw transport_failure(failure_kind::tls_handshake, "failed to configure TLS server name");
      }
   }
   stream.set_verify_mode(asio::ssl::verify_peer);
   auto identity_failure = std::make_shared<bool>(false);
   auto chain_failure = std::make_shared<bool>(false);
   auto verification_observed = std::make_shared<bool>(false);
   stream.set_verify_callback([verify_identity = asio::ssl::host_name_verification(target.host), identity_failure,
                               chain_failure,
                               verification_observed](bool preverified, asio::ssl::verify_context& context) mutable {
      *verification_observed = true;
      if (!preverified) {
         *chain_failure = true;
         return false;
      }
      const bool verified = verify_identity(preverified, context);
      if (!verified && X509_STORE_CTX_get_error_depth(context.native_handle()) == 0) {
         *identity_failure = true;
      }
      return verified;
   });

   auto cancel_guard = begin_operation(connection, stream, connect_deadline, control);
   error_code error;
   co_await stream.async_handshake(asio::ssl::stream_base::client, asio::redirect_error(asio::use_awaitable, error));
   throw_if_operation_failed(error, connect_deadline, control);
   if (error) {
      const auto failure =
         *identity_failure ? (ip_literal ? failure_kind::tls_ip : failure_kind::tls_hostname)
         : (*verification_observed && (*chain_failure || SSL_get_verify_result(stream.native_handle()) != X509_V_OK))
            ? failure_kind::tls_verification
            : failure_kind::tls_handshake;
      throw transport_failure(failure, "TLS handshake or peer verification failed");
   }
   co_return connection;
}

void client_impl::prune_expired_idle_connections() {
   const auto now = idle_clock::now();
   const auto max_idle_age = std::chrono::microseconds{options.max_idle_connection_age.count()};
   for (auto entry = idle_connections.begin(); entry != idle_connections.end();) {
      auto& connections = entry->second;
      for (auto connection = connections.begin(); connection != connections.end();) {
         if (!connection->connection->open() || now - connection->idle_since > max_idle_age) {
            connection->connection->close();
            connection = connections.erase(connection);
            FC_ASSERT(idle_connection_count > 0, "Outbound HTTP idle connection count underflow");
            --idle_connection_count;
         } else {
            ++connection;
         }
      }
      if (connections.empty())
         entry = idle_connections.erase(entry);
      else
         ++entry;
   }
}

asio::awaitable<std::pair<std::shared_ptr<connection_state>, bool>>
client_impl::acquire_connection(const target_info& target, const request_options& policy,
                                const std::optional<time_point>& total_deadline,
                                const std::shared_ptr<request_control>& control, bool force_fresh) {
   if (!force_fresh) {
      prune_expired_idle_connections();
      auto found = idle_connections.find(target.connection_key);
      while (found != idle_connections.end() && !found->second.empty()) {
         auto connection = std::move(found->second.back().connection);
         found->second.pop_back();
         FC_ASSERT(idle_connection_count > 0, "Outbound HTTP idle connection count underflow");
         --idle_connection_count;
         if (found->second.empty())
            idle_connections.erase(found);
         if (connection->healthy_for_reuse())
            co_return std::pair{std::move(connection), true};
         connection->close();
         found = idle_connections.find(target.connection_key);
      }
   }
   co_return std::pair{co_await create_connection(target, policy, total_deadline, control), false};
}

void client_impl::release_connection(const std::string& key, std::shared_ptr<connection_state> connection) {
   prune_expired_idle_connections();
   if (!connection->open()) {
      connection->close();
      return;
   }
   if (idle_connection_count >= options.max_idle_connections) {
      connection->close();
      return;
   }
   idle_connections[key].push_back(idle_connection{
      .connection = std::move(connection),
      .idle_since = idle_clock::now(),
   });
   ++idle_connection_count;
}

} // namespace http
} // namespace fc
