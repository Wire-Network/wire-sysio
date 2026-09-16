#pragma once

#include <array>
#include <atomic>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <cstddef>
#include <cstdint>
#include <fc/io/json_template.hpp>
#include <fc/network/es/es_client.hpp>
#include <fc/parallel/batch_task_queue.hpp>
#include <fc/parallel/worker_task_queue.hpp>
#include <fc/time.hpp>
#include <functional>
#include <magic_enum/magic_enum.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sysio/chain/types.hpp>
#include <sysio/chain_plugin/get_info_db.hpp>
#include <sysio/http_client_plugin/http_client_options.hpp>
#include <vector>

namespace sysio::status_monitor {

/// Name of the plugin's diagnostic fc::logger and the value of the template's `${logger}` token.
inline constexpr std::string_view logger_name = "status_monitor";
/// Value of the template's `${message}` token.
inline constexpr std::string_view record_message = "status_monitor";
/// Key the get_info snapshot is nested under inside the `${data}` object (`data.status_monitor` when the
/// template writes `"data": "${data}"`).
inline constexpr std::string_view record_key = "status_monitor";
/// Value of the template's `${thread}` token: the render worker, named for the plugin.
inline constexpr std::string_view worker_thread_label = "status_monitor";
/// Value of the template's `${level}` token: a status document is informational by definition.
inline constexpr std::string_view record_level = "info";
/// Every token name render_document supplies, and therefore every name a template may reference: the logging
/// layout's value tokens plus `data`. The layout's extra-fields tokens are not among them -- a status template
/// writes its fixed members literally -- so parse_config rejects a template that references anything else.
inline constexpr auto supplied_tokens = std::to_array<std::string_view>({
   magic_enum::enum_name(fc::json_template_default_token::timestamp),
   magic_enum::enum_name(fc::json_template_default_token::iso8601),
   magic_enum::enum_name(fc::json_template_default_token::epoch_millis),
   magic_enum::enum_name(fc::json_template_default_token::level),
   magic_enum::enum_name(fc::json_template_default_token::message),
   magic_enum::enum_name(fc::json_template_default_token::logger),
   magic_enum::enum_name(fc::json_template_default_token::file),
   magic_enum::enum_name(fc::json_template_default_token::line),
   magic_enum::enum_name(fc::json_template_default_token::func),
   magic_enum::enum_name(fc::json_template_default_token::thread),
   magic_enum::enum_name(fc::json_template_default_token::data),
});
/// Separator between token names wherever a token list is rendered (help text, diagnostics).
inline constexpr std::string_view token_list_separator = ", ";

/// Program-option names; all are `cfg` options (command line and config.ini).
namespace option {
/// Base URL of the `_bulk` endpoint; absent leaves the plugin disabled and nothing else validated.
inline constexpr auto target_url = "status-monitor-target-url";
/// Index or write alias the documents are written to; required and non-empty once the target URL is set.
inline constexpr auto target_index = "status-monitor-target-index";
/// Path of the JSON document template; required once the target URL is set, and compiled at startup.
inline constexpr auto target_template_file = "status-monitor-target-template-file";
/// HTTP basic auth user; provided together with the password or not at all.
inline constexpr auto username = "status-monitor-username";
/// HTTP basic auth password; provided together with the user or not at all.
inline constexpr auto password = "status-monitor-password";
/// Documents per _bulk request at most; 1 to max_items_per_task_ceiling.
inline constexpr auto max_items_per_task = "status-monitor-max-items-per-task";
/// Rendered documents waiting behind the active _bulk request at most; 1 to max_pending_documents_ceiling.
inline constexpr auto max_pending_documents = "status-monitor-max-pending-documents";
/// Connect timeout of each _bulk request, in milliseconds; greater than 0.
inline constexpr auto connect_timeout_ms = "status-monitor-connect-timeout-ms";
/// Header, read, idle, and total timeout of each _bulk request, in milliseconds; greater than 0.
inline constexpr auto request_timeout_ms = "status-monitor-request-timeout-ms";
/// Delivery attempts after the first for a retryable failure; 0 to max_retries_ceiling.
inline constexpr auto max_retries = "status-monitor-max-retries";
/// Initial retry backoff, in milliseconds; greater than 0, doubled per attempt up to
/// fc::network::es::es_max_retry_backoff_ms.
inline constexpr auto retry_backoff_ms = "status-monitor-retry-backoff-ms";
/// PEM CA bundle added to system trust for the endpoint's requests; overrides outbound-http-additional-ca-file.
inline constexpr auto additional_ca_file = "status-monitor-additional-ca-file";
/// Hashed CA directory added to system trust for the endpoint's requests; overrides outbound-http-additional-ca-path.
inline constexpr auto additional_ca_path = "status-monitor-additional-ca-path";
/// Explicit proxy URL for the endpoint's requests; overrides outbound-http-proxy.
inline constexpr auto proxy = "status-monitor-proxy";
/// The three transport options above, in the shape sysio::outbound_http registers and reads.
inline constexpr sysio::outbound_http::transport_option_names transport_option_names{
   .additional_ca_file = additional_ca_file,
   .additional_ca_path = additional_ca_path,
   .proxy = proxy,
};
} // namespace option

/// Documents per _bulk request at most; the delivery worker sends whatever is queued, up to this many.
inline constexpr uint32_t default_max_items_per_task = 100;
/// Rendered documents waiting behind the active _bulk request at most; the newest is dropped when full.
inline constexpr uint32_t default_max_pending_documents = 256;
/// Upper bounds on the two queue options: a bulk request never usefully carries more documents than fit
/// its body cap (at the ceiling one drain is about 10 MiB of documents, which assemble_bulk_bodies splits
/// into several bodies -- bounded, just pointless), and the delivery queue stays bounded so a stalled
/// endpoint costs a bounded amount of memory.
inline constexpr uint32_t max_items_per_task_ceiling = 10'000;
/// Upper bound on --status-monitor-max-pending-documents, the delivery queue's depth (see above).
inline constexpr uint32_t max_pending_documents_ceiling = 65'536;
/// Upper bound on --status-monitor-max-retries: with fc::network::es::es_max_retry_backoff_ms as the backoff
/// cap, ten retries already hold the delivery worker for about 20 s of backoff plus eleven request timeouts on
/// a dead endpoint.
inline constexpr uint32_t max_retries_ceiling = 10;
/// Snapshots waiting for the render worker at most. Rendering takes microseconds, so this never fills in
/// steady state; the bound keeps memory finite if the worker ever stalls.
inline constexpr std::size_t max_pending_snapshots = 32;
/// Body and single-document caps of the bulk requests: the fc::network::es defaults, not options, because a
/// status document is about 1 KiB and default_max_items_per_task of them fill a small fraction of the body cap.
inline constexpr uint32_t max_batch_bytes = fc::network::es::es_default_max_batch_bytes;
/// Single-document cap: a rendered document larger than this is dropped and counted, never sent.
inline constexpr uint32_t max_doc_bytes = fc::network::es::es_default_max_doc_bytes;

/// Everything the plugin needs, parsed once from its options.
struct config {
   /// Endpoint and request-level settings of the _bulk target, as es_client::validate() normalized them.
   fc::network::es::es_client_options delivery;
   /// Proxy and extra CA trust of the endpoint's requests: the status-monitor-* values over the outbound-http-* ones.
   fc::http::transport_options transport;
   /// The operator's compiled document template; rendered once per snapshot.
   fc::json_template document_template;
   /// Documents per _bulk request at most (--status-monitor-max-items-per-task).
   uint32_t max_items_per_task = default_max_items_per_task;
   /// Rendered documents waiting behind the active _bulk request at most (--status-monitor-max-pending-documents).
   uint32_t max_pending_documents = default_max_pending_documents;
};

/// Register the plugin's options on @p cfg (command line and config.ini).
void add_options(boost::program_options::options_description& cfg);

/// Parse the plugin's options. nullopt when `--status-monitor-target-url` is absent: the plugin stays disabled
/// and nothing else is validated, so a config.ini carrying the tuning knobs but no target never fails startup.
/// Throws chain::plugin_config_exception on an invalid value or combination, a missing index or template file,
/// a template that does not compile, or a template that references a name outside supplied_tokens.
std::optional<config> parse_config(const boost::program_options::variables_map& options);

/// One queued unit from the application thread: the get_info snapshot and when it was taken.
struct status_snapshot {
   chain_apis::get_info_db::get_info_results info; ///< the get_info snapshot /v1/chain/get_info would return
   fc::time_point observed;                        ///< wall-clock time of the snapshot -> the timestamp tokens
};

/// Render one document: @p document_template over the supplied_tokens -- `data` set to
/// `{ "status_monitor": <snapshot> }` (the snapshot exactly as /v1/chain/get_info would return it), the three
/// timestamp entries from @p snapshot.observed, `level`/`message`/`logger`/`thread` from the constants above, and
/// `file`/`line`/`func` from this renderer's location. Returns one newline-terminated line.
std::string render_document(const status_snapshot& snapshot, const fc::json_template& document_template);

/// One assembled _bulk request body (action/document line pairs) plus its document count.
struct bulk_body {
   std::string body;       ///< the NDJSON request body: one action line and one document line per document
   uint32_t doc_count = 0; ///< documents paired into body
};

/// Pair every document with @p action_line and pack the pairs, in order, into bodies of at most
/// @p body_byte_cap each (plus at most one pair, as es_sink allows). Documents are consumed: each is emptied
/// once appended, so the caller's buffer and the bodies never hold every document twice.
std::vector<bulk_body> assemble_bulk_bodies(std::span<std::string> documents, std::string_view action_line,
                                            std::size_t body_byte_cap);

/// Counters the workers maintain; read from the application thread.
struct pipeline_stats {
   uint64_t snapshots_submitted = 0;          ///< admitted to the render queue
   uint64_t snapshots_dropped_queue_full = 0; ///< rejected by a full render queue
   uint64_t documents_queued = 0;             ///< rendered and admitted to the delivery queue
   uint64_t documents_dropped_oversize = 0;   ///< larger than max_doc_bytes
   uint64_t documents_dropped_queue_full = 0; ///< rejected by a full delivery queue
   uint64_t render_failures = 0;              ///< render_document threw (never expected)
   uint64_t batches_indexed = 0;              ///< bulk requests fully acknowledged
   uint64_t batches_failed = 0;               ///< bulk requests partial, rejected, unavailable, or canceled
   uint64_t documents_indexed = 0;            ///< documents acknowledged by the endpoint
   uint64_t documents_failed = 0;             ///< documents rejected or never delivered
};

/// The two-stage worker pipeline: render (worker_task_queue<status_snapshot>, one thread) then delivery
/// (batch_task_queue<std::string>, one thread, up to max_items_per_task documents per bulk request). The
/// sender is injected: the plugin passes fc::network::es::es_client::bulk, tests pass a recorder. Workers
/// touch only their items, the counters, the last-failure detail, the sender, and the failure reporter; the
/// reporter is the one place a worker reaches back to the caller for anything but delivery.
class pipeline {
public:
   /// Delivers one assembled body; must not throw (es_client::bulk never does).
   using sender = std::function<fc::network::es::es_bulk_result(std::string body, uint32_t doc_count)>;
   /// Reports one bulk request whose outcome was not `indexed`, with the batch's result and the documents it
   /// carried. Called on the delivery worker, once per failed batch, before the next one is sent; must not
   /// throw (the plugin's reporter only logs, at a limited rate).
   using failure_reporter = std::function<void(const fc::network::es::es_bulk_result& result, uint32_t doc_count)>;

