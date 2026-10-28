#pragma once
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/query_engine_plugin/query_engine.hpp>

namespace sysio::query_engine {
/// JSON-RPC/HTTP adapter. The engine owns execution; this class owns envelopes, IDs, byte caps and notifications.
class query_http_handler : public std::enable_shared_from_this<query_http_handler> {
public:
   /// Charges `size` bytes to http_plugin's in-flight budget until the returned token is destroyed
   /// (http_plugin::reserve_bytes_in_flight).
   using reserve_bytes_in_flight = std::function<std::shared_ptr<void>(size_t size)>;

   /// Retain the shared engine for every pending HTTP request. `reserve` charges each encoded response body
   /// while it waits for an http_plugin thread; empty charges nothing.
   explicit query_http_handler(std::shared_ptr<query_engine>, reserve_bytes_in_flight reserve = {});
   /// Cancel and join HTTP workers before releasing their engine.
   ~query_http_handler();
   /// Validate and enqueue one HTTP body; a dedicated worker calls the engine's blocking execute method. Each
   /// response is encoded once, by this adapter; the callback's emitter copies the bytes into the response.
   void submit(std::string body, url_response_stream_callback);
   /// Bind the production endpoint/category while retaining this adapter through route lifetime.
   api_description_stream create_api();
   /// Cancel queued/running HTTP requests and join the adapter workers.
   void stop();

private:
   struct impl;
   std::unique_ptr<impl> state;
};
} // namespace sysio::query_engine
