#pragma once

#include <fc/io/json_template.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/es/es_client.hpp>
#include <fc/parallel/batch_task_queue.hpp>
#include <fc/parallel/worker_task_queue.hpp>
#include <fc/time.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain_plugin/get_info_db.hpp>
#include <sysio/http_client_plugin/http_client_options.hpp>
#include <sysio/query_engine_plugin/query_service.hpp>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include <magic_enum/magic_enum.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sysio::alert_monitor {

/// Name of the plugin's diagnostic fc::logger and the value of the template's `${logger}` token.
inline constexpr std::string_view logger_name = "alert_monitor";
/// Value of the template's `${message}` token.
inline constexpr std::string_view record_message = "alert_monitor";
/// Key a status document's object (see status_document) is nested under inside the `${data}` object
/// (`data.alert_monitor` when the template writes `"data": "${data}"`); no query may take it.
inline constexpr std::string_view record_key = "alert_monitor";
/// Value of the template's `${thread}` token: the render worker, named for the plugin.
inline constexpr std::string_view worker_thread_label = "alert_monitor";
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
/// Base URL of the `_bulk` endpoint; absent leaves the plugin disabled and nothing else validated, except that
/// `alert-monitor-query` requires it.
inline constexpr auto target_url = "alert-monitor-target-url";
/// Index or write alias the documents are written to; required and non-empty once the target URL is set.
inline constexpr auto target_index = "alert-monitor-target-index";
/// Path of the JSON document template; required once the target URL is set, and compiled at startup.
inline constexpr auto target_template_file = "alert-monitor-target-template-file";
/// HTTP basic auth user; provided together with the password or not at all.
inline constexpr auto username = "alert-monitor-username";
/// HTTP basic auth password; provided together with the user or not at all.
inline constexpr auto password = "alert-monitor-password";
/// Documents per _bulk request at most; 1 to max_items_per_task_ceiling.
inline constexpr auto max_items_per_task = "alert-monitor-max-items-per-task";
/// Rendered documents waiting behind the active _bulk request at most; 1 to max_pending_documents_ceiling.
inline constexpr auto max_pending_documents = "alert-monitor-max-pending-documents";
/// Connect timeout of each _bulk request, in milliseconds; greater than 0.
inline constexpr auto connect_timeout_ms = "alert-monitor-connect-timeout-ms";
/// Header, read, idle, and total timeout of each _bulk request, in milliseconds; greater than 0.
inline constexpr auto request_timeout_ms = "alert-monitor-request-timeout-ms";
/// Delivery attempts after the first for a retryable failure; 0 to max_retries_ceiling.
inline constexpr auto max_retries = "alert-monitor-max-retries";
/// Initial retry backoff, in milliseconds; greater than 0, doubled per attempt up to
/// fc::network::es::es_max_retry_backoff.
inline constexpr auto retry_backoff_ms = "alert-monitor-retry-backoff-ms";
/// PEM CA bundle added to system trust for the endpoint's requests; overrides outbound-http-additional-ca-file.
inline constexpr auto additional_ca_file = "alert-monitor-additional-ca-file";
/// Hashed CA directory added to system trust for the endpoint's requests; overrides outbound-http-additional-ca-path.
inline constexpr auto additional_ca_path = "alert-monitor-additional-ca-path";
/// Explicit proxy URL for the endpoint's requests; overrides outbound-http-proxy.
inline constexpr auto proxy = "alert-monitor-proxy";
/// The three transport options above, in the shape sysio::outbound_http registers and reads.
inline constexpr sysio::outbound_http::transport_option_names transport_option_names{
   .additional_ca_file = additional_ca_file,
   .additional_ca_path = additional_ca_path,
   .proxy = proxy,
};
/// One query per value, repeatable (repeat the key in config.ini; repeat the flag on the CLI), as a spec in the
/// style of --signature-provider: `<snake_case_name>,<block_interval>,<query...>`. Everything after the second comma
/// is the WireQL text, commas included.
inline constexpr auto query = "alert-monitor-query";
} // namespace option

