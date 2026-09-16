#include <algorithm>
#include <fc/exception/exception.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <filesystem>
#include <fmt/ranges.h> // fmt::join
#include <string_view>
#include <sysio/chain/exceptions.hpp>
#include <sysio/status_monitor_plugin/status_monitor.hpp>
#include <utility>

namespace sysio::status_monitor {

namespace bpo = boost::program_options;

namespace {

/// Value of the template's `${func}` token: the renderer, so `${file}:${line} ${func}` reads like a log record's.
constexpr std::string_view render_function_name = "render_document";

/// The newline that ends each NDJSON document line, paired with the client's action line.
constexpr char document_line_terminator = '\n';

/// Threads rendering snapshots into documents. A single render thread keeps snapshots in block order: a second
/// would let two renders interleave and reach the delivery queue out of the order their blocks became
/// irreversible.
constexpr uint64_t render_threads = 1;

/// Copy an optional string option into @p target when present.
void read_optional(const bpo::variables_map& options, const char* name, std::optional<std::string>& target) {
   if (options.contains(name))
      target = options[name].as<std::string>();
}

} // anonymous namespace

void add_options(bpo::options_description& cfg) {
   // The option names, the placeholder list, and the ceilings are formatted from the constants that define or
   // enforce them, so the help text can never advertise a stale option name, a token parse_config rejects, or
   // a bound that has since moved.
   const std::string target_index_description = fmt::format(
      "Index or write alias the status documents are written to. Required when --{} is set", option::target_url);
   const std::string template_file_description = fmt::format(
      "Path of the JSON document template: a JSON object whose string values may contain ${{token}} "
      "placeholders: {}. timestamp accepts :iso8601 or :epoch_millis and level accepts :upper or :lower; every "
      "other member is emitted as written. Required when --{} is set; the source tree carries a sample at "
      "etc/status_monitor/document-template.json",
      fmt::join(supplied_tokens, token_list_separator), option::target_url);
   const std::string username_description =
      fmt::format("HTTP basic auth user for the target endpoint; requires --{}", option::password);
   const std::string password_description =
      fmt::format("HTTP basic auth password for the target endpoint; requires --{}", option::username);
   const std::string max_items_per_task_description =
      fmt::format("Maximum documents per _bulk request (1 to {}); the delivery worker sends whatever is queued, "
                  "up to this many",
                  max_items_per_task_ceiling);
   const std::string max_pending_documents_description =
      fmt::format("Maximum rendered documents waiting behind the active _bulk request (1 to {}); the newest is "
                  "dropped when full",
                  max_pending_documents_ceiling);
   const std::string max_retries_description =
      fmt::format("Additional delivery attempts after the first for a 5xx, 429, timeout, or connection failure "
                  "(0 to {})",
                  max_retries_ceiling);
   const std::string retry_backoff_ms_description =
      fmt::format("Initial retry backoff in milliseconds (greater than 0); doubles per attempt, capped at {} ms",
                  fc::network::es::es_max_retry_backoff_ms);

   auto opts = cfg.add_options();
   opts(option::target_url, bpo::value<std::string>(),
        "Base URL of the OpenSearch/Elasticsearch endpoint that receives one status document per LIB advance "
        "(e.g. https://opensearch.example.com). If not provided, the plugin is disabled.");
   opts(option::target_index, bpo::value<std::string>(), target_index_description.c_str());
   opts(option::target_template_file, bpo::value<std::filesystem::path>(), template_file_description.c_str());
   opts(option::username, bpo::value<std::string>(), username_description.c_str());
   opts(option::password, bpo::value<std::string>(), password_description.c_str());
   opts(option::max_items_per_task, bpo::value<uint32_t>()->default_value(default_max_items_per_task),
        max_items_per_task_description.c_str());
   opts(option::max_pending_documents, bpo::value<uint32_t>()->default_value(default_max_pending_documents),
        max_pending_documents_description.c_str());
   opts(option::connect_timeout_ms,
        bpo::value<uint32_t>()->default_value(fc::network::es::es_default_connect_timeout_ms),
        "Connect timeout in milliseconds for each _bulk request");
   opts(option::request_timeout_ms,
        bpo::value<uint32_t>()->default_value(fc::network::es::es_default_request_timeout_ms),
        "Header, read, idle, and total timeout in milliseconds for each _bulk request");
   opts(option::max_retries, bpo::value<uint32_t>()->default_value(fc::network::es::es_default_max_retries),
        max_retries_description.c_str());
   opts(option::retry_backoff_ms, bpo::value<uint32_t>()->default_value(fc::network::es::es_default_retry_backoff_ms),
        retry_backoff_ms_description.c_str());
   sysio::outbound_http::add_transport_program_options(cfg, option::transport_option_names, "status monitor");
}

std::optional<config> parse_config(const bpo::variables_map& options) {
   if (!options.contains(option::target_url))
      return std::nullopt;
   config cfg;
   cfg.delivery.url = options[option::target_url].as<std::string>();

   SYS_ASSERT(options.contains(option::target_index), chain::plugin_config_exception,
              "--{} is required when --{} is set", option::target_index, option::target_url);
   cfg.delivery.index = options[option::target_index].as<std::string>();
   SYS_ASSERT(!cfg.delivery.index.empty(), chain::plugin_config_exception, "--{} must not be empty",
              option::target_index);

   SYS_ASSERT(options.contains(option::target_template_file), chain::plugin_config_exception,
              "--{} is required when --{} is set", option::target_template_file, option::target_url);
   const auto template_path = options[option::target_template_file].as<std::filesystem::path>();
   try {
      cfg.document_template = fc::json_template::from_file(template_path);
   } catch (const fc::exception& e) {
      SYS_THROW(chain::plugin_config_exception, "--{} ({}): {}", option::target_template_file, template_path.string(),
                e.top_message());
   }
   // The template compiles for any name; the names this plugin supplies are the contract, checked here so a typo
   // fails startup instead of every render.
   for (const auto& name : cfg.document_template.tokens()) {
      SYS_ASSERT(std::ranges::find(supplied_tokens, std::string_view{name}) != supplied_tokens.end(),
                 chain::plugin_config_exception,
                 "--{} ({}): the template references token '{}', which this plugin does not supply; available: {}",
                 option::target_template_file, template_path.string(), name,
                 fmt::join(supplied_tokens, token_list_separator));
   }

   read_optional(options, option::username, cfg.delivery.username);
   read_optional(options, option::password, cfg.delivery.password);
   cfg.delivery.connect_timeout_ms = options[option::connect_timeout_ms].as<uint32_t>();
   cfg.delivery.request_timeout_ms = options[option::request_timeout_ms].as<uint32_t>();
   cfg.delivery.max_retries = options[option::max_retries].as<uint32_t>();
   cfg.delivery.retry_backoff_ms = options[option::retry_backoff_ms].as<uint32_t>();
   cfg.delivery.max_batch_bytes = max_batch_bytes;
   cfg.delivery.max_doc_bytes = max_doc_bytes;
   cfg.max_items_per_task = options[option::max_items_per_task].as<uint32_t>();
   cfg.max_pending_documents = options[option::max_pending_documents].as<uint32_t>();
   cfg.transport = sysio::outbound_http::read_transport_options(options, option::transport_option_names);

   SYS_ASSERT(cfg.delivery.username.has_value() == cfg.delivery.password.has_value(), chain::plugin_config_exception,
              "--{} and --{} must be provided together", option::username, option::password);
   SYS_ASSERT(cfg.max_items_per_task > 0 && cfg.max_items_per_task <= max_items_per_task_ceiling,
              chain::plugin_config_exception, "--{} must be between 1 and {}", option::max_items_per_task,
              max_items_per_task_ceiling);
   SYS_ASSERT(cfg.max_pending_documents > 0 && cfg.max_pending_documents <= max_pending_documents_ceiling,
              chain::plugin_config_exception, "--{} must be between 1 and {}", option::max_pending_documents,
              max_pending_documents_ceiling);
   SYS_ASSERT(cfg.delivery.connect_timeout_ms > 0, chain::plugin_config_exception, "--{} must be greater than 0",
              option::connect_timeout_ms);
   SYS_ASSERT(cfg.delivery.request_timeout_ms > 0, chain::plugin_config_exception, "--{} must be greater than 0",
              option::request_timeout_ms);
   SYS_ASSERT(cfg.delivery.retry_backoff_ms > 0, chain::plugin_config_exception, "--{} must be greater than 0",
              option::retry_backoff_ms);
   SYS_ASSERT(cfg.delivery.max_retries <= max_retries_ceiling, chain::plugin_config_exception,
              "--{} must be between 0 and {}", option::max_retries, max_retries_ceiling);
   // Everything the client checks beyond the above is the endpoint itself (scheme, parseability), so its
   // failure is reported against the target option.
   try {
      cfg.delivery = fc::network::es::es_client::validate(std::move(cfg.delivery));
   } catch (const fc::exception& e) {
      SYS_THROW(chain::plugin_config_exception, "--{}: {}", option::target_url, e.top_message());
   }
   return cfg;
}

std::string render_document(const status_snapshot& snapshot, const fc::json_template& document_template) {
   using entry = std::pair<std::string, fc::variant_object>;
   using token = fc::json_template_default_token;
   // The snapshot's variant is the same encoding the HTTP API serializes for /v1/chain/get_info, so the nested
   // object renders byte-identically to that response body.
   const fc::variant_object data = fc::to_data(entry{std::string{record_key}, fc::variant{snapshot.info}.get_object()});
   fc::json_template_values values;
   values.set_time(snapshot.observed)
      .set(token::level, record_level)
      .set(token::message, record_message)
      .set(token::logger, logger_name)
      // __FILE__ is the absolute path the build passed to the compiler -- exactly what es_sink's ${file} carries
      // for a log record, so status and log documents agree on the field's shape.
      .set(token::file, std::string_view{__FILE__})
      .set(token::line, __LINE__)
      .set(token::func, render_function_name)
      .set(token::thread, worker_thread_label)
      .set(token::data, fc::variant{data});
   std::string document = document_template.render(values);
   document.push_back(document_line_terminator);
   return document;
}

std::vector<bulk_body> assemble_bulk_bodies(std::span<std::string> documents, std::string_view action_line,
                                            std::size_t body_byte_cap) {
   std::vector<bulk_body> bodies;
   bulk_body current;
   for (auto& document : documents) {
      const std::size_t incoming = action_line.size() + document.size();
      // Close the current body first when appending would exceed the cap, so every request body stays
      // <= body_byte_cap (+ at most one action/document pair, as es_sink allows).
      if (current.doc_count > 0 && current.body.size() + incoming > body_byte_cap) {
         bodies.push_back(std::move(current));
         current = {};
      }
      current.body.append(action_line);
      current.body.append(document);
      ++current.doc_count;
      // Consumed: the span is the delivery worker's transient buffer, so releasing each document here keeps
      // the peak at one copy instead of two until the batch is destroyed.
      document.clear();
      document.shrink_to_fit();
   }
   if (current.doc_count > 0)
      bodies.push_back(std::move(current));
   return bodies;
}

pipeline::pipeline(const config& cfg, std::string action_line, sender send, failure_reporter report_failure)
   : _document_template(cfg.document_template)
   , _action_line(std::move(action_line))
   , _body_byte_cap(cfg.delivery.max_batch_bytes)
   , _doc_byte_cap(cfg.delivery.max_doc_bytes)
   , _send(std::move(send))
   , _report_failure(std::move(report_failure)) {
   _delivery = fc::parallel::batch_task_queue<std::string>::create(
      {.max_items_per_task = cfg.max_items_per_task, .max_pending_items = cfg.max_pending_documents},
      [this](std::span<std::string> documents) { delivery_stage(documents); });
   // The second thread-bearing member is created inside a guard: a throw here skips the destructor, and the
   // delivery worker would otherwise keep a shared_ptr to a queue nobody ever stops.
   try {
      _render = fc::parallel::worker_task_queue<status_snapshot>::create(
         {.max_threads = render_threads, .max_pending_items = max_pending_snapshots},
         [this](status_snapshot& snapshot) { render_stage(snapshot); });
   } catch (...) {
      _delivery->stop();
      throw;
   }
}

pipeline::~pipeline() {
   shutdown();
}

bool pipeline::submit(status_snapshot snapshot) {
   if (!_render->try_push(std::move(snapshot))) {
      _counters.snapshots_dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
      return false;
   }
   _counters.snapshots_submitted.fetch_add(1, std::memory_order_relaxed);
   return true;
}

void pipeline::shutdown() {
   _render->discard_pending();
   _render->stop(); // joins: no render is in flight afterwards, so nothing else enters the delivery queue
   _delivery->discard_pending();
   _delivery->stop(); // joins: a batch in progress completes (or returns canceled if the client was canceled)
}

pipeline_stats pipeline::stats() const {
   pipeline_stats stats;
   stats.snapshots_submitted = _counters.snapshots_submitted.load(std::memory_order_relaxed);
   stats.snapshots_dropped_queue_full = _counters.snapshots_dropped_queue_full.load(std::memory_order_relaxed);
   stats.documents_queued = _counters.documents_queued.load(std::memory_order_relaxed);
   stats.documents_dropped_oversize = _counters.documents_dropped_oversize.load(std::memory_order_relaxed);
   stats.documents_dropped_queue_full = _counters.documents_dropped_queue_full.load(std::memory_order_relaxed);
   stats.render_failures = _counters.render_failures.load(std::memory_order_relaxed);
   stats.batches_indexed = _counters.batches_indexed.load(std::memory_order_relaxed);
   stats.batches_failed = _counters.batches_failed.load(std::memory_order_relaxed);
   stats.documents_indexed = _counters.documents_indexed.load(std::memory_order_relaxed);
   stats.documents_failed = _counters.documents_failed.load(std::memory_order_relaxed);
   return stats;
}

std::string pipeline::last_failure() const {
   std::lock_guard<std::mutex> lk(_failure_mtx);
   return _last_failure;
}

void pipeline::note_failure(std::string detail) {
   std::lock_guard<std::mutex> lk(_failure_mtx);
   _last_failure = std::move(detail);
}

void pipeline::render_stage(status_snapshot& snapshot) {
   // Worker callbacks run unguarded (a throw would unwind out of the thread pool and terminate the process):
   // every failure is counted and the item dropped. The guard spans the whole callback -- the queue push
   // allocates too -- so nothing at all escapes into the worker.
   try {
      std::string document = render_document(snapshot, _document_template);
      if (document.size() > _doc_byte_cap) {
         _counters.documents_dropped_oversize.fetch_add(1, std::memory_order_relaxed);
         return;
      }
      if (!_delivery->try_push(std::move(document))) {
         _counters.documents_dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
         return;
      }
      _counters.documents_queued.fetch_add(1, std::memory_order_relaxed);
   } catch (...) {
      _counters.render_failures.fetch_add(1, std::memory_order_relaxed);
   }
}

void pipeline::delivery_stage(std::span<std::string> documents) {
   uint64_t accounted = 0;
   try {
      for (auto& body : assemble_bulk_bodies(documents, _action_line, _body_byte_cap)) {
         const uint32_t doc_count = body.doc_count;
         const auto result = _send(std::move(body.body), doc_count);
         const bool indexed = result.outcome == fc::network::es::es_bulk_result::status::indexed;
         // Published before the counters that advertise it, so a reader that observes documents_failed or
         // batches_failed advance and then calls last_failure() never sees a stale or empty detail.
         if (!indexed) {
            note_failure(result.detail);
            // One report per failed batch, so nothing a batch carried is dropped silently. Neither the
            // reporter nor note_failure throws; if either ever did, this batch is still unaccounted for, so
            // the catch below counts its documents as the loss they are.
            if (_report_failure)
               _report_failure(result, doc_count);
         }
         // Counted as handled only past every step that could throw before the counters below are reached.
         accounted += doc_count;
         _counters.documents_indexed.fetch_add(result.indexed_docs, std::memory_order_relaxed);
         _counters.documents_failed.fetch_add(result.failed_docs, std::memory_order_relaxed);
         if (indexed) {
            _counters.batches_indexed.fetch_add(1, std::memory_order_relaxed);
         } else {
            _counters.batches_failed.fetch_add(1, std::memory_order_relaxed);
         }
      }
   } catch (...) {
      // assemble_bulk_bodies or the sender threw (es_client::bulk never does): whatever was not yet accounted
      // for is lost.
      _counters.batches_failed.fetch_add(1, std::memory_order_relaxed);
      _counters.documents_failed.fetch_add(documents.size() - accounted, std::memory_order_relaxed);
   }
}

progress classify_progress(const pipeline_stats& previous, const pipeline_stats& current, bool was_failing) {
   const bool new_failures = current.batches_failed > previous.batches_failed ||
                             current.snapshots_dropped_queue_full > previous.snapshots_dropped_queue_full ||
                             current.documents_dropped_oversize > previous.documents_dropped_oversize ||
                             current.documents_dropped_queue_full > previous.documents_dropped_queue_full ||
                             current.render_failures > previous.render_failures;
   if (new_failures)
      return progress::failing;
   if (was_failing && current.batches_indexed > previous.batches_indexed)
      return progress::recovered;
   return progress::steady;
}

} // namespace sysio::status_monitor
