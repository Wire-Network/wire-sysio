#pragma once

#include <boost/program_options/variables_map.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <sysio/http_client_plugin/http_client_options.hpp>
#include <utility>

namespace sysio::outpost_rpc {

inline constexpr uint64_t rpc_max_request_bytes = 1ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t rpc_max_response_bytes = 4ULL * 1024ULL * 1024ULL;

/** Return the shared bounded policy for one configured outpost RPC client. */
inline fc::network::json_rpc::client_options rpc_options(const boost::program_options::variables_map& options,
                                                         outbound_http::transport_option_names transport_names) {
   auto result = fc::network::json_rpc::client_options{};
   result.transport =
      outbound_http::read_transport_options(
         options,
         transport_names,
         std::move(result.transport));
   result.request =
      fc::http::request_options{
         .max_request_body_bytes = rpc_max_request_bytes,
         .max_response_body_bytes = rpc_max_response_bytes,
         .timeouts =
            fc::http::timeout_options{
               .connect = fc::seconds(10),
               .header = fc::seconds(30),
               .read = fc::seconds(30),
               .idle = fc::seconds(30),
               .total = fc::seconds(30),
            },
      };
   return result;
}

} // namespace sysio::outpost_rpc