/// Fields of an --alert-monitor-query spec; fc::split folds the third and later commas into the query field.
inline constexpr std::size_t query_spec_field_count = 3;
/// Spec format named in every parse error (never the spec's own text).
inline constexpr std::string_view query_spec_format = "<snake_case_name>,<block_interval>,<query...>";
/// A query name: snake_case, so it is a valid `${data}` key and a stable OpenSearch field path (data.<name>).
/// Matched over the whole string with std::regex_match, so a trailing newline is rejected rather than ignored.
inline constexpr std::string_view query_name_pattern = "[a-z][a-z0-9_]*";
/// What a blank query consists of; compared bytewise (no locale, never std::isspace on UTF-8 bytes).
inline constexpr std::string_view query_blank_characters = " \t\n\v\f\r";
/// The `${data}` key nodeop's own es_sink writes a log record's thread name under: `data.thread`, a string, in the
/// index this plugin may share with the log records.
inline constexpr std::string_view es_sink_thread_key = "thread";
/// Names no query may take: the status key, and es_sink_thread_key, where a query's object would be a mapping conflict
/// with es_sink's string and every document rejected.
inline constexpr auto reserved_query_names = std::to_array<std::string_view>({record_key, es_sink_thread_key});

/// Documents per _bulk request at most; the delivery worker sends whatever is queued, up to this many.
inline constexpr uint32_t default_max_items_per_task = 100;
/// Rendered documents waiting behind the active _bulk request at most; the newest is dropped when full.
inline constexpr uint32_t default_max_pending_documents = 256;
/// Upper bounds on the two queue options: a bulk request never usefully carries more documents than fit
/// its body cap (at the ceiling one drain is about 10 MiB of documents, which assemble_bulk_bodies splits
/// into several bodies -- bounded, just pointless), and the delivery queue stays bounded so a stalled
/// endpoint costs a bounded amount of memory.
inline constexpr uint32_t max_items_per_task_ceiling = 10'000;
/// Upper bound on --alert-monitor-max-pending-documents, the delivery queue's depth (see above).
inline constexpr uint32_t max_pending_documents_ceiling = 65'536;
/// Upper bound on --alert-monitor-max-retries: with fc::network::es::es_max_retry_backoff as the backoff
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

/// One validated --alert-monitor-query value.
struct query_entry {
   uint32_t ordinal = 0;        ///< position among the option's values
   std::string name;            ///< snake_case; the `${data}` key of its documents, and what diagnostics name
   std::string query;           ///< WireQL text; never logged
   uint32_t block_interval = 1; ///< runs once per LIB bucket of this many blocks (1..UINT32_MAX)
};

/// Everything the plugin needs, parsed once from its options.
struct config {
   /// Endpoint and request-level settings of the _bulk target, as es_client::validate() normalized them.
   fc::network::es::es_client_options delivery;
   /// Proxy and extra CA trust of the endpoint's requests: the alert-monitor-* values over the outbound-http-* ones.
   fc::http::transport_options transport;
   /// The operator's compiled document template; rendered once per status snapshot and once per query result.
   fc::json_template document_template;
   /// Documents per _bulk request at most (--alert-monitor-max-items-per-task).
   uint32_t max_items_per_task = default_max_items_per_task;
   /// Rendered documents waiting behind the active _bulk request at most (--alert-monitor-max-pending-documents).
   uint32_t max_pending_documents = default_max_pending_documents;
   /// The --alert-monitor-query entries, each ordinal its index here; empty when no query is configured.
   std::vector<query_entry> queries;
};

/// Register the plugin's options on @p cfg (command line and config.ini).
void add_options(boost::program_options::options_description& cfg);

/// Parse the plugin's options. nullopt when `--alert-monitor-target-url` is absent: the plugin stays disabled
/// and nothing else is validated, except that `alert-monitor-query` requires it, so a config.ini carrying the tuning
/// knobs but no target never fails startup. A relative `--alert-monitor-target-template-file` is resolved against
/// @p config_dir -- the plugin passes app().config_dir(), as chain_plugin does for protocol-features-dir, so a node
/// directory stays relocatable. It has no default: an omitted directory would silently resolve a relative path
/// against the working directory. Throws chain::plugin_config_exception on an invalid value or combination, a
/// missing index or template file, a template that does not compile, a template that references a name outside
/// supplied_tokens, an invalid query spec (see parse_query_entry), or a query name used twice (the error names it).
std::optional<config> parse_config(const boost::program_options::variables_map& options,
                                   const std::filesystem::path& config_dir);

