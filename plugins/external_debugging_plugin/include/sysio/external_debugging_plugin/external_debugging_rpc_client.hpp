#pragma once

/// Protobuf-typed JSON-RPC 2.0 methods on top of json_rpc_client::call().
/// This header lives in the opp library (which links protobuf::libprotobuf)
/// rather than in libfc (which does not).
///
/// The JSON-RPC "method" field carries the API path (e.g. "/api/opp/envelope"),
/// and "params" carries the protobuf message serialized to JSON.
/// The server returns "result" containing the protobuf response as JSON.

#include <fc/network/json_rpc/json_rpc_client.hpp>

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

#include <string>
#include <type_traits>

namespace sysio::debugging::rpc_client {

// -----------------------------------------------------------------------
//  API route constants — mirrors the TS ApiPaths namespace.
//  No inline string literals.
// -----------------------------------------------------------------------
namespace api_paths {
static constexpr auto ping = "/api/ping";
static constexpr auto opp_base = "/api/opp";
static constexpr auto opp_envelope = "/api/opp/envelope";
} // namespace api_paths

using namespace sysio::opp;
using namespace sysio::opp::debugging;
namespace rpc = fc::network::json_rpc;

/// Send a typed protobuf request via JSON-RPC 2.0 and deserialize the response.
/// Uses protobuf's built-in JSON serialization (MessageToJsonString / JsonStringToMessage).
/// The JSON-RPC method is the API path; params is the protobuf request as a JSON object.
template <typename Res, typename Req>
Res execute(rpc::json_rpc_client& client, const std::string& method, const Req& request) {
   static_assert(std::is_base_of_v<google::protobuf::Message, Req>,
                 "Req must be a protobuf Message");
   static_assert(std::is_base_of_v<google::protobuf::Message, Res>,
                 "Res must be a protobuf Message");

   // Serialize request protobuf to JSON, then parse as fc::variant for json_rpc_client
   std::string req_json;
   auto status = google::protobuf::util::MessageToJsonString(request, &req_json);
   FC_ASSERT(status.ok(), "protobuf MessageToJsonString failed: {}",
             std::string(status.message()));

   fc::variant params = fc::json::from_string(req_json);

   // call() wraps in {"jsonrpc":"2.0","method":...,"params":...,"id":N}
   // and returns the unwrapped "result" field
   fc::variant result = client.call(method, params);

   // Convert result variant back to JSON string, then deserialize into protobuf
   std::string result_json = fc::json::to_string(result, fc::json::yield_function_t{});

   Res response;
   status = google::protobuf::util::JsonStringToMessage(result_json, &response);
   FC_ASSERT(status.ok(), "protobuf JsonStringToMessage failed: {}",
             std::string(status.message()));

   return response;
}

} // namespace sysio::opp::debugging
