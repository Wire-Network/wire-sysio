#pragma once
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/query_engine_plugin/query_engine.hpp>

namespace sysio::query_engine_plugin {
/// JSON-RPC/HTTP adapter. The engine owns execution; this class owns envelopes, IDs, byte caps and notifications.
class query_http_handler : public std::enable_shared_from_this<query_http_handler> {
public:
   /// Retain the shared engine for every pending HTTP request.
   explicit query_http_handler(std::shared_ptr<query_engine>);
   /// Cancel and join HTTP workers before releasing their engine.
   ~query_http_handler();
   /// Validate and enqueue one HTTP body; a dedicated worker calls the engine's blocking execute method.
   void submit(std::string body, url_response_callback);
   /// Bind the production endpoint/category while retaining this adapter through route lifetime.
   api_description create_api();
   /// Cancel queued/running HTTP requests and join the adapter workers.
   void stop();

private:
   struct impl;
   std::unique_ptr<impl> state;
};
} // namespace sysio::query_engine_plugin