/// Parse one spec: `fc::split(spec, ',', query_spec_field_count)` (as signature_provider_manager_plugin does).
/// Exactly three fields; the name matches query_name_pattern and is not reserved; the interval parses with
/// `std::from_chars` into uint32_t, consumes the whole field and is nonzero (as chain_plugin's snapshot endpoint does;
/// this rejects empty, signed, padded, fractional and overflowing values); the query is not blank --
/// `query.find_first_not_of(query_blank_characters) == npos`. Throws
/// plugin_config_exception naming the option, the ordinal and query_spec_format, plus: for a field-count error, the
/// expected (3) and actual counts (as signature_provider_manager_plugin's spec parser does); otherwise the failing
/// field's index and label -- 0 `name`, 1 `block_interval`, 2 `query`. No message reproduces any field -- a missing
/// field shifts query text into the name and interval positions -- except a name that already matched
/// query_name_pattern: the interval and query errors, and parse_config's duplicate-name error, include it so the
/// operator can find the entry (a snake_case token cannot carry query text). The ordinal alone is not a config.ini
/// line position (`composing()` puts CLI values first).
query_entry parse_query_entry(std::string_view spec, uint32_t ordinal);

/// True when an entry with @p interval is due at @p lib: its bucket (lib / interval) is nonzero and newer than
/// @p admitted_bucket. No backfill: after a pause or a jump, only the current bucket runs. Never due for a zero
/// interval, which parse_query_entry rejects.
bool is_due(chain::block_num_type lib, uint32_t interval, chain::block_num_type admitted_bucket);

/// One queued unit from the application thread: the get_info snapshot and when it was taken.
struct status_snapshot {
   chain_apis::get_info_db::get_info_results info; ///< the get_info snapshot /v1/chain/get_info would return
   fc::time_point observed;                        ///< wall-clock time of the snapshot -> the timestamp tokens
};

/// Member keys every object a document's `${data}` holds starts with, status and query documents alike: which kind
/// of document it is, and the layout version of that kind. Declared once; status_document::key and
/// query_document::key name these same constants.
namespace document_key {
inline constexpr auto record = "record";   ///< status_document::record or query_document::record
inline constexpr auto version = "version"; ///< status_document::version or query_document::version
} // namespace document_key

/// The object a status document's `${data}` holds under record_key: its fixed values and member keys.
namespace status_document {
/// Value of the `record` member.
inline constexpr auto record = "status";
/// Value of the `version` member: the layout of this object, raised when a member changes shape or meaning.
inline constexpr uint32_t version = 1;
/// Member keys of the object.
namespace key {
using document_key::record;              ///< status_document::record
using document_key::version;             ///< status_document::version
inline constexpr auto status = "status"; ///< the get_info snapshot, as /v1/chain/get_info returns it
} // namespace key
} // namespace status_document

/// Render one document: @p document_template over the supplied_tokens -- `data` set to
/// `{ "alert_monitor": { "record": "status", "version": 1, "status": <snapshot> } }` (see status_document; the
/// snapshot carries the get_info fields, with integers as JSON numbers), the three timestamp entries from
/// @p snapshot.observed, `level`/`message`/`logger`/`thread` from the constants above, and `file`/`line`/`func` from
/// this renderer's location. Returns one newline-terminated line.
std::string render_document(const status_snapshot& snapshot, const fc::json_template& document_template);

/// The LIB that made a query due, copied on the application thread from the cached get_info.
struct query_trigger {
   chain::block_num_type lib_num = 0;      ///< get_info's last_irreversible_block_num
   chain::block_id_type lib_id;            ///< get_info's last_irreversible_block_id
   std::optional<fc::time_point> lib_time; ///< get_info's last_irreversible_block_time; absent when get_info has none
};

