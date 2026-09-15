#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <fc/network/es/es_client_options.hpp>
#include <fc/network/http/http_client.hpp>
#include <fc/network/url.hpp>
#include <optional>
#include <string>
#include <thread>

namespace fc::network::es {

/// Outcome of one bulk request after the client's retry loop.
struct es_bulk_result {
   enum class status : uint8_t {
      indexed,     ///< 2xx with "errors":false -- every document acknowledged
      partial,     ///< 2xx with "errors":true -- some items rejected; never retried (the rest DID index)
      rejected,    ///< a terminal 4xx (other than 429), or a 2xx that is not a bulk response
      unavailable, ///< retries exhausted on 5xx / 429 / timeout / connection failure
      canceled,    ///< cancel() was requested before or during delivery
   };
   status outcome = status::canceled;
   uint32_t attempts = 0;     ///< requests actually sent
   uint32_t indexed_docs = 0; ///< documents positively acknowledged by the endpoint
   uint32_t failed_docs = 0;  ///< documents rejected item-by-item (partial) or as a whole (every other failure)
   std::string detail;        ///< endpoint-sanitized diagnostic of the last failure; empty when indexed
};

/// OpenSearch/Elasticsearch client over the asynchronous, executor-bound fc::http::client. It offers two
/// capabilities: bulk delivery -- the delivery half of es_sink, shared with any producer that assembles its own
/// NDJSON -- and a one-shot reachability probe a producer runs at initialization.
/// The client owns one io_context and one thread that runs it (named es-client); every request, retry, and
/// backoff wait is a coroutine on that executor, so the HTTP work never blocks a producer's thread.
/// async_bulk() is the coroutine form for callers on any executor; bulk() is the blocking convenience a
/// producer's delivery worker calls -- it waits for the coroutine's result while the io thread does the I/O.
/// bulk() is used from ONE thread at a time (the producer's delivery worker); cancel() may be called from any
/// thread. Opens no connection until the first request or probe(). HTTPS uses the platform trust store with
/// peer and hostname verification (the client cannot disable it).
class es_client {
public:
   /// Strip a trailing '/' from url and FC_ASSERT the endpoint (http/https scheme, non-empty index),
   /// byte-cap (0 < max_doc_bytes <= max_batch_bytes <= es_max_batch_bytes_ceiling), and auth-pairing invariants.
   static es_client_options validate(es_client_options options);

   /// Validates @p options again (cheap and idempotent, so a caller may pass raw options) and starts the io thread.
   explicit es_client(es_client_options options);
   /// cancel(), then joins the io thread. A producer stops the thread that calls bulk() first.
   ~es_client();

   es_client(const es_client&) = delete;
   es_client& operator=(const es_client&) = delete;

   /// `{"index":{"_index":"<escaped index>"}}\n` -- the line a producer writes before every document.
   const std::string& action_line() const noexcept { return _action_line; }
   /// The endpoint host with credentials stripped, as used in diagnostics.
   const std::string& endpoint() const noexcept { return _endpoint; }
   /// The executor every request runs on -- what a coroutine caller co_spawns async_bulk() onto.
   boost::asio::any_io_executor get_executor() const noexcept { return _http.get_executor(); }

   /// POST @p body -- action/document line pairs, each line newline-terminated, @p doc_count documents in
   /// total -- to <url>/_bulk. Retries a 5xx, a 429, a timeout, or a connection failure up to max_retries
   /// times with capped exponential backoff; never retries after a 2xx. Completes within at most
   /// (max_retries + 1) request timeouts plus the backoffs. Never throws for any delivery outcome -- every one
   /// is a value. The only exceptions are the two programming errors below, neither of them a delivery result.
   /// Must run on get_executor() -- the affinity is FC_ASSERTed on entry.
   /// At most one request may be in flight at a time (async_bulk(), bulk(), or probe()): the cancellation signal
   /// holds a single slot, so an overlapping request would silently displace the first one's cancel handler.
   /// An overlapping request is FC_ASSERTed.
   boost::asio::awaitable<es_bulk_result> async_bulk(std::string body, uint32_t doc_count);