   /// Starts both worker threads. @p action_line is the es client's; @p send delivers one assembled body;
   /// @p report_failure, when set, is invoked once per failed batch.
   pipeline(const config& cfg, std::string action_line, sender send, failure_reporter report_failure = {});
   /// shutdown().
   ~pipeline();

   /// Non-copyable: a pipeline owns two running worker queues and the counters those workers write.
   pipeline(const pipeline&) = delete;
   pipeline& operator=(const pipeline&) = delete;

   /// Queue one snapshot for rendering. False (and counted) when the render queue is full.
   bool submit(status_snapshot snapshot);
   /// Discard every pending snapshot and document, then stop the render worker and the delivery worker, in
   /// that order (nothing can enter the delivery queue after the render worker is joined). A bulk request in
   /// flight completes, or returns `canceled` when the sender's client was canceled first. Idempotent.
   void shutdown();
   /// The counters (each read atomically; the set is not one transaction).
   pipeline_stats stats() const;
   /// Detail of the most recent failed bulk request; empty when none failed yet.
   std::string last_failure() const;

private:
   /// Render-worker callback: one snapshot -> one document line -> the delivery queue.
   void render_stage(status_snapshot& snapshot);
   /// Delivery-worker callback: a drained span of documents -> one or more bulk requests.
   void delivery_stage(std::span<std::string> documents);
   /// Keep @p detail as the last failure (delivery worker; read by last_failure()).
   void note_failure(std::string detail);