/// The object a query document's `${data}` holds under the query's name: its fixed values and member keys.
namespace query_document {
/// Value of the `record` member; a status document's is status_document::record.
inline constexpr auto record = "query";
/// Value of the `version` member: the layout of this object, raised when a member changes shape or meaning.
inline constexpr uint32_t version = 1;
/// Member keys of the object, and of its `query` and `trigger` members.
namespace key {
using document_key::record;                              ///< query_document::record
using document_key::version;                             ///< query_document::version
inline constexpr auto query = "query";                   ///< the entry: name, ordinal, block_interval
inline constexpr auto name = "name";                     ///< query_entry::name
inline constexpr auto ordinal = "ordinal";               ///< query_entry::ordinal
inline constexpr auto block_interval = "block_interval"; ///< query_entry::block_interval
inline constexpr auto trigger = "trigger";               ///< the LIB that made it due: lib_num, lib_id, lib_time
inline constexpr auto lib_num = "lib_num";               ///< query_trigger::lib_num
inline constexpr auto lib_id = "lib_id";                 ///< query_trigger::lib_id
inline constexpr auto lib_time = "lib_time";             ///< query_trigger::lib_time (null when absent)
inline constexpr auto observed_at = "observed_at";       ///< wall-clock time the result was rendered
inline constexpr auto result = "result";                 ///< the reflected query_engine::query_result, unchanged
} // namespace key
} // namespace query_document

/// Render one query document with the same template and tokens as status documents (level/message/logger use the
/// existing constants): ${data} = { "<entry.name>": { record, version, query{name, ordinal, block_interval},
/// trigger{lib_num, lib_id, lib_time}, observed_at, result } }. Never includes entry.query. The timestamp tokens and
/// `observed_at` come from @p observed. Returns one newline-terminated line.
std::string render_query_document(const query_entry& entry, const query_trigger& trigger, fc::time_point observed,
                                  const query_engine::query_result& result, const fc::json_template& document_template);

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

/// Counters the workers maintain; read from the application thread. The documents_* admission counters are the
/// status documents' and the query_documents_* ones the query documents'; the batch and delivered-document
/// counters cover both, since both share one delivery queue.
struct pipeline_stats {
   uint64_t snapshots_submitted = 0;                ///< admitted to the render queue
   uint64_t snapshots_dropped_queue_full = 0;       ///< rejected by a full render queue
   uint64_t documents_queued = 0;                   ///< status documents rendered and admitted to the delivery queue
   uint64_t documents_dropped_oversize = 0;         ///< status documents larger than max_doc_bytes
   uint64_t documents_dropped_queue_full = 0;       ///< status documents rejected by a full delivery queue
   uint64_t query_documents_queued = 0;             ///< query documents admitted to the delivery queue
   uint64_t query_documents_dropped_oversize = 0;   ///< query documents larger than max_doc_bytes
   uint64_t query_documents_dropped_queue_full = 0; ///< query documents rejected by a full delivery queue
   uint64_t render_failures = 0;                    ///< render_document threw (never expected)
   uint64_t batches_indexed = 0;                    ///< bulk requests fully acknowledged
   uint64_t batches_failed = 0;                     ///< bulk requests partial, rejected, unavailable, or canceled
   uint64_t documents_indexed = 0;                  ///< documents acknowledged by the endpoint
   uint64_t documents_failed = 0;                   ///< documents rejected or never delivered
};

/// What pipeline::submit_document did with a document.
enum class admission : uint8_t {
   queued,             ///< admitted to the delivery queue
   dropped_oversize,   ///< larger than the single-document cap; never sent
   dropped_queue_full, ///< refused by a full delivery queue, or by one shutdown() has stopped; never sent
};