   /// async_bulk() for a worker thread: co_spawns it onto the io thread and blocks until its result arrives.
   /// Never throws for a delivery outcome. It blocks the calling thread, so that thread must NOT be the io
   /// thread -- it would wait on the one thread that has to run the coroutine; the affinity is FC_ASSERTed on
   /// entry, and that assert propagates instead of folding into a result.
   es_bulk_result bulk(std::string body, uint32_t doc_count);

   /// One connectivity check of the configured endpoint, for a producer to run at initialization: a single
   /// GET of the base URL's root, carrying the same auth header and User-Agent as a bulk request and bounded
   /// by the same connect and request timeouts. Never retried -- an endpoint that does not answer while the
   /// producer is starting is a configuration failure, not back-pressure to wait out.
   /// Returns normally on a 2xx, and on a 403: the root is the cluster-info path, which OpenSearch
   /// fine-grained access control gates behind the cluster `monitor/main` permission, so a least-privilege
   /// credential that may only write to _bulk is answered with a 403. The endpoint answered and accepted the
   /// credential -- only reading the cluster info is unauthorized -- so the check treats it as reachable.
   /// A 401 (the credential itself was rejected), a 404, any other status, a connection failure, or a timeout
   /// throws fc::exception naming the sanitized probed URL and the reason; a probe aborted by cancel() throws
   /// with its own message rather than reporting the endpoint unreachable. A failure to run the coroutine at
   /// all surfaces as the std::exception the future rethrows.
   /// Like bulk() it blocks the calling thread on the io thread's coroutine, so it must NOT be called from the
   /// io thread (FC_ASSERTed), and it shares the one-request-at-a-time guard, so it must not run concurrently
   /// with a bulk request. Calling it after cancel() is a programming error (FC_ASSERTed): the client is
   /// permanently canceled and no request would be sent.
   void probe();

   /// Abort the in-flight request and any backoff wait: an async_bulk() in progress completes with `canceled`,
   /// and every later one returns `canceled` immediately. Idempotent; safe from any thread (the signal itself
   /// is emitted on the io thread).
   void cancel() noexcept;

private:
   /// probe()'s coroutine: the single GET, on the io thread. Throws what probe() propagates.
   boost::asio::awaitable<void> async_probe();
   /// Account a 2xx bulk response: positive-signal probe for "errors":false, full parse otherwise.
   es_bulk_result account_response(const std::string& body, uint32_t doc_count, uint32_t attempts) const;
   /// The result every abort path returns: the whole batch counts as failed.
   static es_bulk_result canceled_result(uint32_t doc_count, uint32_t attempts, std::string detail = {});
   /// "Basic <base64(user:pass)>" when the options carry credentials.
   static std::optional<std::string> auth_header_for(const es_client_options& options);

   const es_client_options _options;
   const std::string _action_line; ///< built once from _options.index
   const fc::url _bulk_url;        ///< <url>/_bulk
   const fc::url _root_url;        ///< <url>/ -- what probe() requests
   const std::string _endpoint;    ///< Endpoint host with credentials stripped, for diagnostics.
   std::optional<std::string> _auth_header;
   boost::asio::io_context _io; ///< the delivery event loop, run by _io_thread
   boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work; ///< keeps run() alive
   http::client _http;                                                             ///< bound to _io; io thread only
   boost::asio::cancellation_signal _cancellation; ///< io thread only; cancel() posts its emit there
   std::atomic<bool> _cancel_requested{false};
   /// Set for the duration of one async_bulk() or async_probe(); guards the single cancellation slot.
   std::atomic<bool> _in_flight{false};
   std::thread _io_thread; ///< last member: starts after everything it uses exists
};

} // namespace fc::network::es