   /// The atomic mirror of pipeline_stats the workers write; see pipeline_stats for each counter's meaning.
   struct counters {
      std::atomic<uint64_t> snapshots_submitted{0};
      std::atomic<uint64_t> snapshots_dropped_queue_full{0};
      std::atomic<uint64_t> documents_queued{0};
      std::atomic<uint64_t> documents_dropped_oversize{0};
      std::atomic<uint64_t> documents_dropped_queue_full{0};
      std::atomic<uint64_t> render_failures{0};
      std::atomic<uint64_t> batches_indexed{0};
      std::atomic<uint64_t> batches_failed{0};
      std::atomic<uint64_t> documents_indexed{0};
      std::atomic<uint64_t> documents_failed{0};
   };

   fc::json_template _document_template;
   std::string _action_line;
   uint32_t _body_byte_cap; ///< cfg.delivery.max_batch_bytes
   uint32_t _doc_byte_cap;  ///< cfg.delivery.max_doc_bytes
   sender _send;
   failure_reporter _report_failure; ///< empty when the caller wants no per-batch report
   counters _counters;
   mutable std::mutex _failure_mtx;
   std::string _last_failure; ///< guarded by _failure_mtx
   std::shared_ptr<fc::parallel::batch_task_queue<std::string>> _delivery;
   std::shared_ptr<fc::parallel::worker_task_queue<status_snapshot>> _render;
};

/// What the application thread should log after comparing two counter snapshots.
enum class progress : uint8_t {
   steady,    ///< nothing new to report
   failing,   ///< a failure or drop counter advanced
   recovered, ///< no new failure, and a bulk request was fully acknowledged after a failing streak
};

/// The reporting policy: any advanced failure or drop counter is `failing`; an acknowledged batch with no new
/// failure after a failing streak is `recovered`; everything else is `steady`.
progress classify_progress(const pipeline_stats& previous, const pipeline_stats& current, bool was_failing);

} // namespace sysio::status_monitor