/// The two-stage worker pipeline: render (worker_task_queue<status_snapshot>, one thread) then delivery
/// (batch_task_queue<std::string>, one thread, up to max_items_per_task documents per bulk request). Query
/// documents, rendered by their callers, enter the delivery stage directly through submit_document(). The
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
   /// Queue one already-rendered query document (see render_query_document) for delivery: the size cap and the
   /// delivery queue a rendered status document goes through, counted on the query_documents_* counters. Callable
   /// from any thread; never waits for room. dropped_queue_full once shutdown() has stopped the delivery worker.
   admission submit_document(std::string document);
   /// Discard every pending snapshot and stop the render worker, then stop the delivery worker and discard every
   /// document it left queued, in that order. Nothing enters the delivery queue once the render worker is joined,
   /// and the delivery worker is stopped before its queue is discarded: a submit_document() racing this call is
   /// either admitted before the stop -- then sent or discarded like any queued document -- or refused after it,
   /// never admitted after the discard and left unsent. The stop takes no further batch, so the queued backlog is
   /// never delivered; a bulk request in flight completes, or returns `canceled` when the sender's client was
   /// canceled first. Idempotent.
   void shutdown();
   /// A copy of the counters, taken under the stats lock; each batch's counters and failure detail change together.
   pipeline_stats stats() const;
   /// Detail of the most recent failed bulk request; empty when none failed yet.
   std::string last_failure() const;

private:
   /// The three counters one document source advances in admit().
   struct admission_counters {
      uint64_t pipeline_stats::*queued;             ///< admitted to the delivery queue
      uint64_t pipeline_stats::*dropped_oversize;   ///< larger than the single-document cap
      uint64_t pipeline_stats::*dropped_queue_full; ///< rejected by a full delivery queue
   };
   /// Counters of the status documents render_stage() admits.
   static constexpr admission_counters status_admission{&pipeline_stats::documents_queued,
                                                        &pipeline_stats::documents_dropped_oversize,
                                                        &pipeline_stats::documents_dropped_queue_full};
   /// Counters of the query documents submit_document() admits.
   static constexpr admission_counters query_admission{&pipeline_stats::query_documents_queued,
                                                       &pipeline_stats::query_documents_dropped_oversize,
                                                       &pipeline_stats::query_documents_dropped_queue_full};

   /// Render-worker callback: one snapshot -> one document line -> admit().
   void render_stage(status_snapshot& snapshot);
   /// The tail both document sources share: a document above the single-document cap is dropped, any other is
   /// offered to the delivery queue; the outcome is counted on @p counters and returned. Throws only what the
   /// queue's allocation throws.
   admission admit(std::string document, const admission_counters& counters);
   /// Delivery-worker callback: a drained span of documents -> one or more bulk requests.
   void delivery_stage(std::span<std::string> documents);
   /// Add @p n to one counter under the stats lock.
   void count(uint64_t pipeline_stats::*counter, uint64_t n = 1);

   fc::json_template _document_template;
   std::string _action_line;
   uint32_t _body_byte_cap; ///< cfg.delivery.max_batch_bytes
   uint32_t _doc_byte_cap;  ///< cfg.delivery.max_doc_bytes
   sender _send;
   failure_reporter _report_failure; ///< empty when the caller wants no per-batch report
   mutable std::mutex _stats_mtx;    ///< guards _stats and _last_failure
   pipeline_stats _stats;
   std::string _last_failure;
   std::shared_ptr<fc::parallel::batch_task_queue<std::string>> _delivery;
   std::shared_ptr<fc::parallel::worker_task_queue<status_snapshot>> _render;
};

/// Deadline of every query execution: the engine's own default (1 s). It also bounds query_runner::shutdown(),
/// keeping node shutdown well inside a supervisor's kill grace.
inline constexpr std::chrono::milliseconds query_deadline = query_engine::defaults::timeout;
/// Interval of the throttled failure lines: the delivery failure/recovery line, the per-batch line, and the
/// per-query lines.
inline constexpr fc::microseconds failure_report_interval = fc::seconds(60);

/// First event, then at most one line per failure_report_interval carrying the events in between.
/// Not synchronized: each throttle is used by one thread at a time.
struct failure_throttle {
   fc::time_point last_report; ///< when a line was last logged
   uint64_t unlogged = 0;      ///< events since last_report that were not logged
   /// The unlogged count to report when a line is due at @p now; nullopt (and the event counted) otherwise.
   std::optional<uint64_t> admit(fc::time_point now);
};

