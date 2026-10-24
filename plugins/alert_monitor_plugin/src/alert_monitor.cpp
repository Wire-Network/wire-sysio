#include <fc/exception/exception.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/string.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/alert_monitor_plugin/alert_monitor.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/query_engine_plugin/query_config.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h> // fmt::join

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include <regex>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace sysio::alert_monitor {

namespace bpo = boost::program_options;

namespace {

/// Value of the template's `${func}` token: the renderer, so `${file}:${line} ${func}` reads like a log record's.
constexpr std::string_view render_function_name = "render_document";
/// render_function_name's counterpart for a query document.
constexpr std::string_view render_query_function_name = "render_query_document";

/// The newline that ends each NDJSON document line, paired with the client's action line.
constexpr char document_line_terminator = '\n';

/// Threads rendering snapshots into documents. A single render thread keeps snapshots in block order: a second
/// would let two renders interleave and reach the delivery queue out of the order their blocks became
/// irreversible.
constexpr uint64_t render_threads = 1;

/// Separator of an --alert-monitor-query spec's fields.
constexpr char query_spec_separator = ',';
/// Largest --alert-monitor-query block interval: whatever the spec's uint32_t field holds.
constexpr uint32_t max_query_block_interval = std::numeric_limits<uint32_t>::max();

/// The fields of an --alert-monitor-query spec, in spec order: an error names a field by its index
/// (magic_enum::enum_integer) and its label (magic_enum::enum_name), the query_entry member it fills.
enum class query_spec_field : uint8_t { name, block_interval, query };
static_assert(magic_enum::enum_count<query_spec_field>() == query_spec_field_count,
              "every spec field has a label, and fc::split folds the remainder into the last one");

/// What a query execution's failure line says when the document was not queued: the admission outcome by name
/// (dropped_oversize or dropped_queue_full), as the pipeline_stats query_documents_* counters count them apart.
constexpr std::string_view document_dropped_failure_format = "document dropped: {}";
/// What a query execution's failure line says when the query engine offers no service (not started, or stopped).
constexpr std::string_view service_unavailable_failure = "query service unavailable";
/// What a query execution's failure line says for anything that is not a query_engine::query_error.
constexpr std::string_view unexpected_failure = "unexpected error";
/// The limit a query_error names, after its kind.
constexpr std::string_view query_error_limit_format = " (limit {})";
/// The source location a query_error names, after its kind.
constexpr std::string_view query_error_span_format = " at line {}, column {}";
/// A query_error as one clause: its kind, its limit or location (or nothing), then its message.
constexpr std::string_view query_error_format = "{}{}: {}";
/// The line logged for a failed query execution: the entry's name and ordinal -- never its text -- what failed, and
/// the unlogged_events_clause over query_failure_events.
constexpr std::string_view query_failure_format = "query '{}' (value {}) failed: {}{}";
/// What a query failure line calls the failures of its entry that its throttle held back.
constexpr std::string_view query_failure_events = "failure(s) of this query";

using fc::network::es::to_config_ms;

/// Copy an optional string option into @p target when present.
void read_optional(const bpo::variables_map& options, const char* name, std::optional<std::string>& target) {
   if (options.contains(name))
      target = options[name].as<std::string>();
}

/// Throw the error of one invalid --alert-monitor-query field: the option, the value's @p ordinal, the field's index
/// and label, @p problem, and query_spec_format. @p valid_name, when non-empty, is a name that already matched
/// query_name_pattern and is included so the operator can find the entry; no other spec text is ever reproduced,
/// because a missing field shifts query text into the name and interval positions.
[[noreturn]] void throw_invalid_query_field(uint32_t ordinal, query_spec_field field, std::string_view problem,
                                            std::string_view valid_name = {}) {
   if (valid_name.empty()) {
      SYS_THROW(chain::plugin_config_exception, "--{} value {}: field {} ({}) {}; format: {}", option::query, ordinal,
                magic_enum::enum_integer(field), magic_enum::enum_name(field), problem, query_spec_format);
   }
   SYS_THROW(chain::plugin_config_exception, "--{} value {} ('{}'): field {} ({}) {}; format: {}", option::query,
             ordinal, valid_name, magic_enum::enum_integer(field), magic_enum::enum_name(field), problem,
             query_spec_format);
}

/// @p e as a failure line or startup error states it: the kind, its limit or line and column, and its message, which
/// the engine keeps free of query text (query_error is its client-visible failure).
std::string query_error_summary(const query_engine::query_error& e) {
   std::string qualifier;
   if (e.limit)
      qualifier = fmt::format(query_error_limit_format, *e.limit);
   else if (e.span)
      qualifier = fmt::format(query_error_span_format, e.span->line, e.span->column);
   return fmt::format(query_error_format, magic_enum::enum_name(e.kind), qualifier, e.what());
}

/// Render @p document_template over the supplied_tokens: `data` set to `{ <data_key>: <data_object> }`, the three
/// timestamp entries from @p observed, `level`/`message`/`logger`/`thread` from the constants, and
/// `file`/`line`/`func` naming the calling renderer (@p line, @p func). The one body both document kinds go through,
/// so they agree on every token and on the shape of `data`. Returns one newline-terminated line.
std::string render_line(const fc::json_template& document_template, fc::time_point observed, std::string_view func,
                        int line, std::string data_key, fc::mutable_variant_object data_object) {
   using token = fc::json_template_default_token;
   using data_entry = std::pair<std::string, fc::variant_object>;
   const fc::variant_object data =
      fc::to_data(data_entry{std::move(data_key), fc::variant_object{std::move(data_object)}});
   fc::json_template_values values;
   values.set_time(observed)
      .set(token::level, record_level)
      .set(token::message, record_message)
      .set(token::logger, logger_name)
      // __FILE__ is the absolute path the build passed to the compiler -- exactly what es_sink's ${file} carries
      // for a log record, so status and log documents agree on the field's shape.
      .set(token::file, std::string_view{__FILE__})
      .set(token::line, line)
      .set(token::func, func)
      .set(token::thread, worker_thread_label)
      .set(token::data, fc::variant{data});
   std::string document = document_template.render(values);
   document.push_back(document_line_terminator);
   return document;
}

} // anonymous namespace

void add_options(bpo::options_description& cfg) {
   // The option names, the placeholder list, and the ceilings are formatted from the constants that define or
   // enforce them, so the help text can never advertise a stale option name, a token parse_config rejects, or
   // a bound that has since moved.
   const std::string target_index_description = fmt::format(
      "Index or write alias the status and query documents are written to. Required when --{} is set",
      option::target_url);
   const std::string template_file_description = fmt::format(
      "Path of the JSON document template: a JSON object whose string values may contain ${{token}} "
      "placeholders: {}. timestamp accepts :iso8601 or :epoch_millis and level accepts :upper or :lower; every "
      "other member is emitted as written. Required when --{} is set; the source tree carries a sample at "
      "etc/alert_monitor/document-template.json",
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
                  fc::network::es::es_max_retry_backoff.count());
   const std::string query_description = fmt::format(
      "A WireQL query this node runs at its own LIB interval, sending each result as one document through the same "
      "template and endpoint as the status documents; repeatable (repeat the key in config.ini, repeat the flag on "
      "the command line). Format: {}; everything after the second comma is the query, commas included. The name "
      "matches {} over the whole string, is unique, is not a reserved name ({}), and is the ${{data}} key of the "
      "query's documents (so the template's ${{data}} must be a whole value). The block_interval is 1 to {}: the "
      "query runs once per LIB bucket of that many blocks (first at LIB block_interval), never catching up missed "
      "buckets. Each query is parsed at startup. Every execution has a fixed {} ms overall deadline in place of the "
      "node's --{}, so queries must stay small; each chain read within it is still bounded by --{}, which by default "
      "is the smaller of --{} and the read-only transaction time. Queries share the engine's --{} and --{} with each "
      "other and with HTTP queries; an execution refused as busy is retried within its deadline. Requires --{} and "
      "plugin = {}, which refuses producer nodes. A query given in config.ini can contain neither # nor a newline, "
      "and loses trailing spaces and tabs. On the command line they are fine, but the spec must be quoted there, "
      "because the option takes multiple tokens",
      query_spec_format, query_name_pattern, fmt::join(reserved_query_names, token_list_separator),
      max_query_block_interval, query_deadline.count(), query_engine::option::timeout_ms,
      query_engine::option::max_capture_ms, query_engine::option::timeout_ms, query_engine::option::worker_threads,
      query_engine::option::max_in_flight, option::target_url, query_engine_plugin_name);

   auto opts = cfg.add_options();
   opts(option::target_url, bpo::value<std::string>(),
        "Base URL of the OpenSearch/Elasticsearch endpoint that receives one status document per LIB advance and "
        "the results of the configured queries (e.g. https://opensearch.example.com). If not provided, the plugin is "
        "disabled.");
   opts(option::target_index, bpo::value<std::string>(), target_index_description.c_str());
   opts(option::target_template_file, bpo::value<std::filesystem::path>(), template_file_description.c_str());
   opts(option::username, bpo::value<std::string>(), username_description.c_str());
   opts(option::password, bpo::value<std::string>(), password_description.c_str());
   opts(option::max_items_per_task, bpo::value<uint32_t>()->default_value(default_max_items_per_task),
        max_items_per_task_description.c_str());
   opts(option::max_pending_documents, bpo::value<uint32_t>()->default_value(default_max_pending_documents),
        max_pending_documents_description.c_str());
   opts(option::connect_timeout_ms,
        bpo::value<uint32_t>()->default_value(to_config_ms(fc::network::es::es_default_connect_timeout)),
        "Connect timeout in milliseconds for each _bulk request");
   opts(option::request_timeout_ms,
        bpo::value<uint32_t>()->default_value(to_config_ms(fc::network::es::es_default_request_timeout)),
        "Header, read, idle, and total timeout in milliseconds for each _bulk request");
   opts(option::max_retries, bpo::value<uint32_t>()->default_value(fc::network::es::es_default_max_retries),
        max_retries_description.c_str());
   opts(option::retry_backoff_ms,
        bpo::value<uint32_t>()->default_value(to_config_ms(fc::network::es::es_default_retry_backoff)),
        retry_backoff_ms_description.c_str());
   opts(option::query, bpo::value<std::vector<std::string>>()->composing()->multitoken(), query_description.c_str());
   sysio::outbound_http::add_transport_program_options(cfg, option::transport_option_names, "alert monitor");
}

query_entry parse_query_entry(std::string_view spec, uint32_t ordinal) {
   // Built once, on first use; matched over the whole string (std::regex_match), never searched.
   static const std::regex name_regex{query_name_pattern.begin(), query_name_pattern.end()};
   auto fields = fc::split(spec, query_spec_separator, query_spec_field_count);
   SYS_ASSERT(fields.size() == query_spec_field_count, chain::plugin_config_exception,
              "--{} value {}: expected {} comma-separated fields, but got {}; format: {}", option::query, ordinal,
              query_spec_field_count, fields.size(), query_spec_format);
   std::string& name = fields[magic_enum::enum_integer(query_spec_field::name)];
   std::string& interval_text = fields[magic_enum::enum_integer(query_spec_field::block_interval)];
   std::string& query = fields[magic_enum::enum_integer(query_spec_field::query)];

   if (!std::regex_match(name, name_regex)) {
      throw_invalid_query_field(
         ordinal, query_spec_field::name,
         fmt::format("is not snake_case (it must match {} over the whole string)", query_name_pattern));
   }
   if (std::ranges::find(reserved_query_names, std::string_view{name}) != reserved_query_names.end()) {
      throw_invalid_query_field(
         ordinal, query_spec_field::name,
         fmt::format("is a reserved name (reserved: {})", fmt::join(reserved_query_names, token_list_separator)));
   }
   // The whole field, unsigned and in range: from_chars refuses a sign, leading whitespace, and an empty field,
   // and stops early on a fraction or trailing text, which the end check then rejects.
   uint32_t block_interval = 0;
   const char* const interval_end = interval_text.data() + interval_text.size();
   const auto [parsed_end, error] = std::from_chars(interval_text.data(), interval_end, block_interval);
   if (error != std::errc{} || parsed_end != interval_end || block_interval == 0) {
      throw_invalid_query_field(ordinal, query_spec_field::block_interval,
                                fmt::format("is not an integer from 1 to {}", max_query_block_interval), name);
   }
   if (query.find_first_not_of(query_blank_characters) == std::string::npos)
      throw_invalid_query_field(ordinal, query_spec_field::query, "is blank", name);

   return query_entry{
      .ordinal = ordinal, .name = std::move(name), .query = std::move(query), .block_interval = block_interval};
}

bool is_due(chain::block_num_type lib, uint32_t interval, chain::block_num_type admitted_bucket) {
   if (interval == 0)
      return false;
   const chain::block_num_type bucket = lib / interval;
   return bucket != 0 && bucket > admitted_bucket;
}

std::optional<config> parse_config(const bpo::variables_map& options, const std::filesystem::path& config_dir) {
   // Before the disabled return below: without it a configured query would be dropped silently along with the
   // plugin, which is never what an operator who wrote one intends.
   SYS_ASSERT(!options.contains(option::query) || options.contains(option::target_url), chain::plugin_config_exception,
              "--{} requires --{}", option::query, option::target_url);
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
   auto template_path = options[option::target_template_file].as<std::filesystem::path>();
   // Relative to the config directory, as chain_plugin resolves protocol-features-dir, so a node directory that is
   // moved or archived as a whole still finds its template.
   if (template_path.is_relative())
      template_path = config_dir / template_path;
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

   if (options.contains(option::query)) {
      const auto& specs = options[option::query].as<std::vector<std::string>>();
      cfg.queries.reserve(specs.size());
      for (uint32_t ordinal = 0; ordinal < specs.size(); ++ordinal) {
         query_entry entry = parse_query_entry(specs[ordinal], ordinal);
         // A name is a document's `${data}` key and its OpenSearch field path, so two queries sharing one would
         // write two shapes to one field.
         const auto earlier = std::ranges::find(cfg.queries, entry.name, &query_entry::name);
         SYS_ASSERT(earlier == cfg.queries.end(), chain::plugin_config_exception,
                    "--{} value {} ('{}'): the name is already used by value {}; every query needs its own name",
                    option::query, ordinal, entry.name, earlier->ordinal);
         cfg.queries.push_back(std::move(entry));
      }
      // A query's result is its document's ${data}: without the token every query document would be sent empty.
      SYS_ASSERT(cfg.document_template.contains(fc::json_template_default_token::data), chain::plugin_config_exception,
                 "--{} ({}): the template must reference ${{{}}} when --{} is set", option::target_template_file,
                 template_path.string(), magic_enum::enum_name(fc::json_template_default_token::data), option::query);
   }

   read_optional(options, option::username, cfg.delivery.username);
   read_optional(options, option::password, cfg.delivery.password);
   cfg.delivery.connect_timeout = std::chrono::milliseconds{options[option::connect_timeout_ms].as<uint32_t>()};
   cfg.delivery.request_timeout = std::chrono::milliseconds{options[option::request_timeout_ms].as<uint32_t>()};
   cfg.delivery.max_retries = options[option::max_retries].as<uint32_t>();
   cfg.delivery.retry_backoff = std::chrono::milliseconds{options[option::retry_backoff_ms].as<uint32_t>()};
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
   SYS_ASSERT(cfg.delivery.connect_timeout > std::chrono::milliseconds::zero(), chain::plugin_config_exception,
              "--{} must be greater than 0", option::connect_timeout_ms);
   SYS_ASSERT(cfg.delivery.request_timeout > std::chrono::milliseconds::zero(), chain::plugin_config_exception,
              "--{} must be greater than 0", option::request_timeout_ms);
   SYS_ASSERT(cfg.delivery.retry_backoff > std::chrono::milliseconds::zero(), chain::plugin_config_exception,
              "--{} must be greater than 0", option::retry_backoff_ms);
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

void validate_queries(const std::vector<query_entry>& queries, const bpo::variables_map& options) {
   if (queries.empty())
      return;
   query_engine::query_config engine_config;
   try {
      engine_config = query_engine::parse_config(options);
   } catch (const query_engine::query_error& e) {
      SYS_THROW(chain::plugin_config_exception, "--{}: invalid query engine configuration: {}", option::query,
                query_error_summary(e));
   }
   for (const query_entry& entry : queries) {
      try {
         // The deadline an execution gets, as query_runner::execute passes it; never --query-timeout-ms.
         query_engine::query_budget budget{engine_config, query_engine::query_budget::clock::now,
                                           query_engine::query_options{.timeout = query_deadline}};
         query_engine::parse_query(entry.query, budget);
      } catch (const query_engine::query_error& e) {
         SYS_THROW(chain::plugin_config_exception, "--{} value {} ('{}'): the query engine rejects the query: {}",
                   option::query, entry.ordinal, entry.name, query_error_summary(e));
      }
   }
}

std::string render_document(const status_snapshot& snapshot, const fc::json_template& document_template) {
   namespace key = status_document::key;
   fc::mutable_variant_object document;
   document(key::record, status_document::record)(key::version, status_document::version);
   // The snapshot's variant is the one the HTTP API serializes for /v1/chain/get_info, so `status` has the same fields
   // as that response body, with integers as JSON numbers.
   document(key::status, fc::variant{snapshot.info});
   return render_line(document_template, snapshot.observed, render_function_name, __LINE__, std::string{record_key},
                      std::move(document));
}

std::string render_query_document(const query_entry& entry, const query_trigger& trigger, fc::time_point observed,
                                  const query_engine::query_result& result,
                                  const fc::json_template& document_template) {
   namespace key = query_document::key;
   // entry.query is deliberately absent: a document names its query, it never carries the query's text. `result` is
   // the engine's reflected result as returned -- its `state` is the state the engine actually captured, which is not
   // relabelled as the trigger block.
   fc::mutable_variant_object query_member;
   query_member(key::name, entry.name)(key::ordinal, entry.ordinal)(key::block_interval, entry.block_interval);
   fc::mutable_variant_object trigger_member;
   trigger_member(key::lib_num, trigger.lib_num)(key::lib_id, trigger.lib_id)(key::lib_time, trigger.lib_time);
   fc::mutable_variant_object document;
   document(key::record, query_document::record)(key::version, query_document::version);
   document(key::query, std::move(query_member))(key::trigger, std::move(trigger_member));
   document(key::observed_at, observed)(key::result, fc::variant{result});
   return render_line(document_template, observed, render_query_function_name, __LINE__, entry.name,
                      std::move(document));
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
   const bool admitted = _render->try_push(std::move(snapshot));
   count(admitted ? &pipeline_stats::snapshots_submitted : &pipeline_stats::snapshots_dropped_queue_full);
   return admitted;
}

admission pipeline::submit_document(std::string document) {
   return admit(std::move(document), query_admission);
}

void pipeline::shutdown() {
   _render->discard_pending();
   _render->stop(); // joins: no render is in flight afterwards, so nothing else enters the delivery queue
   // Stopped before discarded: stop() refuses every later submit_document() and takes no further batch, so the
   // discard is the last word on the queue. Discarding first would leave a document admitted in between unsent,
   // yet counted as queued.
   _delivery->stop(); // joins: a batch in progress completes (or returns canceled if the client was canceled)
   _delivery->discard_pending();
}

pipeline_stats pipeline::stats() const {
   std::lock_guard<std::mutex> lk(_stats_mtx);
   return _stats;
}

std::string pipeline::last_failure() const {
   std::lock_guard<std::mutex> lk(_stats_mtx);
   return _last_failure;
}

void pipeline::count(uint64_t pipeline_stats::*counter, uint64_t n) {
   std::lock_guard<std::mutex> lk(_stats_mtx);
   _stats.*counter += n;
}

void pipeline::render_stage(status_snapshot& snapshot) {
   // Worker callbacks run unguarded (a throw would unwind out of the thread pool and terminate the process):
   // every failure is counted and the item dropped. The guard spans the whole callback -- the queue push
   // allocates too -- so nothing at all escapes into the worker. admit() counts the outcome; the render worker has
   // nobody to return it to.
   try {
      admit(render_document(snapshot, _document_template), status_admission);
   } catch (...) {
      count(&pipeline_stats::render_failures);
   }
}

admission pipeline::admit(std::string document, const admission_counters& counters) {
   if (document.size() > _doc_byte_cap) {
      count(counters.dropped_oversize);
      return admission::dropped_oversize;
   }
   if (!_delivery->try_push(std::move(document))) {
      count(counters.dropped_queue_full);
      return admission::dropped_queue_full;
   }
   count(counters.queued);
   return admission::queued;
}

void pipeline::delivery_stage(std::span<std::string> documents) {
   uint64_t accounted = 0;
   try {
      for (auto& body : assemble_bulk_bodies(documents, _action_line, _body_byte_cap)) {
         const uint32_t doc_count = body.doc_count;
         const auto result = _send(std::move(body.body), doc_count);
         const bool indexed = result.outcome == fc::network::es::es_bulk_result::status::indexed;
         // One report per failed batch, so nothing a batch carried is dropped silently. The reporter does not
         // throw; if it ever did, this batch is still unaccounted for, so the catch below counts its documents as
         // the loss they are.
         if (!indexed && _report_failure)
            _report_failure(result, doc_count);
         // Counted as handled only past every step that could throw before the counters below are reached.
         accounted += doc_count;
         // One critical section per batch, so stats() and last_failure() see its counters and detail together.
         std::lock_guard<std::mutex> lk(_stats_mtx);
         _stats.documents_indexed += result.indexed_docs;
         _stats.documents_failed += result.failed_docs;
         if (indexed) {
            ++_stats.batches_indexed;
         } else {
            ++_stats.batches_failed;
            _last_failure = result.detail;
         }
      }
   } catch (...) {
      // assemble_bulk_bodies or the sender threw (es_client::bulk never does): whatever was not yet accounted
      // for is lost.
      std::lock_guard<std::mutex> lk(_stats_mtx);
      ++_stats.batches_failed;
      _stats.documents_failed += documents.size() - accounted;
   }
}

std::optional<uint64_t> failure_throttle::admit(fc::time_point now) {
   if (now - last_report < failure_report_interval) {
      ++unlogged;
      return std::nullopt;
   }
   last_report = now;
   return std::exchange(unlogged, 0);
}

std::string unlogged_events_clause(uint64_t unlogged, std::string_view events) {
   if (unlogged == 0)
      return {};
   return fmt::format(unlogged_events_format, unlogged, events);
}

query_runner::query_runner(const config& cfg, pipeline& sink, service_provider provider, const fc::logger& log)
   : _cfg(cfg)
   , _sink(sink)
   , _provider(std::move(provider))
   , _log(log)
   , _admitted_bucket(cfg.queries.size(), 0)
   , _running(std::make_unique<std::atomic<bool>[]>(cfg.queries.size()))
   , _throttles(cfg.queries.size()) {
   if (_cfg.queries.empty())
      return;
   // One caller per entry, so no entry waits for another's caller (the engine's workers are still shared); and room
   // for one waiting job per entry, which _running already guarantees, so the pending bound never refuses a job
   // dispatch() admitted.
   _callers = fc::parallel::worker_task_queue<job>::create(
      {.max_threads = _cfg.queries.size(), .max_pending_items = _cfg.queries.size()}, [this](job& j) { run(j); });
}

query_runner::~query_runner() {
   shutdown();
}

void query_runner::dispatch(const query_trigger& trigger) {
   for (uint32_t i = 0; _callers && i < _cfg.queries.size(); ++i) {
      const auto interval = _cfg.queries[i].block_interval;
      if (!is_due(trigger.lib_num, interval, _admitted_bucket[i]))
         continue;
      _admitted_bucket[i] = trigger.lib_num / interval;
      if (_running[i].exchange(true)) { // the previous run of this entry is still active
         count(&query_stats::skipped_busy);
         continue;
      }
      try {
         // Refused only after stop(); the plugin never dispatches then, as its shutdown unsubscribes first.
         if (!_callers->try_push({i, trigger}))
            _running[i] = false;
      } catch (...) {
         _running[i] = false; // never leave an entry stuck as busy
         throw;               // the plugin's FC_LOG_AND_DROP reports it
      }
   }
}

void query_runner::shutdown() {
   _stopping = true;
   if (!_callers)
      return;
   _callers->discard_pending();
   _callers->stop(); // joins: a caller inside execute() returns by its query_deadline
}

query_stats query_runner::stats() const {
   std::lock_guard<std::mutex> lk(_stats_mtx);
   return _stats;
}

void query_runner::set_logger(const fc::logger& log) {
   std::lock_guard<std::mutex> lk(_stats_mtx);
   _log = log;
}

void query_runner::run(job& j) noexcept {
   // noexcept and fully guarded, handlers included: an escaping exception would terminate the process
   // (worker_task_queue contract).
   try {
      const auto& entry = _cfg.queries[j.ordinal];
      try {
         if (auto service = _provider()) {
            // A fixed per-call deadline in place of the node's query-timeout-ms (30 s on API nodes); it bounds
            // shutdown() in either plugin shutdown order. Chain reads keep the engine's query-max-capture-ms.
            auto result = execute(*service, entry);
            // ${data} = { "<entry.name>": { record, version, query, trigger, observed_at, result } }
            const admission outcome = _sink.submit_document(
               render_query_document(entry, j.trigger, fc::time_point::now(), result, _cfg.document_template));
            if (outcome == admission::queued)
               count(&query_stats::succeeded);
            else
               report_failure(entry, fmt::format(document_dropped_failure_format, magic_enum::enum_name(outcome)));
         } else {
            count_failure(std::nullopt);
            report_failure(entry, service_unavailable_failure);
         }
      } catch (const query_engine::query_error& e) {
         count_failure(e.kind);
         report_failure(entry, query_error_summary(e));
      } catch (...) {
         count_failure(std::nullopt);
         report_failure(entry, unexpected_failure);
      }
   } catch (...) {}
   _running[j.ordinal] = false;
}

query_engine::query_result query_runner::execute(query_engine::query_service& service, const query_entry& entry) {
   using clock = std::chrono::steady_clock;
   const clock::time_point deadline = clock::now() + query_deadline;
   std::chrono::milliseconds timeout = query_deadline;
   for (;;) {
      try {
         return service.execute(entry.query, query_engine::query_options{.timeout = timeout});
      } catch (const query_engine::query_error& e) {
         if (e.kind != query_engine::error_kind::QUERY_BUSY || _stopping)
            throw;
         std::this_thread::sleep_for(query_busy_retry_interval);
         timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
         if (timeout <= std::chrono::milliseconds::zero() || _stopping)
            throw;
      }
   }
}

void query_runner::count(uint64_t query_stats::*counter) {
   std::lock_guard<std::mutex> lk(_stats_mtx);
   ++(_stats.*counter);
}

void query_runner::count_failure(std::optional<query_engine::error_kind> kind) {
   // magic_enum, never a cast: a kind outside the declared enumerators has no slot and lands in failed_other.
   std::optional<std::size_t> slot;
   if (kind)
      slot = magic_enum::enum_index(*kind);
   std::lock_guard<std::mutex> lk(_stats_mtx);
   if (slot)
      ++_stats.failed[*slot];
   else
      ++_stats.failed_other;
}

void query_runner::report_failure(const query_entry& entry, std::string_view what) {
   // Only the caller running this entry touches its throttle (the _running hand-off), so it needs no lock.
   const std::optional<uint64_t> unlogged = _throttles[entry.ordinal].admit(fc::time_point::now());
   if (!unlogged)
      return;
   // A copy, taken under the mutex set_logger() writes under, so a SIGHUP rebind never races this line.
   fc::logger log = [this] {
      std::lock_guard<std::mutex> lk(_stats_mtx);
      return _log;
   }();
   fc_wlog(log, query_failure_format, entry.name, entry.ordinal, what,
           unlogged_events_clause(*unlogged, query_failure_events));
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

} // namespace sysio::alert_monitor