/// What a throttled line ends with when failure_throttle::admit reports events it held back: their count, then what
/// they were (for example "failed batch(es)").
inline constexpr std::string_view unlogged_events_format = " ({} more {} since the last line)";

/// unlogged_events_format over @p unlogged and @p events, for the end of a throttled line; empty when @p unlogged is
/// 0, so a line with nothing held back ends where its own format does.
std::string unlogged_events_clause(uint64_t unlogged, std::string_view events);

/// Counters of the query side; copied under query_runner's mutex.
struct query_stats {
   uint64_t succeeded = 0;    ///< executed, rendered, and admitted to the delivery queue
   uint64_t skipped_busy = 0; ///< due while the previous run of the same entry was still active
   std::array<uint64_t, magic_enum::enum_count<query_engine::error_kind>()> failed{}; ///< by kind (enum_index)
   uint64_t failed_other = 0; ///< not a query_error, or no service (before/after query_engine_plugin runs)
};

/// Runs the configured queries off the application thread: dispatch() admits due entries into a caller pool (one
/// thread per entry); a caller executes through the injected query service and hands the rendered document to the
/// pipeline. At most one execution per entry at a time.
class query_runner {
public:
   /// Production: query_engine_plugin::get_query_service (null before it starts / after it stops). Tests: a fake.
   using service_provider = std::function<std::shared_ptr<query_engine::query_service>()>;
   /// Creates the caller pool (max_threads = max_pending_items = entries) when cfg.queries is non-empty. @p cfg and
   /// @p sink must outlive the runner; @p log is copied (see set_logger).
   query_runner(const config& cfg, pipeline& sink, service_provider provider, const fc::logger& log);
   /// shutdown().
   ~query_runner();

   /// Neither copyable nor movable: the caller pool's callback captures `this`, and the runner owns those running
   /// threads.
   query_runner(const query_runner&) = delete;
   query_runner& operator=(const query_runner&) = delete;
   query_runner(query_runner&&) = delete;
   query_runner& operator=(query_runner&&) = delete;

   /// Application thread, once per newly observed LIB: admit every due, idle entry. Never blocks.
   void dispatch(const query_trigger& trigger);
   /// Discard queued jobs, then join the callers; bounded by the per-call deadline (query_deadline). Idempotent.
   void shutdown();
   /// A copy of the counters.
   query_stats stats() const;
   /// Rebind the failure-line logger (the plugin's handle_sighup calls it). Copied under _stats_mtx; report_failure
   /// copies it under the same mutex before logging.
   void set_logger(const fc::logger& log);

private:
   /// One admitted execution: the entry (by ordinal) and the LIB that made it due.
   struct job {
      uint32_t ordinal;      ///< index into config::queries
      query_trigger trigger; ///< copied into the document's `trigger` member
   };
   /// Caller callback: execute one job and submit its document. noexcept and fully guarded, as the caller pool
   /// requires; clears the entry's running flag last.
   void run(job& j) noexcept;
   /// Add one to @p counter under _stats_mtx.
   void count(uint64_t query_stats::*counter);
   /// Count one failure by @p kind; nullopt counts failed_other.
   void count_failure(std::optional<query_engine::error_kind> kind);
   /// Log @p what against @p entry's name and ordinal, throttled by the entry's failure_throttle. Never the query
   /// text.
   void report_failure(const query_entry& entry, std::string_view what);

   const config& _cfg;                                  ///< the plugin's parsed configuration; outlives the runner
   pipeline& _sink;                                     ///< receives every rendered query document
   const service_provider _provider;                    ///< the query service, looked up per execution
   fc::logger _log;                                     ///< the plugin's logger name
   std::vector<chain::block_num_type> _admitted_bucket; ///< application thread only
   std::unique_ptr<std::atomic<bool>[]> _running;       ///< per entry
   std::vector<failure_throttle> _throttles;            ///< per entry; one caller at a time (the _running hand-off)
   mutable std::mutex _stats_mtx;                       ///< guards _stats and _log
   query_stats _stats;                                  ///< see stats()
   std::shared_ptr<fc::parallel::worker_task_queue<job>> _callers; ///< null without queries; last member
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

} // namespace sysio::alert_monitor
