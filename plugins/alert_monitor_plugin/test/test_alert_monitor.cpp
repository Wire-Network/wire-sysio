#include <algorithm>
#include <array>
#include <atomic>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fc-test/capture_http_server.hpp>
#include <fc/exception/exception.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>
#include <fc/io/json_template.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/es/es_client.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <magic_enum/magic_enum.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <query_fixture.hpp> // the query engine's chain fixture, read-only (see test/CMakeLists.txt)
#include <semaphore>
#include <set>
#include <span>
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sysio/alert_monitor_plugin/alert_monitor.hpp>
#include <sysio/alert_monitor_plugin/alert_monitor_plugin.hpp>
#include <sysio/chain/app.hpp> // detail::log_non_default_options, nodeop's "Non-default options" line
#include <sysio/chain/exceptions.hpp>
#include <sysio/query_engine_plugin/query_config.hpp>
#include <sysio/query_engine_plugin/query_engine.hpp>
#include <thread>
#include <utility>
#include <vector>

using namespace sysio;
using namespace sysio::chain::literals;
using fc::test::split_bulk_lines;
namespace bpo = boost::program_options;

namespace {

constexpr std::string_view target_argument = "--alert-monitor-target-url=http://127.0.0.1:9";
constexpr std::string_view index_argument = "--alert-monitor-target-index=test-status";
/// The template argument up to its path; template_file::argument() and the relative-path cases append one.
constexpr std::string_view template_argument_prefix = "--alert-monitor-target-template-file=";
/// The smallest useful template: the snapshot and nothing else.
constexpr std::string_view data_only_template = R"({"data":"${data}"})";
/// The get_info_results fields sample_info() populates (fc omits unset std::optional members).
constexpr auto expected_get_info_fields = std::to_array<std::string_view>(
   {"server_version", "chain_id", "head_block_num", "last_irreversible_block_num", "last_irreversible_block_id",
    "head_block_id", "head_block_time", "head_block_producer", "virtual_block_cpu_limit", "virtual_block_net_limit",
    "block_cpu_limit", "block_net_limit", "fork_db_head_block_num", "total_cpu_weight"});
/// A 64-hex block id for the sample snapshot's LIB (block 41).
constexpr std::string_view sample_lib_block_id = "0000002900000000000000000000000000000000000000000000000000000000";
/// Above 0xffffffff, which the HTTP body quotes; a status document carries it as a JSON number.
constexpr uint64_t wide_weight = uint64_t{1} << 40;
constexpr uint32_t sample_head_block_num = 42;
/// A fixed wall-clock reference; the render tests pin its epoch-millis rendering.
const fc::time_point reference_now = fc::time_point::from_iso_string("2026-01-02T03:04:05.123");
constexpr int64_t reference_now_sub_second_millis = 123;
constexpr int64_t millis_per_second = 1000;
constexpr std::string_view action_line = R"({"index":{"_index":"test-status"}})"
                                         "\n";
constexpr uint32_t small_batch = 4;
constexpr uint64_t snapshot_count = 10;
constexpr uint64_t failing_count = 3;
constexpr auto drain_wait = std::chrono::seconds(5);
constexpr auto drain_poll = std::chrono::milliseconds(10);
constexpr std::string_view unavailable_detail = "HTTP 503 from 127.0.0.1:9";
/// The logging layout's extra-fields token: outside supplied_tokens, so parse_config rejects a template that
/// references it and the help text must not advertise it.
constexpr std::string_view rejected_token = magic_enum::enum_name(fc::json_template_default_token::extra_object);
/// A delivery queue one document deep, so everything behind the held send is dropped.
constexpr uint32_t single_pending_document = 1;
/// A single-document cap below any rendered document's size.
constexpr uint32_t tiny_doc_byte_cap = 8;
/// Every --alert-monitor-* option the component registers; both option cases iterate it.
constexpr auto every_alert_monitor_option = std::to_array<std::string_view>(
   {alert_monitor::option::target_url, alert_monitor::option::target_index,
    alert_monitor::option::target_template_file, alert_monitor::option::username, alert_monitor::option::password,
    alert_monitor::option::max_items_per_task, alert_monitor::option::max_pending_documents,
    alert_monitor::option::connect_timeout_ms, alert_monitor::option::request_timeout_ms,
    alert_monitor::option::max_retries, alert_monitor::option::retry_backoff_ms,
    alert_monitor::option::additional_ca_file, alert_monitor::option::additional_ca_path,
    alert_monitor::option::proxy, alert_monitor::option::query});
/// Transport arguments: a node-wide proxy and CA file, and the plugin's own proxy.
constexpr std::string_view global_proxy_argument = "--outbound-http-proxy=http://global-proxy:3128";
constexpr std::string_view global_ca_file_argument = "--outbound-http-additional-ca-file=/tmp/global-ca.pem";
constexpr std::string_view alert_monitor_proxy_argument = "--alert-monitor-proxy=http://status-proxy:3128";
constexpr std::string_view alert_monitor_proxy = "http://status-proxy:3128";
constexpr std::string_view global_ca_file = "/tmp/global-ca.pem";

// --- query specs ---

/// One query of the command-line/config.ini case: the entry parse_config must produce for it.
struct expected_query {
   std::string_view name;   ///< query_entry::name
   uint32_t block_interval; ///< query_entry::block_interval
   std::string_view query;  ///< query_entry::query
};
/// The queries the command-line/config.ini case configures, in the order parse_config must list them: the first
/// command_line_query_count come from the command line, the rest from config.ini (appbase stores the command line
/// first, so a composing option lists its command-line values first).
constexpr auto command_line_then_config_ini_queries = std::to_array<expected_query>({
   {"from_cli_a", 1, "SELECT 1"},
   {"from_cli_b", 2, "SELECT 2"},
   {"from_ini_a", 10, "SELECT 10"},
   {"from_ini_b", 100, "SELECT 100"},
});
/// How many of command_line_then_config_ini_queries the command line carries.
constexpr std::size_t command_line_query_count = 2;
/// A spec whose query contains commas; everything after the second comma is the query.
constexpr std::string_view comma_query_spec = "n,1,SELECT a, b FROM t";
/// The query comma_query_spec must yield: whole, commas included.
constexpr std::string_view comma_query = "SELECT a, b FROM t";
/// A config.ini value with a trailing comment: config.ini cuts the line at its first `#` and trims what remains.
constexpr std::string_view commented_query_spec = "n,1,SELECT 1 # note";
/// The query commented_query_spec must yield.
constexpr std::string_view commented_query = "SELECT 1";
/// A valid query with every field distinctive; its spec is built with spec_of() and parsed at full_query_ordinal.
constexpr expected_query full_query{"pending_underwrites", 10, "SELECT COUNT(*) AS n FROM uwreqs OWNER 'sysio.uwrit'"};
/// The nonzero ordinal full_query is parsed at.
constexpr uint32_t full_query_ordinal = 3;
/// The largest interval: the whole uint32_t range is accepted.
constexpr std::string_view max_interval_spec = "n,4294967295,Q";
/// A query with surrounding spaces: parse_query_entry keeps the query field exactly as given (only config.ini trims).
constexpr std::string_view padded_query_spec = "n,1, SELECT 1 ";
/// The query padded_query_spec must yield.
constexpr std::string_view padded_query = " SELECT 1 ";
/// Specs with fewer than query_spec_field_count fields.
constexpr auto too_few_fields_specs = std::to_array<std::string_view>({"", "n", "n,1"});
/// Specs whose block_interval is empty, not a number, signed, padded, fractional, zero, or above UINT32_MAX.
constexpr auto bad_interval_specs = std::to_array<std::string_view>(
   {"n,,Q", "n,x,Q", "n,-1,Q", "n,+1,Q", "n, 1,Q", "n,1.5,Q", "n,0,Q", "n,4294967296,Q"});
/// Specs whose name is empty, not snake_case, carries a trailing newline (reachable from the command line, and
/// rejected only because the name is matched over the whole string), or is reserved.
constexpr auto bad_name_specs = std::to_array<std::string_view>(
   {",1,Q", "Pending,1,Q", "1abc,1,Q", "a-b,1,Q", "abc\n,1,Q", "alert_monitor,1,Q", "thread,1,Q"});
/// Specs whose query is empty or consists only of query_blank_characters.
constexpr auto blank_query_specs = std::to_array<std::string_view>({"n,1,", "n,1,   ", "n,1,\t\v\f"});
/// What a field-count error says; it names the expected and actual counts, never a field.
constexpr std::string_view field_count_error_marker = "comma-separated fields, but got";
/// How a field error names the failing field: its index and label.
constexpr std::string_view name_field_error_marker = "field 0 (name)";
constexpr std::string_view interval_field_error_marker = "field 1 (block_interval)";
constexpr std::string_view query_field_error_marker = "field 2 (query)";
/// A distinctive valid name: once a name is valid, the interval, query, and duplicate-name errors carry it.
constexpr std::string_view echoed_name = "pending_underwrites";
/// echoed_name with an invalid interval.
constexpr std::string_view echoed_name_bad_interval_spec = "pending_underwrites,x,Q";
/// echoed_name with a blank query.
constexpr std::string_view echoed_name_blank_query_spec = "pending_underwrites,1,";
/// Specs that push query text into the name or interval position: query text in the interval field (twice) and in
/// the name field. No error may reproduce any of it.
constexpr auto query_text_in_other_fields_specs =
   std::to_array<std::string_view>({"SELECT a, b FROM t", "pending,SELECT a, b FROM t", "SELECT a, b, c FROM t"});
/// What every query text in these cases starts with, so a message carrying query text is found by it.
constexpr std::string_view query_text_marker = "SELECT";
/// A short spec for the cases that only need some valid query.
constexpr std::string_view minimal_query_spec = "n,1,Q";
/// What nodeop's "Non-default options" line writes in place of a masked option value.
constexpr std::string_view masked_option_value = "***";
/// minimal_query_spec's name with a different interval and query: a duplicate name.
constexpr std::string_view duplicate_name_spec = "n,2,R";
/// A query the engine's parser rejects; it starts with query_text_marker, so an error echoing it is found.
constexpr std::string_view unparseable_query = "SELECT FROM WHERE";
/// How a startup parse error locates a single-line query's failure.
constexpr std::string_view syntax_error_location_marker = "at line 1, column";
/// A valid query with `#` inside a literal: config.ini cuts it at the `#`, leaving the literal unclosed.
constexpr std::string_view hash_in_literal_spec =
   "cut_entry,1,SELECT key.id AS id FROM sample.positions WHERE code = 'x#y'";
/// A template that never references ${data}.
constexpr std::string_view no_data_template = R"({"message":"${message}"})";
/// An engine query-size limit below every query of these cases.
constexpr std::string_view tiny_query_bytes_argument = "--query-max-query-bytes=8";
/// An engine configuration the engine itself rejects.
constexpr std::string_view zero_in_flight_argument = "--query-max-in-flight=0";

// --- query scheduling and execution ---

/// The query texts the scripted query service answers; any other text is answered at once.
namespace scripted_query {
constexpr std::string_view answer = "SELECT answer";                     ///< a small result, at once
constexpr std::string_view syntax_error = "SELECT syntax_error";         ///< query_error QUERY_SYNTAX
constexpr std::string_view limit_error = "SELECT limit_error";           ///< query_error QUERY_LIMIT naming a limit
constexpr std::string_view unexpected_error = "SELECT unexpected_error"; ///< a std::runtime_error, not a query_error
constexpr std::string_view blocked = "SELECT blocked";                   ///< waits for the gate or its deadline
constexpr std::string_view busy = "SELECT busy";                         ///< query_error QUERY_BUSY, every time
constexpr std::string_view busy_then_answer = "SELECT busy_then_answer"; ///< QUERY_BUSY scripted_busy_refusals times
} // namespace scripted_query
/// How many times busy_then_answer is refused before it answers.
constexpr std::size_t scripted_busy_refusals = 2;
/// The bound on shutdown() while a caller retries QUERY_BUSY: well under the query_deadline the retry would
/// otherwise run out.
constexpr auto busy_shutdown_bound = alert_monitor::query_deadline / 2;
/// The message of every failure the scripted query service throws.
constexpr std::string_view scripted_failure_message = "scripted failure";
/// The limit the scripted QUERY_LIMIT failure names.
constexpr std::string_view scripted_limit = sysio::query_engine::option::max_result_rows;
/// The interval of the runner cases' entries: due at every LIB.
constexpr uint32_t every_block = 1;
/// The intervals of the due-entries case.
constexpr uint32_t every_ten_blocks = 10;
constexpr uint32_t every_hundred_blocks = 100;
/// The first LIB a runner case dispatches: bucket 1 of every_block.
constexpr chain::block_num_type first_lib = 1;
/// More entries than a pool of four callers could run at once: all of them blocked inside execute together proves
/// one caller per entry.
constexpr std::size_t concurrent_entry_count = 6;
/// Name stem of the entries repeated_specs() builds (`<stem>_<index>` is snake_case).
constexpr std::string_view repeated_entry_stem = "entry";
/// Dispatches the skip case makes while the first run is still blocked.
constexpr uint64_t busy_dispatches = 2;
/// How many times query_deadline shutdown and destruction may take with every caller blocked: each blocked caller
/// returns at its deadline, so only a caller that is never released can exceed this.
constexpr int shutdown_bound_factor = 10;
/// The bound on shutdown and destruction with every caller blocked.
constexpr auto shutdown_bound = alert_monitor::query_deadline * shutdown_bound_factor;
/// A document submitted directly to fill the delivery queue in the queue-full case.
constexpr std::string_view filler_document = R"({"filler":1})"
                                             "\n";
/// Name of the logger the captured_log cases hand to a component.
constexpr std::string_view captured_logger_name = "alert_monitor_test";
/// Failures the repeated-failure case waits for: the first is logged, the rest fall inside failure_report_interval.
constexpr uint64_t repeated_failures = 2;
/// How a query failure line quotes an entry's name (`query '<name>' (value <ordinal>) failed: ...`).
constexpr std::string_view quoted_name_format = "'{}'";

// --- failure throttle ---

/// A step well inside failure_report_interval.
constexpr fc::microseconds inside_interval_step = fc::seconds(1);
/// The smallest step: one short of the interval is still inside it.
constexpr fc::microseconds smallest_step = fc::microseconds{1};
/// What the held-back clause cases call their events.
constexpr std::string_view throttled_events = "test event(s)";

// --- end to end ---

/// The end-to-end query: the remainder of its spec carries commas through to the engine.
constexpr std::string_view end_to_end_query_name = "positions_by_amount";
/// The end-to-end query's interval: due at every LIB.
constexpr uint32_t end_to_end_block_interval = 1;
/// The end-to-end query's WireQL text, over the chain fixture's seeded positions.
constexpr std::string_view end_to_end_query = "SELECT key.id AS id, amount FROM sample.positions ORDER BY amount DESC";
/// Rows the chain fixture seeds into sample.positions.
constexpr std::size_t fixture_position_rows = 3;
/// The table the end-to-end query reads.
constexpr std::string_view fixture_positions_table = "positions";

/// A temporary directory holding a template file written from @p text; the path is an option argument.
struct template_file {
   fc::temp_directory dir;
   std::filesystem::path path = dir.path() / "template.json";

   explicit template_file(std::string_view text) {
      std::ofstream out{path};
      out << text;
   }
   std::string argument() const { return std::string{template_argument_prefix} + path.string(); }
};

/// Register the component's option surface on @p cfg (no application, no plugin class), plus the node-wide outbound
/// HTTP options http_client_plugin registers in nodeop.
void add_component_options(bpo::options_description& cfg) {
   alert_monitor::add_options(cfg);
   sysio::outbound_http::add_global_transport_program_options(cfg);
}

/// Parse alert-monitor arguments as a command line through add_component_options().
bpo::variables_map parse_options(const std::vector<std::string>& arguments) {
   bpo::options_description cfg;
   add_component_options(cfg);
   bpo::variables_map options;
   bpo::store(bpo::command_line_parser(arguments).options(cfg).run(), options);
   bpo::notify(options);
   return options;
}

/// Parse @p command_line, then @p config_ini as the text of a config.ini, into one variables_map the way appbase
/// does (application_base.cpp): the command line is stored first, then the file through bpo::parse_config_file with
/// unregistered options disallowed. A composing option therefore lists its command-line values before its config.ini
/// values, and config.ini's own rules apply to the file's values: a line is cut at its first `#`, and a value is
/// trimmed of spaces, tabs, carriage returns, and newlines at both ends.
bpo::variables_map parse_command_line_and_config_ini(const std::vector<std::string>& command_line,
                                                     const std::string& config_ini) {
   bpo::options_description cfg;
   add_component_options(cfg);
   bpo::variables_map options;
   bpo::store(bpo::command_line_parser(command_line).options(cfg).run(), options);
   std::istringstream config_file{config_ini};
   constexpr bool allow_unregistered = false;
   bpo::store(bpo::parse_config_file(config_file, cfg, allow_unregistered), options);
   bpo::notify(options);
   return options;
}

/// The command-line argument carrying one --alert-monitor-query @p spec.
std::string query_argument(std::string_view spec) {
   return fmt::format("--{}={}", alert_monitor::option::query, spec);
}

/// One config.ini line setting @p option to @p value.
std::string config_ini_line(std::string_view option, std::string_view value) {
   return fmt::format("{} = {}\n", option, value);
}

/// The --alert-monitor-query spec of a query named @p name, due every @p block_interval blocks, running @p query.
std::string spec_of(std::string_view name, uint32_t block_interval, std::string_view query) {
   return fmt::format("{},{},{}", name, block_interval, query);
}

/// The target, index, and template arguments every active configuration needs.
std::vector<std::string> required_arguments(const template_file& tpl) {
   return {std::string{target_argument}, std::string{index_argument}, tpl.argument()};
}

/// parse_config over @p arguments plus required_arguments(), with the template's own directory as the config
/// directory.
std::optional<alert_monitor::config> active_config(const template_file& tpl, std::vector<std::string> arguments = {}) {
   const std::vector<std::string> required = required_arguments(tpl);
   arguments.insert(arguments.end(), required.begin(), required.end());
   return alert_monitor::parse_config(parse_options(arguments), tpl.dir.path());
}

/// The detail string (what FC_LOG_AND_RETHROW and nodeop print) of the plugin_config_exception @p parse throws. Fails
/// the test when @p parse returns; any other exception propagates and fails it too.
template <typename Parse>
std::string config_error_of(Parse&& parse) {
   try {
      parse();
   } catch (const chain::plugin_config_exception& e) {
      return e.to_detail_string();
   }
   BOOST_FAIL("expected a plugin_config_exception");
   return {};
}

/// The epoch millis of reference_now, derived independently of the renderer's arithmetic.
int64_t reference_now_millis() {
   return int64_t{fc::time_point_sec{reference_now}.sec_since_epoch()} * millis_per_second +
          reference_now_sub_second_millis;
}

/// A get_info snapshot with distinctive values in the fields the assertions read.
chain_apis::get_info_db::get_info_results sample_info(uint32_t head_block_num = sample_head_block_num) {
   chain_apis::get_info_db::get_info_results info;
   info.server_version = "0badf00d";
   info.head_block_num = head_block_num;
   info.last_irreversible_block_num = head_block_num - 1;
   info.last_irreversible_block_id = chain::block_id_type{std::string{sample_lib_block_id}};
   info.head_block_producer = "sysio"_n;
   info.fork_db_head_block_num = head_block_num;
   info.total_cpu_weight = wide_weight;
   return info;
}

alert_monitor::status_snapshot sample_snapshot(uint32_t head_block_num = sample_head_block_num) {
   return {sample_info(head_block_num), reference_now};
}

/// A config for pipeline tests; the delivery settings are unused because the sender is a recorder.
alert_monitor::config pipeline_config(uint32_t max_items_per_task) {
   alert_monitor::config cfg;
   cfg.document_template = fc::json_template::parse(data_only_template);
   cfg.max_items_per_task = max_items_per_task;
   return cfg;
}

/// Three small newline-terminated documents for the body-assembly cases (assemble_bulk_bodies consumes them).
std::vector<std::string> sample_documents() {
   return {"{\"a\":1}\n", "{\"b\":2}\n", "{\"c\":3}\n"};
}

/// The get_info snapshot inside a rendered data-only document (`data.alert_monitor.status`).
fc::variant_object status_of(const std::string& document) {
   return fc::json::from_string(document)
      .get_object()["data"]
      .get_object()[std::string{alert_monitor::record_key}]
      .get_object()[std::string{alert_monitor::status_document::key::status}]
      .get_object();
}

/// Records every body a pipeline sends. Replies `indexed` unless `fail` is set; when `hold_first` is set, the
/// first send blocks until release() is called, so later documents accumulate in the queue.
struct recording_sender {
   std::mutex mtx;
   std::vector<alert_monitor::bulk_body> bodies; ///< guarded by mtx
   bool fail = false;
   bool hold_first = false;
   std::atomic<uint32_t> sends{0};
   std::atomic<bool> released{false};
   std::binary_semaphore first_send_started{0};
   std::binary_semaphore release_first_send{0};

   /// Unblock the held first send; safe to call more than once (a binary semaphore may be released only once).
   void release() {
      if (!released.exchange(true))
         release_first_send.release();
   }

   fc::network::es::es_bulk_result send(std::string body, uint32_t doc_count) {
      if (sends.fetch_add(1) == 0 && hold_first) {
         first_send_started.release();
         release_first_send.acquire();
      }
      {
         std::lock_guard<std::mutex> lk(mtx);
         bodies.push_back({std::move(body), doc_count});
      }
      fc::network::es::es_bulk_result result;
      result.attempts = 1;
      if (fail) {
         result.outcome = fc::network::es::es_bulk_result::status::unavailable;
         result.failed_docs = doc_count;
         result.detail = std::string{unavailable_detail};
      } else {
         result.outcome = fc::network::es::es_bulk_result::status::indexed;
         result.indexed_docs = doc_count;
      }
      return result;
   }

   alert_monitor::pipeline::sender as_sender() {
      return [this](std::string body, uint32_t doc_count) { return send(std::move(body), doc_count); };
   }
};

/// Records every call the pipeline's failure reporter makes, as the plugin's logging reporter would be called.
struct recording_failure_reporter {
   /// One reported batch: the client's result and the documents that batch carried.
   struct call {
      fc::network::es::es_bulk_result result;
      uint32_t doc_count = 0;
   };

   std::mutex mtx;
   std::vector<call> calls; ///< guarded by mtx

   /// A copy of what has been recorded so far.
   std::vector<call> recorded() {
      std::lock_guard<std::mutex> lk(mtx);
      return calls;
   }

   alert_monitor::pipeline::failure_reporter as_reporter() {
      return [this](const fc::network::es::es_bulk_result& result, uint32_t doc_count) {
         std::lock_guard<std::mutex> lk(mtx);
         calls.push_back({result, doc_count});
      };
   }
};

/// Poll @p source (a pipeline or a query_runner) until @p done(source.stats()) or drain_wait elapses; returns the
/// final stats.
template <typename Source, typename Predicate>
auto wait_for(const Source& source, Predicate done) {
   const auto deadline = std::chrono::steady_clock::now() + drain_wait;
   auto stats = source.stats();
   while (!done(stats) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(drain_poll);
      stats = source.stats();
   }
   return stats;
}

/// The `${data}` key of every document @p sender delivered, in delivery order (data-only template).
std::vector<std::string> delivered_data_keys(recording_sender& sender) {
   std::lock_guard<std::mutex> lk(sender.mtx);
   std::vector<std::string> keys;
   for (const auto& body : sender.bodies) {
      const auto lines = split_bulk_lines(body.body);
      // Action and document lines alternate; the documents are the odd lines.
      for (std::size_t l = 1; l < lines.size(); l += 2) {
         const fc::variant document = fc::json::from_string(lines[l]); // outlives the loop over its members
         for (const auto& member : document["data"].get_object())
            keys.push_back(member.key());
      }
   }
   return keys;
}

/// The LIBs of @p libs at which an entry with @p interval runs, admitting each due LIB the way query_runner::dispatch
/// does (the admitted bucket becomes lib / interval).
std::vector<chain::block_num_type> due_libs(uint32_t interval, std::span<const chain::block_num_type> libs) {
   std::vector<chain::block_num_type> due;
   chain::block_num_type admitted_bucket = 0;
   for (const auto lib : libs) {
      if (alert_monitor::is_due(lib, interval, admitted_bucket)) {
         admitted_bucket = lib / interval;
         due.push_back(lib);
      }
   }
   return due;
}

/// A trigger at LIB @p lib_num; the id and time are the sample values, which the runner only copies.
alert_monitor::query_trigger trigger_at(chain::block_num_type lib_num) {
   return {.lib_num = lib_num,
           .lib_id = chain::block_id_type{std::string{sample_lib_block_id}},
           .lib_time = reference_now};
}

/// @p count specs, each named `<repeated_entry_stem>_<index>`, due every block, running @p query.
std::vector<std::string> repeated_specs(std::string_view query, std::size_t count) {
   std::vector<std::string> specs;
   for (std::size_t i = 0; i < count; ++i)
      specs.push_back(spec_of(fmt::format("{}_{}", repeated_entry_stem, i), every_block, query));
   return specs;
}

/// The failure-count slot of @p kind in query_stats::failed.
std::size_t failure_slot(sysio::query_engine::error_kind kind) {
   return *magic_enum::enum_index(kind);
}

/// @p name as a query failure line quotes it (quoted_name_format), so a case finds the lines about one entry.
std::string quoted_name(std::string_view name) {
   return fmt::format(quoted_name_format, name);
}

/// The sum of every query_stats::failed slot.
uint64_t failed_by_any_kind(const alert_monitor::query_stats& stats) {
   uint64_t total = 0;
   for (const auto count : stats.failed)
      total += count;
   return total;
}

/// A spdlog sink that keeps the text of every message (its payload, unformatted), so a case can assert on what a
/// component logged.
class recording_log_sink final : public spdlog::sinks::base_sink<std::mutex> {
public:
   /// A copy of the messages logged so far.
   std::vector<std::string> messages() {
      std::lock_guard<std::mutex> lk(mutex_);
      return _messages;
   }

protected:
   /// Called under base_sink's mutex_.
   void sink_it_(const spdlog::details::log_msg& msg) override {
      _messages.emplace_back(msg.payload.data(), msg.payload.size());
   }
   /// Nothing is buffered.
   void flush_() override {}

private:
   std::vector<std::string> _messages; ///< guarded by base_sink's mutex_
};

/// An fc::logger whose lines land in a recording_log_sink instead of stderr. A component copies the logger it is
/// given; the copies share the sink.
struct captured_log {
   std::shared_ptr<recording_log_sink> sink = std::make_shared<recording_log_sink>();
   fc::logger log{std::string{captured_logger_name}};

   captured_log() { log.update_agent_logger(agent_logger()); }
   /// A new spdlog logger writing into sink: what this capture installs into every fc::logger it captures.
   std::unique_ptr<spdlog::logger> agent_logger() const {
      return std::make_unique<spdlog::logger>(std::string{captured_logger_name}, sink);
   }
   /// True when one logged line contains every one of @p fragments.
   bool has_line_with(std::initializer_list<std::string_view> fragments) {
      return std::ranges::any_of(sink->messages(), [&](const std::string& line) {
         return std::ranges::all_of(fragments, [&](std::string_view f) { return line.find(f) != std::string::npos; });
      });
   }
   /// True when no logged line contains @p fragment.
   bool never_logged(std::string_view fragment) {
      return std::ranges::none_of(sink->messages(),
                                  [&](const std::string& line) { return line.find(fragment) != std::string::npos; });
   }
};

/// Redirects the default fc::logger -- what ilog and friends write through -- into @p capture's sink for its lifetime,
/// at info level, then restores the default logger's own agent logger and level.
class default_log_redirect {
public:
   /// Install @p capture's sink as the default logger's agent logger.
   explicit default_log_redirect(captured_log& capture) {
      fc::logger& default_log = fc::logger::default_logger();
      default_log.update_agent_logger(capture.agent_logger());
      default_log.set_log_level(fc::log_level::info);
   }
   /// Restore the default logger's own agent logger and level.
   ~default_log_redirect() {
      fc::logger& default_log = fc::logger::default_logger();
      default_log.update_agent_logger(std::move(_previous_agent));
      default_log.set_log_level(_previous_level);
   }
   default_log_redirect(const default_log_redirect&) = delete;
   default_log_redirect& operator=(const default_log_redirect&) = delete;

private:
   /// The default logger's own agent logger, moved out before the redirect installs the capturing one.
   std::unique_ptr<spdlog::logger> _previous_agent = std::move(fc::logger::default_logger().get_agent_logger());
   /// The default logger's own level.
   fc::log_level _previous_level = fc::logger::default_logger().get_log_level();
};

/// A query_service scripted by query text (see scripted_query): every call records the deadline it was given, and a
/// blocked call waits for open_gate() or its deadline -- measured from the call, as the engine measures it -- then
/// throws the engine's QUERY_TIMEOUT, so a blocked caller always returns.
class scripted_query_service final : public sysio::query_engine::query_service {
public:
   sysio::query_engine::query_result
   execute(const std::string& query, const std::optional<sysio::query_engine::query_options>& options) override {
      using sysio::query_engine::error_kind;
      using sysio::query_engine::query_error;
      auto count_return = fc::make_scoped_exit([this] { ++_returned; });
      const std::optional<std::chrono::milliseconds> timeout = options ? options->timeout : std::nullopt;
      {
         std::lock_guard<std::mutex> lk(_mtx);
         _timeouts.push_back(timeout);
      }
      if (query == scripted_query::syntax_error)
         throw query_error(error_kind::QUERY_SYNTAX, std::string{scripted_failure_message});
      if (query == scripted_query::limit_error)
         throw query_error(error_kind::QUERY_LIMIT, std::string{scripted_failure_message}, std::nullopt,
                           std::string{scripted_limit});
      if (query == scripted_query::unexpected_error)
         throw std::runtime_error(std::string{scripted_failure_message});
      if (query == scripted_query::busy)
         throw query_error(error_kind::QUERY_BUSY, std::string{scripted_failure_message});
      if (query == scripted_query::busy_then_answer) {
         std::lock_guard<std::mutex> lk(_mtx);
         if (_busy_refusals < scripted_busy_refusals) {
            ++_busy_refusals;
            throw query_error(error_kind::QUERY_BUSY, std::string{scripted_failure_message});
         }
      }
      if (query == scripted_query::blocked) {
         // Absent options take the engine's own default deadline, as the engine does.
         wait_for_gate(timeout.value_or(sysio::query_engine::defaults::timeout));
      }
      return {};
   }

   /// Release every blocked call, now and later.
   void open_gate() {
      {
         std::lock_guard<std::mutex> lk(_mtx);
         _gate_open = true;
      }
      _cv.notify_all();
   }
   /// Wait (at most drain_wait) until @p callers calls are blocked at once.
   bool wait_until_blocked(std::size_t callers) {
      std::unique_lock<std::mutex> lk(_mtx);
      return _cv.wait_for(lk, drain_wait, [&] { return _blocked >= callers; });
   }
   /// The most calls that were ever blocked at once.
   std::size_t peak_blocked() {
      std::lock_guard<std::mutex> lk(_mtx);
      return _peak_blocked;
   }
   /// Calls blocked right now.
   std::size_t blocked() {
      std::lock_guard<std::mutex> lk(_mtx);
      return _blocked;
   }
   /// The deadline every call was given (options->timeout), in call order.
   std::vector<std::optional<std::chrono::milliseconds>> timeouts() {
      std::lock_guard<std::mutex> lk(_mtx);
      return _timeouts;
   }
   /// Calls that have returned or thrown.
   std::size_t returned() const { return _returned.load(); }

private:
   /// Block until open_gate() or @p timeout after now; throw the engine's QUERY_TIMEOUT at the deadline.
   void wait_for_gate(std::chrono::milliseconds timeout) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      std::unique_lock<std::mutex> lk(_mtx);
      ++_blocked;
      _peak_blocked = std::max(_peak_blocked, _blocked);
      _cv.notify_all();
      const bool opened = _cv.wait_until(lk, deadline, [this] { return _gate_open; });
      --_blocked;
      if (!opened)
         throw sysio::query_engine::query_error(sysio::query_engine::error_kind::QUERY_TIMEOUT,
                                                std::string{scripted_failure_message});
   }

   std::mutex _mtx;                                                 ///< guards every member below but _returned
   std::condition_variable _cv;                                     ///< gate, and blocked-count changes
   bool _gate_open = false;                                         ///< guarded by _mtx
   std::size_t _blocked = 0;                                        ///< guarded by _mtx
   std::size_t _peak_blocked = 0;                                   ///< guarded by _mtx
   std::size_t _busy_refusals = 0;                                  ///< guarded by _mtx
   std::vector<std::optional<std::chrono::milliseconds>> _timeouts; ///< guarded by _mtx
   std::atomic<std::size_t> _returned{0};                           ///< see returned()
};

/// One query_runner over a real pipeline (its sender a recorder) and a scripted query service, logging into a
/// captured log. A case adjusts cfg and the sender, then calls start(). Members are declared so that destruction
/// joins the runner before the pipeline it submits into, and the pipeline before the sender it calls; the
/// destructor first releases the gate and a held send, so a failed check never leaves a join waiting.
struct runner_harness {
   captured_log log;
   recording_sender sender;
   alert_monitor::config cfg = pipeline_config(small_batch);
   std::shared_ptr<scripted_query_service> service = std::make_shared<scripted_query_service>();
   std::optional<alert_monitor::pipeline> pipeline;
   std::optional<alert_monitor::query_runner> runner;

   /// One entry per spec, each parsed by parse_query_entry with its index as the ordinal.
   explicit runner_harness(const std::vector<std::string>& specs) {
      for (uint32_t ordinal = 0; ordinal < specs.size(); ++ordinal)
         cfg.queries.push_back(alert_monitor::parse_query_entry(specs[ordinal], ordinal));
   }
   ~runner_harness() {
      service->open_gate();
      sender.release();
   }
   /// Start the pipeline, then the runner over @p provider (by default, one handing out the scripted service).
   void start(alert_monitor::query_runner::service_provider provider = {}) {
      pipeline.emplace(cfg, std::string{action_line}, sender.as_sender());
      if (!provider)
         provider = [scripted = service] { return scripted; };
      runner.emplace(cfg, *pipeline, std::move(provider), log.log);
   }
};

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(alert_monitor_plugin_tests)

// --- options ---------------------------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(every_option_is_registered) try {
   bpo::options_description cfg;
   alert_monitor::add_options(cfg);
   std::set<std::string> names;
   for (const auto& option : cfg.options())
      names.insert(option->long_name());
   for (const auto name : every_alert_monitor_option) {
      BOOST_CHECK_MESSAGE(names.contains(std::string{name}), "missing option " + std::string{name});
   }
   // The template option's help text is generated from supplied_tokens, so it lists exactly the names
   // parse_config accepts -- and never the extra-fields token, which parse_config rejects.
   const auto* template_option = cfg.find_nothrow(alert_monitor::option::target_template_file, false);
   BOOST_REQUIRE(template_option != nullptr);
   const std::string& template_help = template_option->description();
   BOOST_CHECK(template_help.find(rejected_token) == std::string::npos);
   for (const auto name : alert_monitor::supplied_tokens) {
      BOOST_CHECK_MESSAGE(template_help.find(name) != std::string::npos,
                          "template help is missing supplied token " + std::string{name});
   }
   // A ceiling is formatted from the constant that enforces it, not spelled out.
   const auto* max_items_option = cfg.find_nothrow(alert_monitor::option::max_items_per_task, false);
   BOOST_REQUIRE(max_items_option != nullptr);
   BOOST_CHECK(max_items_option->description().find(std::to_string(alert_monitor::max_items_per_task_ceiling)) !=
               std::string::npos);
   // Each half of the auth pair names the other from its constant, so renaming one cannot leave the help stale.
   const auto* username_option = cfg.find_nothrow(alert_monitor::option::username, false);
   BOOST_REQUIRE(username_option != nullptr);
   BOOST_CHECK(username_option->description().find(fmt::format("--{}", alert_monitor::option::password)) !=
               std::string::npos);
}
FC_LOG_AND_RETHROW()

// No target URL: disabled, and the other options are not even validated (a config.ini can carry the knobs),
// except that alert-monitor-query requires it.
BOOST_AUTO_TEST_CASE(absent_target_url_disables_without_validating_the_rest) try {
   const fc::temp_directory config_dir;
   BOOST_CHECK(!alert_monitor::parse_config(parse_options({}), config_dir.path()).has_value());
   BOOST_CHECK(!alert_monitor::parse_config(parse_options({"--alert-monitor-max-items-per-task=0"}), config_dir.path())
                   .has_value());
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(defaults_apply_when_target_index_and_template_are_given) try {
   const template_file tpl{data_only_template};
   const auto cfg = active_config(tpl);
   BOOST_REQUIRE(cfg.has_value());
   BOOST_CHECK_EQUAL(cfg->delivery.url, "http://127.0.0.1:9");
   BOOST_CHECK_EQUAL(cfg->delivery.index, "test-status");
   BOOST_CHECK(!cfg->delivery.username.has_value());
   BOOST_CHECK_EQUAL(cfg->delivery.max_batch_bytes, alert_monitor::max_batch_bytes);
   BOOST_CHECK_EQUAL(cfg->delivery.max_doc_bytes, alert_monitor::max_doc_bytes);
   BOOST_CHECK_EQUAL(cfg->delivery.max_retries, fc::network::es::es_default_max_retries);
   BOOST_CHECK_EQUAL(cfg->delivery.retry_backoff.count(), fc::network::es::es_default_retry_backoff.count());
   BOOST_CHECK_EQUAL(cfg->delivery.connect_timeout.count(), fc::network::es::es_default_connect_timeout.count());
   BOOST_CHECK_EQUAL(cfg->delivery.request_timeout.count(), fc::network::es::es_default_request_timeout.count());
   BOOST_CHECK(cfg->document_template.contains(fc::json_template_default_token::data));
   BOOST_CHECK_EQUAL(cfg->max_items_per_task, alert_monitor::default_max_items_per_task);
   BOOST_CHECK_EQUAL(cfg->max_pending_documents, alert_monitor::default_max_pending_documents);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(explicit_values_override_the_defaults) try {
   const template_file tpl{data_only_template};
   const auto cfg =
      active_config(tpl, {"--alert-monitor-username=u", "--alert-monitor-password=p",
                          "--alert-monitor-max-items-per-task=7", "--alert-monitor-max-pending-documents=9",
                          "--alert-monitor-connect-timeout-ms=11", "--alert-monitor-request-timeout-ms=13",
                          "--alert-monitor-max-retries=0", "--alert-monitor-retry-backoff-ms=17"});
   BOOST_REQUIRE(cfg.has_value());
   BOOST_CHECK_EQUAL(cfg->delivery.username.value_or(""), "u");
   BOOST_CHECK_EQUAL(cfg->delivery.password.value_or(""), "p");
   BOOST_CHECK_EQUAL(cfg->max_items_per_task, 7u);
   BOOST_CHECK_EQUAL(cfg->max_pending_documents, 9u);
   BOOST_CHECK_EQUAL(cfg->delivery.connect_timeout.count(), 11);
   BOOST_CHECK_EQUAL(cfg->delivery.request_timeout.count(), 13);
   BOOST_CHECK_EQUAL(cfg->delivery.max_retries, 0u);
   BOOST_CHECK_EQUAL(cfg->delivery.retry_backoff.count(), 17);
}
FC_LOG_AND_RETHROW()

// Each alert-monitor-* transport option overrides its outbound-http-* counterpart, which applies when the
// plugin's own option is absent; with neither, the transport stays at its defaults.
BOOST_AUTO_TEST_CASE(transport_options_overlay_the_node_wide_values) try {
   const template_file tpl{data_only_template};
   const auto cfg = active_config(tpl, {std::string{global_proxy_argument}, std::string{global_ca_file_argument},
                                        std::string{alert_monitor_proxy_argument}});
   BOOST_REQUIRE(cfg.has_value());
   BOOST_REQUIRE(cfg->transport.proxy.has_value());
   BOOST_CHECK_EQUAL(*cfg->transport.proxy, alert_monitor_proxy);
   BOOST_REQUIRE(cfg->transport.additional_ca_file.has_value());
   BOOST_CHECK_EQUAL(cfg->transport.additional_ca_file->string(), global_ca_file);
   BOOST_CHECK(!cfg->transport.additional_ca_path.has_value());

   const auto defaults = active_config(tpl);
   BOOST_REQUIRE(defaults.has_value());
   BOOST_CHECK(!defaults->transport.proxy.has_value());
   BOOST_CHECK(!defaults->transport.additional_ca_file.has_value());
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(invalid_active_configurations_are_rejected) try {
   using chain::plugin_config_exception;
   const template_file tpl{data_only_template};
   const auto& config_dir = tpl.dir.path();
   // the index and the template are required once a target is set
   BOOST_CHECK_THROW(
      alert_monitor::parse_config(parse_options({std::string{target_argument}, tpl.argument()}), config_dir),
      plugin_config_exception);
   BOOST_CHECK_THROW(alert_monitor::parse_config(
                        parse_options({std::string{target_argument}, std::string{index_argument}}), config_dir),
                     plugin_config_exception);
   // The empty index rides in as a separate token: boost::program_options rejects the `--option=` spelling with
   // an empty value during parsing, so parse_config's own emptiness check is only reachable this way.
   BOOST_CHECK_THROW(alert_monitor::parse_config(parse_options({std::string{target_argument},
                                                                "--alert-monitor-target-index", "", tpl.argument()}),
                                                 config_dir),
                     plugin_config_exception);
   // a template that is missing, not JSON, not an object, or malformed
   BOOST_CHECK_THROW(
      alert_monitor::parse_config(
         parse_options({std::string{target_argument}, std::string{index_argument},
                        std::string{template_argument_prefix} + (tpl.dir.path() / "missing.json").string()}),
         config_dir),
      plugin_config_exception);
   BOOST_CHECK_THROW(active_config(template_file{"not json"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(template_file{"[1]"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(template_file{R"({"a":"${message"})"}), plugin_config_exception);
   // a template that references a name the plugin does not supply: any other name, or the logging sink's
   // extra-fields token (a status template writes its fixed members literally)
   BOOST_CHECK_THROW(active_config(template_file{R"({"a":"${nope}"})"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(template_file{fmt::format(R"({{"a":"${{{}}}"}})", rejected_token)}),
                     plugin_config_exception);
   // auth pairing, ranges, and the endpoint
   BOOST_CHECK_THROW(active_config(tpl, {"--alert-monitor-username=u"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--alert-monitor-password=p"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--alert-monitor-max-items-per-task=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--alert-monitor-max-pending-documents=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--alert-monitor-max-items-per-task=" +
                                         std::to_string(alert_monitor::max_items_per_task_ceiling + 1)}),
                     plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--alert-monitor-max-pending-documents=" +
                                         std::to_string(alert_monitor::max_pending_documents_ceiling + 1)}),
                     plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--alert-monitor-connect-timeout-ms=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--alert-monitor-request-timeout-ms=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--alert-monitor-retry-backoff-ms=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(
      active_config(tpl, {"--alert-monitor-max-retries=" + std::to_string(alert_monitor::max_retries_ceiling + 1)}),
      plugin_config_exception);
   BOOST_CHECK_THROW(alert_monitor::parse_config(parse_options({"--alert-monitor-target-url=ftp://127.0.0.1:9",
                                                                 std::string{index_argument}, tpl.argument()}),
                                                 config_dir),
                     plugin_config_exception);
   // An empty target URL rides in as a separate token for the same reason the empty index does; the plugin reports
   // the client's own "url is required" against the target option.
   BOOST_CHECK_THROW(alert_monitor::parse_config(parse_options({"--alert-monitor-target-url", "",
                                                                std::string{index_argument}, tpl.argument()}),
                                                 config_dir),
                     plugin_config_exception);
}
FC_LOG_AND_RETHROW()

// --- query options ---------------------------------------------------------------------------------------

// Every field lands in its entry exactly as given: the name, the interval (the whole uint32_t range), the query with
// its commas and spaces, and the ordinal the caller passes.
BOOST_AUTO_TEST_CASE(parse_query_entry_fills_every_field) try {
   const auto entry = alert_monitor::parse_query_entry(
      spec_of(full_query.name, full_query.block_interval, full_query.query), full_query_ordinal);
   BOOST_CHECK_EQUAL(entry.ordinal, full_query_ordinal);
   BOOST_CHECK_EQUAL(entry.name, full_query.name);
   BOOST_CHECK_EQUAL(entry.block_interval, full_query.block_interval);
   BOOST_CHECK_EQUAL(entry.query, full_query.query);
   BOOST_CHECK_EQUAL(alert_monitor::parse_query_entry(comma_query_spec, 0).query, comma_query);
   BOOST_CHECK_EQUAL(alert_monitor::parse_query_entry(max_interval_spec, 0).block_interval,
                     std::numeric_limits<uint32_t>::max());
   BOOST_CHECK_EQUAL(alert_monitor::parse_query_entry(padded_query_spec, 0).query, padded_query);
}
FC_LOG_AND_RETHROW()

// The option repeats on the command line and in config.ini: parse_config lists every value, the command line's first
// (appbase stores it first), each entry's ordinal its position in that list.
BOOST_AUTO_TEST_CASE(queries_repeat_on_the_command_line_and_in_config_ini) try {
   const template_file tpl{data_only_template};
   std::vector<std::string> command_line = required_arguments(tpl);
   std::string config_ini;
   for (std::size_t i = 0; i < command_line_then_config_ini_queries.size(); ++i) {
      const auto& q = command_line_then_config_ini_queries[i];
      const std::string spec = spec_of(q.name, q.block_interval, q.query);
      if (i < command_line_query_count)
         command_line.push_back(query_argument(spec));
      else
         config_ini += config_ini_line(alert_monitor::option::query, spec);
   }
   const auto cfg =
      alert_monitor::parse_config(parse_command_line_and_config_ini(command_line, config_ini), tpl.dir.path());
   BOOST_REQUIRE(cfg.has_value());
   BOOST_REQUIRE_EQUAL(cfg->queries.size(), command_line_then_config_ini_queries.size());
   for (uint32_t ordinal = 0; ordinal < cfg->queries.size(); ++ordinal) {
      const auto& entry = cfg->queries[ordinal];
      const auto& expected = command_line_then_config_ini_queries[ordinal];
      BOOST_TEST_CONTEXT("value " << ordinal) {
         BOOST_CHECK_EQUAL(entry.ordinal, ordinal);
         BOOST_CHECK_EQUAL(entry.name, expected.name);
         BOOST_CHECK_EQUAL(entry.block_interval, expected.block_interval);
         BOOST_CHECK_EQUAL(entry.query, expected.query);
      }
   }
}
FC_LOG_AND_RETHROW()

// Everything after the second comma is the query, commas included, from the command line and from config.ini alike.
BOOST_AUTO_TEST_CASE(a_query_keeps_its_commas) try {
   const template_file tpl{data_only_template};
   const auto from_command_line = active_config(tpl, {query_argument(comma_query_spec)});
   BOOST_REQUIRE(from_command_line.has_value());
   BOOST_REQUIRE_EQUAL(from_command_line->queries.size(), 1u);
   BOOST_CHECK_EQUAL(from_command_line->queries[0].query, comma_query);

   const auto from_config_ini = alert_monitor::parse_config(
      parse_command_line_and_config_ini(required_arguments(tpl),
                                        config_ini_line(alert_monitor::option::query, comma_query_spec)),
      tpl.dir.path());
   BOOST_REQUIRE(from_config_ini.has_value());
   BOOST_REQUIRE_EQUAL(from_config_ini->queries.size(), 1u);
   BOOST_CHECK_EQUAL(from_config_ini->queries[0].query, comma_query);
}
FC_LOG_AND_RETHROW()

// config.ini cuts a line at its first `#` and trims the value, so a trailing comment is not part of the query.
BOOST_AUTO_TEST_CASE(config_ini_cuts_a_query_at_its_comment) try {
   const template_file tpl{data_only_template};
   const auto cfg = alert_monitor::parse_config(
      parse_command_line_and_config_ini(required_arguments(tpl),
                                        config_ini_line(alert_monitor::option::query, commented_query_spec)),
      tpl.dir.path());
   BOOST_REQUIRE(cfg.has_value());
   BOOST_REQUIRE_EQUAL(cfg->queries.size(), 1u);
   BOOST_CHECK_EQUAL(cfg->queries[0].query, commented_query);
}
FC_LOG_AND_RETHROW()

// Each invalid spec is rejected by parse_query_entry itself: boost rejects an empty `--opt=` value on the command line
// before parse_config ever runs, so this is the only way every case reaches the parser.
BOOST_AUTO_TEST_CASE(invalid_query_specs_are_rejected) try {
   // Each category of invalid spec, paired with the part of the error that identifies it: the field count, or the
   // index and label of the failing field.
   const auto categories = std::to_array<std::pair<std::span<const std::string_view>, std::string_view>>({
      {too_few_fields_specs, field_count_error_marker},
      {bad_interval_specs, interval_field_error_marker},
      {bad_name_specs, name_field_error_marker},
      {blank_query_specs, query_field_error_marker},
   });
   for (const auto& [specs, marker] : categories) {
      for (const auto spec : specs) {
         BOOST_TEST_CONTEXT("spec '" << spec << "'") {
            const std::string detail = config_error_of([&] { alert_monitor::parse_query_entry(spec, 0); });
            BOOST_CHECK_MESSAGE(detail.find(marker) != std::string::npos, "expected '" << marker << "' in: " << detail);
         }
      }
   }
}
FC_LOG_AND_RETHROW()

// A name is a document's `${data}` key, so two values sharing one are rejected; the error names both values.
BOOST_AUTO_TEST_CASE(a_duplicate_query_name_is_rejected) try {
   const template_file tpl{data_only_template};
   const std::string detail = config_error_of(
      [&] { active_config(tpl, {query_argument(minimal_query_spec), query_argument(duplicate_name_spec)}); });
   BOOST_CHECK(detail.find("value 1") != std::string::npos);
   BOOST_CHECK(detail.find("value 0") != std::string::npos);
}
FC_LOG_AND_RETHROW()

// Once the name is valid, the interval error, the query error, and the duplicate-name error carry it, so the operator
// can find the entry.
BOOST_AUTO_TEST_CASE(a_valid_query_name_is_echoed_in_its_errors) try {
   const template_file tpl{data_only_template};
   const std::string interval_error =
      config_error_of([] { alert_monitor::parse_query_entry(echoed_name_bad_interval_spec, 0); });
   const std::string blank_query_error =
      config_error_of([] { alert_monitor::parse_query_entry(echoed_name_blank_query_spec, 0); });
   const std::string duplicate_error = config_error_of([&] {
      active_config(tpl, {query_argument(spec_of(echoed_name, every_block, comma_query)),
                          query_argument(spec_of(echoed_name, every_ten_blocks, comma_query))});
   });
   for (const auto& detail : {interval_error, blank_query_error, duplicate_error}) {
      BOOST_TEST_CONTEXT(detail) {
         BOOST_CHECK(detail.find(echoed_name) != std::string::npos);
         BOOST_CHECK(detail.find(alert_monitor::option::query) != std::string::npos);
      }
   }
   // The duplicate-name error names the name only: the query text it carries is never reproduced.
   BOOST_CHECK(duplicate_error.find(query_text_marker) == std::string::npos);
}
FC_LOG_AND_RETHROW()

// A missing field shifts query text into the name and interval positions; no error reproduces any of it, neither from
// parse_query_entry nor through parse_config (which adds no wrapper of its own that could re-append the spec).
BOOST_AUTO_TEST_CASE(query_errors_never_echo_query_text) try {
   const template_file tpl{data_only_template};
   for (const auto spec : query_text_in_other_fields_specs) {
      BOOST_TEST_CONTEXT("spec '" << spec << "'") {
         const std::string direct = config_error_of([&] { alert_monitor::parse_query_entry(spec, 0); });
         BOOST_CHECK(direct.find(query_text_marker) == std::string::npos);
         const std::string configured = config_error_of([&] { active_config(tpl, {query_argument(spec)}); });
         BOOST_CHECK(configured.find(query_text_marker) == std::string::npos);
      }
   }
}
FC_LOG_AND_RETHROW()

// nodeop's "Non-default options" line masks every --alert-monitor-query value whole, as it masks the password: each
// is logged as ***, so neither a query's text nor its name appears -- unlike the signature-provider mask, which keeps
// all but the last comma field and would print `n,1,SELECT a,***`. Keyed by alert_monitor::option::query, so renaming
// the option without the mask list fails here instead of leaking query text.
BOOST_AUTO_TEST_CASE(the_non_default_options_line_masks_every_query) try {
   bpo::options_description cfg;
   add_component_options(cfg);
   const std::vector<std::string> arguments{
      query_argument(spec_of(full_query.name, full_query.block_interval, full_query.query)),
      query_argument(comma_query_spec)};
   const bpo::parsed_options parsed = bpo::command_line_parser(arguments).options(cfg).run();
   captured_log log;
   {
      const default_log_redirect redirect{log};
      chain::detail::log_non_default_options(parsed.options);
   }
   const std::string masked = fmt::format("{} = {}", alert_monitor::option::query, masked_option_value);
   BOOST_CHECK(log.has_line_with({fmt::format("{0}, {0}", masked)})); // one masked entry per value
   BOOST_CHECK(log.never_logged(query_text_marker));
   BOOST_CHECK(log.never_logged(full_query.name));
}
FC_LOG_AND_RETHROW()

// A query without a target URL fails instead of being dropped along with the disabled plugin.
BOOST_AUTO_TEST_CASE(a_query_requires_the_target_url) try {
   const fc::temp_directory config_dir;
   const std::string detail = config_error_of(
      [&] { alert_monitor::parse_config(parse_options({query_argument(minimal_query_spec)}), config_dir.path()); });
   BOOST_CHECK(detail.find(alert_monitor::option::query) != std::string::npos);
   BOOST_CHECK(detail.find(alert_monitor::option::target_url) != std::string::npos);
}
FC_LOG_AND_RETHROW()

// A relative template path is resolved against the config directory parse_config is given -- never the working
// directory -- and an absolute one is used as is.
BOOST_AUTO_TEST_CASE(a_relative_template_path_resolves_against_the_config_dir) try {
   using chain::plugin_config_exception;
   const template_file tpl{data_only_template};
   const std::vector<std::string> relative{std::string{target_argument}, std::string{index_argument},
                                           std::string{template_argument_prefix} + tpl.path.filename().string()};
   const auto cfg = alert_monitor::parse_config(parse_options(relative), tpl.dir.path());
   BOOST_REQUIRE(cfg.has_value());
   BOOST_CHECK(cfg->document_template.contains(fc::json_template_default_token::data));

   // From any other directory the same relative path names a file that is not there, and the error names the path
   // it resolved to.
   const fc::temp_directory elsewhere;
   const std::string resolved = (elsewhere.path() / tpl.path.filename()).string();
   const std::string detail =
      config_error_of([&] { alert_monitor::parse_config(parse_options(relative), elsewhere.path()); });
   BOOST_CHECK_MESSAGE(detail.find(resolved) != std::string::npos, "error does not name " + resolved + ": " + detail);

   const auto absolute = alert_monitor::parse_config(parse_options(required_arguments(tpl)), elsewhere.path());
   BOOST_CHECK(absolute.has_value());
}
FC_LOG_AND_RETHROW()

// With queries configured the template must reference ${data}, where a query's result is rendered; without queries
// it need not.
BOOST_AUTO_TEST_CASE(a_query_needs_a_template_that_references_data) try {
   const template_file tpl{no_data_template};
   BOOST_CHECK(active_config(tpl).has_value());
   const std::string error = config_error_of([&] { active_config(tpl, {query_argument(minimal_query_spec)}); });
   BOOST_CHECK_MESSAGE(error.find("${data}") != std::string::npos, error);
   BOOST_CHECK_MESSAGE(error.find(alert_monitor::option::query) != std::string::npos, error);
}
FC_LOG_AND_RETHROW()

// Valid queries pass the startup parse, and a configuration without queries has nothing to parse.
BOOST_AUTO_TEST_CASE(validate_queries_accepts_queries_that_parse) try {
   const template_file tpl{data_only_template};
   const auto cfg =
      active_config(tpl, {query_argument(spec_of(full_query.name, full_query.block_interval, full_query.query)),
                          query_argument(spec_of(end_to_end_query_name, end_to_end_block_interval, end_to_end_query))});
   BOOST_REQUIRE(cfg.has_value());
   BOOST_CHECK_NO_THROW(alert_monitor::validate_queries(cfg->queries, parse_options({})));
   BOOST_CHECK_NO_THROW(alert_monitor::validate_queries({}, parse_options({})));
}
FC_LOG_AND_RETHROW()

// A query the engine's parser rejects fails startup, naming the entry, the error kind, and the line and column --
// never the query text.
BOOST_AUTO_TEST_CASE(a_query_that_does_not_parse_fails_startup) try {
   using sysio::query_engine::error_kind;
   constexpr std::string_view broken_name = "broken_entry";
   const template_file tpl{data_only_template};
   const auto cfg = active_config(tpl, {query_argument(spec_of(broken_name, every_block, unparseable_query))});
   BOOST_REQUIRE(cfg.has_value());
   const std::string error =
      config_error_of([&] { alert_monitor::validate_queries(cfg->queries, parse_options({})); });
   BOOST_CHECK_MESSAGE(error.find(quoted_name(broken_name)) != std::string::npos, error);
   BOOST_CHECK_MESSAGE(error.find(magic_enum::enum_name(error_kind::QUERY_SYNTAX)) != std::string::npos, error);
   BOOST_CHECK_MESSAGE(error.find(syntax_error_location_marker) != std::string::npos, error);
   BOOST_CHECK_MESSAGE(error.find(query_text_marker) == std::string::npos, error);
}
FC_LOG_AND_RETHROW()

// config.ini cuts a query at its first `#`, which WireQL allows only inside a quoted literal, so the cut leaves the
// literal unclosed and the startup parse rejects it. The same spec on the command line is whole and passes.
BOOST_AUTO_TEST_CASE(a_query_cut_at_a_config_ini_comment_fails_startup) try {
   using sysio::query_engine::error_kind;
   const template_file tpl{data_only_template};
   const auto options = parse_command_line_and_config_ini(
      required_arguments(tpl), config_ini_line(alert_monitor::option::query, hash_in_literal_spec));
   const auto cut = alert_monitor::parse_config(options, tpl.dir.path());
   BOOST_REQUIRE(cut.has_value());
   const std::string error = config_error_of([&] { alert_monitor::validate_queries(cut->queries, options); });
   BOOST_CHECK_MESSAGE(error.find(magic_enum::enum_name(error_kind::QUERY_SYNTAX)) != std::string::npos, error);

   const auto whole = active_config(tpl, {query_argument(hash_in_literal_spec)});
   BOOST_REQUIRE(whole.has_value());
   BOOST_CHECK_NO_THROW(alert_monitor::validate_queries(whole->queries, parse_options({})));
}
FC_LOG_AND_RETHROW()

// The startup parse runs under the engine's own configured limits, and an engine configuration the engine rejects
// fails it too.
BOOST_AUTO_TEST_CASE(validate_queries_applies_the_engine_configuration) try {
   using sysio::query_engine::error_kind;
   const template_file tpl{data_only_template};
   const auto engine_options = [&](std::string_view engine_argument) {
      bpo::options_description cfg;
      add_component_options(cfg);
      sysio::query_engine::add_options(cfg);
      std::vector<std::string> arguments = required_arguments(tpl);
      arguments.push_back(query_argument(spec_of(full_query.name, full_query.block_interval, full_query.query)));
      arguments.emplace_back(engine_argument);
      bpo::variables_map options;
      bpo::store(bpo::command_line_parser(arguments).options(cfg).run(), options);
      bpo::notify(options);
      return options;
   };

   const auto small = engine_options(tiny_query_bytes_argument);
   const auto cfg = alert_monitor::parse_config(small, tpl.dir.path());
   BOOST_REQUIRE(cfg.has_value());
   const std::string limit_error = config_error_of([&] { alert_monitor::validate_queries(cfg->queries, small); });
   BOOST_CHECK_MESSAGE(limit_error.find(magic_enum::enum_name(error_kind::QUERY_LIMIT)) != std::string::npos,
                       limit_error);
   BOOST_CHECK_MESSAGE(limit_error.find(sysio::query_engine::option::max_query_bytes) != std::string::npos,
                       limit_error);

   const auto invalid = engine_options(zero_in_flight_argument);
   const std::string config_error = config_error_of([&] { alert_monitor::validate_queries(cfg->queries, invalid); });
   BOOST_CHECK_MESSAGE(config_error.find(magic_enum::enum_name(error_kind::INVALID_PARAMS)) != std::string::npos,
                       config_error);
}
FC_LOG_AND_RETHROW()

// --- documents -------------------------------------------------------------------------------------------

// The shipped sample compiles, and every token in it is replaced: the timestamp is a bare number, the level
// is INFO, message/category are the plugin's names, the location points at the renderer, data.alert_monitor carries
// the status record and version with the snapshot at data.alert_monitor.status, and the literal identity members
// come through as written.
BOOST_AUTO_TEST_CASE(render_document_replaces_every_token_of_the_shipped_sample) try {
   const auto tpl = fc::json_template::from_file(std::filesystem::path{ALERT_MONITOR_SAMPLE_TEMPLATE});
   const auto line = alert_monitor::render_document(sample_snapshot(), tpl);
   BOOST_REQUIRE(!line.empty());
   BOOST_CHECK_EQUAL(line.back(), '\n');
   BOOST_CHECK_EQUAL(line.find('\n'), line.size() - 1);
   BOOST_CHECK(line.find(R"("@timestamp":)" + std::to_string(reference_now_millis()) + ",") != std::string::npos);
   const auto doc = fc::json::from_string(line).get_object();
   BOOST_CHECK_EQUAL(doc["@timestamp"].as_int64(), reference_now_millis());
   BOOST_CHECK_EQUAL(doc["level"].as_string(), "INFO");
   BOOST_CHECK_EQUAL(doc["message"].as_string(), std::string{alert_monitor::record_message});
   BOOST_CHECK_EQUAL(doc["category"].as_string(), std::string{alert_monitor::logger_name});
   const auto source_location = doc["sourceLocation"].as_string();
   BOOST_CHECK(source_location.find("alert_monitor.cpp:") != std::string::npos);
   BOOST_CHECK(source_location.ends_with(" render_document"));
   const auto& status_object = doc["data"].get_object()[std::string{alert_monitor::record_key}].get_object();
   BOOST_CHECK_EQUAL(status_object.size(), 3u); // record, version, status
   BOOST_CHECK_EQUAL(status_object[alert_monitor::status_document::key::record].as_string(),
                     alert_monitor::status_document::record);
   BOOST_CHECK_EQUAL(status_object[alert_monitor::status_document::key::version].as_uint64(),
                     alert_monitor::status_document::version);
   const auto& status = status_object[alert_monitor::status_document::key::status].get_object();
   BOOST_CHECK_EQUAL(status["head_block_num"].as_uint64(), sample_head_block_num);
   BOOST_CHECK_EQUAL(status["last_irreversible_block_num"].as_uint64(), sample_head_block_num - 1);
   BOOST_CHECK_EQUAL(status["server_version"].as_string(), "0badf00d");
   BOOST_CHECK_EQUAL(status["head_block_producer"].as_string(), "sysio");
   BOOST_CHECK(status["total_cpu_weight"].is_uint64());
   BOOST_CHECK_EQUAL(status["total_cpu_weight"].as_uint64(), wide_weight);
   BOOST_CHECK_MESSAGE(status.size() == expected_get_info_fields.size(),
                       "get_info_results gained or lost a reflected field (fc omits unset optionals): update "
                       "expected_get_info_fields");
   for (const auto field : expected_get_info_fields) {
      BOOST_CHECK_MESSAGE(status.contains(std::string{field}), "missing get_info field " + std::string{field});
   }
   BOOST_CHECK_EQUAL(doc["env"].as_string(), "local");
   BOOST_CHECK_EQUAL(doc["app"].as_string(), "nodeop");
   BOOST_CHECK_EQUAL(doc["principal"].as_string(), "node-a");
   BOOST_CHECK_EQUAL(doc["logStream"].as_string(), "nodeop/node-a");
}
FC_LOG_AND_RETHROW()

// supplied_tokens is the contract parse_config enforces, so a template that uses every entry of it must render,
// with the level the plugin fixes.
BOOST_AUTO_TEST_CASE(every_supplied_token_renders) try {
   std::string text = "{";
   for (const auto name : alert_monitor::supplied_tokens)
      text += fmt::format(R"("{0}":"${{{0}}}",)", name);
   text.back() = '}';
   const auto line = alert_monitor::render_document(sample_snapshot(), fc::json_template::parse(text));
   const auto doc = fc::json::from_string(line).get_object();
   BOOST_CHECK_EQUAL(doc.size(), alert_monitor::supplied_tokens.size());
   BOOST_CHECK_EQUAL(doc["level"].as_string(), std::string{alert_monitor::record_level});
   BOOST_CHECK_EQUAL(doc["epoch_millis"].as_int64(), reference_now_millis());
   BOOST_CHECK_EQUAL(doc["data"].get_object().size(), 1u);
}
FC_LOG_AND_RETHROW()

// Byte parity with /v1/chain/get_info: the snapshot is embedded as the text the HTTP layer sends for the same
// struct (beast_http_session: fc::json::to_string(fc::variant(result), fc::time_point::maximum())), except that an
// integer the HTTP body quotes is a bare number -- and the document is exactly what the template says, nothing added.
BOOST_AUTO_TEST_CASE(render_document_embeds_the_http_body_and_adds_nothing) try {
   const auto snapshot = sample_snapshot();
   std::string http_body = fc::json::to_string(fc::variant{snapshot.info}, fc::time_point::maximum());
   const std::string wide_digits = std::to_string(wide_weight);
   const std::string quoted_wide = '"' + wide_digits + '"';
   const auto quoted_at = http_body.find(quoted_wide);
   BOOST_REQUIRE(quoted_at != std::string::npos);
   http_body.replace(quoted_at, quoted_wide.size(), wide_digits);
   const auto data_only = fc::json_template::parse(data_only_template);
   const std::string line = alert_monitor::render_document(snapshot, data_only);
   BOOST_CHECK_EQUAL(line, fmt::format(R"({{"data":{{"{}":{{"{}":"{}","{}":{},"{}":{}}}}}}})"
                                       "\n",
                                       alert_monitor::record_key, alert_monitor::status_document::key::record,
                                       alert_monitor::status_document::record,
                                       alert_monitor::status_document::key::version,
                                       alert_monitor::status_document::version,
                                       alert_monitor::status_document::key::status, http_body));
   const std::string literal_only = alert_monitor::render_document(snapshot, fc::json_template::parse(R"({"x":"y"})"));
   BOOST_CHECK_EQUAL(literal_only, R"({"x":"y"})"
                                   "\n");
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(assemble_bulk_bodies_pairs_each_document_with_the_action_line_and_consumes_them) try {
   auto documents = sample_documents();
   const auto bodies = alert_monitor::assemble_bulk_bodies(documents, action_line, alert_monitor::max_batch_bytes);
   BOOST_REQUIRE_EQUAL(bodies.size(), 1u);
   BOOST_CHECK_EQUAL(bodies[0].doc_count, 3u);
   const auto lines = split_bulk_lines(bodies[0].body);
   BOOST_REQUIRE_EQUAL(lines.size(), 6u);
   BOOST_CHECK_EQUAL(lines[0] + "\n", std::string{action_line});
   BOOST_CHECK_EQUAL(lines[1], "{\"a\":1}");
   BOOST_CHECK_EQUAL(lines[4] + "\n", std::string{action_line});
   BOOST_CHECK_EQUAL(lines[5], "{\"c\":3}");
   BOOST_CHECK_EQUAL(bodies[0].body.back(), '\n');
   for (const auto& document : documents)
      BOOST_CHECK(document.empty()); // consumed
   BOOST_CHECK(alert_monitor::assemble_bulk_bodies({}, action_line, alert_monitor::max_batch_bytes).empty());
}
FC_LOG_AND_RETHROW()

// A cap that fits exactly one pair splits three documents into three bodies; every body stays within the cap
// except by the one pair es_sink also allows.
BOOST_AUTO_TEST_CASE(assemble_bulk_bodies_splits_at_the_body_byte_cap) try {
   auto documents = sample_documents();
   const std::size_t one_pair = action_line.size() + documents[0].size();
   const auto bodies = alert_monitor::assemble_bulk_bodies(documents, action_line, one_pair);
   BOOST_REQUIRE_EQUAL(bodies.size(), 3u);
   for (const auto& body : bodies) {
      BOOST_CHECK_EQUAL(body.doc_count, 1u);
      BOOST_CHECK_EQUAL(body.body.size(), one_pair);
   }
   auto more = sample_documents(); // the first call consumed `documents`
   const auto two_per_body = alert_monitor::assemble_bulk_bodies(more, action_line, 2 * one_pair);
   BOOST_REQUIRE_EQUAL(two_per_body.size(), 2u);
   BOOST_CHECK_EQUAL(two_per_body[0].doc_count, 2u);
   BOOST_CHECK_EQUAL(two_per_body[1].doc_count, 1u);

   // A cap below even one pair loses nothing: the close only fires once a body already holds a pair, so every
   // document still ships, alone in a body of its own and in order.
   const auto expected = sample_documents();
   auto below_one_pair = sample_documents();
   const auto singles = alert_monitor::assemble_bulk_bodies(below_one_pair, action_line, one_pair - 1);
   BOOST_REQUIRE_EQUAL(singles.size(), expected.size());
   for (std::size_t i = 0; i < singles.size(); ++i) {
      BOOST_CHECK_EQUAL(singles[i].doc_count, 1u);
      BOOST_CHECK_EQUAL(singles[i].body, std::string{action_line} + expected[i]);
   }
}
FC_LOG_AND_RETHROW()

// --- pipeline --------------------------------------------------------------------------------------------

// The first send is held while the remaining documents render and queue behind it; once released, the delivery
// worker drains them in bodies of exactly max_items_per_task (plus a remainder), in order. The first body's
// size depends on how many documents the render worker managed to queue before the delivery worker woke
// (1 to max_items_per_task), so only the bodies after it are pinned exactly.
BOOST_AUTO_TEST_CASE(pipeline_batches_queued_documents_up_to_max_items_per_task) try {
   recording_sender sender;
   sender.hold_first = true;
   alert_monitor::pipeline pipeline{pipeline_config(small_batch), std::string{action_line}, sender.as_sender()};
   // Declared after the pipeline, so it runs before the pipeline's destructor joins: a failed REQUIRE never
   // leaves the held send blocking that join.
   auto release_on_exit = fc::make_scoped_exit([&] { sender.release(); });
   for (uint64_t i = 0; i < snapshot_count; ++i)
      BOOST_REQUIRE(pipeline.submit(sample_snapshot(static_cast<uint32_t>(i))));
   BOOST_REQUIRE(sender.first_send_started.try_acquire_for(drain_wait));
   const auto queued = wait_for(pipeline, [](const auto& s) { return s.documents_queued == snapshot_count; });
   BOOST_REQUIRE_EQUAL(queued.documents_queued, snapshot_count);
   sender.release();
   const auto done = wait_for(pipeline, [](const auto& s) { return s.documents_indexed == snapshot_count; });
   pipeline.shutdown();
   BOOST_CHECK_EQUAL(done.documents_indexed, snapshot_count);
   BOOST_CHECK_EQUAL(done.batches_failed, 0u);
   BOOST_CHECK_EQUAL(done.snapshots_submitted, snapshot_count);
   BOOST_CHECK_EQUAL(done.documents_dropped_queue_full, 0u);

   std::lock_guard<std::mutex> lk(sender.mtx);
   BOOST_REQUIRE(!sender.bodies.empty());
   const uint32_t first = sender.bodies.front().doc_count;
   BOOST_CHECK(first >= 1 && first <= small_batch);
   uint64_t remaining = snapshot_count - first;
   for (std::size_t i = 1; i < sender.bodies.size(); ++i) {
      const auto expected = static_cast<uint32_t>(std::min<uint64_t>(small_batch, remaining));
      BOOST_CHECK_EQUAL(sender.bodies[i].doc_count, expected);
      remaining -= expected;
   }
   BOOST_CHECK_EQUAL(remaining, 0u);
   BOOST_CHECK(sender.bodies.size() >= 3u); // ten documents in bodies of at most four
   uint32_t next_head = 0;
   for (const auto& body : sender.bodies) {
      const auto lines = split_bulk_lines(body.body);
      BOOST_REQUIRE_EQUAL(lines.size(), 2u * body.doc_count);
      for (std::size_t l = 0; l < lines.size(); l += 2) {
         BOOST_CHECK_EQUAL(lines[l] + "\n", std::string{action_line});
         BOOST_CHECK_EQUAL(status_of(lines[l + 1])["head_block_num"].as_uint64(), next_head++);
      }
   }
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(pipeline_counts_failures_and_keeps_the_last_detail) try {
   recording_sender sender;
   sender.fail = true;
   alert_monitor::pipeline pipeline{pipeline_config(small_batch), std::string{action_line}, sender.as_sender()};
   for (uint64_t i = 0; i < failing_count; ++i)
      BOOST_REQUIRE(pipeline.submit(sample_snapshot(static_cast<uint32_t>(i))));
   // Read after shutdown()'s join, once both workers are done.
   wait_for(pipeline, [](const auto& s) { return s.documents_failed == failing_count; });
   pipeline.shutdown();
   const auto done = pipeline.stats();
   BOOST_CHECK_EQUAL(done.documents_failed, failing_count);
   BOOST_CHECK_EQUAL(done.documents_indexed, 0u);
   BOOST_CHECK_EQUAL(done.batches_indexed, 0u);
   BOOST_CHECK(done.batches_failed >= 1u);
   BOOST_CHECK_EQUAL(pipeline.last_failure(), std::string{unavailable_detail});
   pipeline.shutdown(); // idempotent
}
FC_LOG_AND_RETHROW()

// The reporter is how a failed batch reaches the log: the plugin's is a warning line, so it must fire exactly
// once per failed batch, carrying that batch's own documents and detail -- and never for an acknowledged one.
BOOST_AUTO_TEST_CASE(the_failure_reporter_sees_every_failed_batch_and_no_indexed_batch) try {
   recording_failure_reporter reporter; // declared before the pipeline: it must outlive the destructor's join
   recording_sender sender;
   sender.fail = true;
   {
      alert_monitor::pipeline pipeline{pipeline_config(small_batch), std::string{action_line}, sender.as_sender(),
                                        reporter.as_reporter()};
      for (uint64_t i = 0; i < failing_count; ++i)
         BOOST_REQUIRE(pipeline.submit(sample_snapshot(static_cast<uint32_t>(i))));
      wait_for(pipeline, [](const auto& s) { return s.documents_failed == failing_count; });
      pipeline.shutdown(); // joins both workers: every report the run will make has been made
      const auto done = pipeline.stats();
      const auto reported = reporter.recorded();
      BOOST_CHECK_EQUAL(reported.size(), done.batches_failed);
      uint64_t reported_documents = 0;
      for (const auto& call : reported) {
         BOOST_CHECK(call.result.outcome == fc::network::es::es_bulk_result::status::unavailable);
         BOOST_CHECK_EQUAL(call.result.detail, std::string{unavailable_detail});
         BOOST_CHECK(call.doc_count >= 1u);
         reported_documents += call.doc_count;
      }
      // Every document the failing pipeline swallowed was named in a report.
      BOOST_CHECK_EQUAL(reported_documents, failing_count);
   }

   recording_failure_reporter quiet_reporter;
   recording_sender indexing_sender;
   alert_monitor::pipeline indexing_pipeline{pipeline_config(small_batch), std::string{action_line},
                                              indexing_sender.as_sender(), quiet_reporter.as_reporter()};
   for (uint64_t i = 0; i < failing_count; ++i)
      BOOST_REQUIRE(indexing_pipeline.submit(sample_snapshot(static_cast<uint32_t>(i))));
   wait_for(indexing_pipeline, [](const auto& s) { return s.documents_indexed == failing_count; });
   indexing_pipeline.shutdown();
   BOOST_CHECK_EQUAL(indexing_pipeline.stats().documents_indexed, failing_count);
   BOOST_CHECK(quiet_reporter.recorded().empty());
}
FC_LOG_AND_RETHROW()

// A delivery queue one document deep: the held first send keeps the delivery worker inside the sender, so only
// one rendered document waits behind it and every later one is dropped and counted. snapshots_dropped_queue_full
// is not asserted -- the render queue's depth is a fixed constant larger than snapshot_count and the render
// worker never blocks, so a render-queue overflow cannot be forced deterministically here.
BOOST_AUTO_TEST_CASE(queue_full_drops_are_counted) try {
   recording_sender sender; // declared before the pipeline: it must outlive the destructor's join
   sender.hold_first = true;
   auto cfg = pipeline_config(small_batch);
   cfg.max_pending_documents = single_pending_document;
   alert_monitor::pipeline pipeline{cfg, std::string{action_line}, sender.as_sender()};
   // Declared after the pipeline, so it runs before the pipeline's destructor joins: a failed REQUIRE never
   // leaves the held send blocking that join.
   auto release_on_exit = fc::make_scoped_exit([&] { sender.release(); });
   for (uint64_t i = 0; i < snapshot_count; ++i)
      BOOST_REQUIRE(pipeline.submit(sample_snapshot(static_cast<uint32_t>(i))));
   BOOST_REQUIRE(sender.first_send_started.try_acquire_for(drain_wait));
   wait_for(pipeline, [](const auto& s) { return s.documents_dropped_queue_full > 0; });
   sender.release();
   pipeline.shutdown();
   const auto done = pipeline.stats();
   BOOST_CHECK_EQUAL(done.snapshots_submitted, snapshot_count);
   BOOST_CHECK(done.documents_dropped_queue_full >= 1u);
   BOOST_CHECK_EQUAL(done.documents_dropped_oversize, 0u);
   BOOST_CHECK_EQUAL(done.render_failures, 0u);
}
FC_LOG_AND_RETHROW()

// A single-document cap below any rendered document: the document is dropped and counted before the delivery
// queue, and the sender is never called.
BOOST_AUTO_TEST_CASE(oversize_documents_are_dropped) try {
   recording_sender sender; // declared before the pipeline: it must outlive the destructor's join
   auto cfg = pipeline_config(small_batch);
   cfg.delivery.max_doc_bytes = tiny_doc_byte_cap;
   alert_monitor::pipeline pipeline{cfg, std::string{action_line}, sender.as_sender()};
   BOOST_REQUIRE(pipeline.submit(sample_snapshot()));
   wait_for(pipeline, [](const auto& s) { return s.documents_dropped_oversize == 1; });
   pipeline.shutdown();
   const auto done = pipeline.stats();
   BOOST_CHECK_EQUAL(done.documents_dropped_oversize, 1u);
   BOOST_CHECK_EQUAL(done.documents_queued, 0u);
   BOOST_CHECK_EQUAL(done.documents_dropped_queue_full, 0u);
   BOOST_CHECK_EQUAL(done.render_failures, 0u);
   BOOST_CHECK_EQUAL(sender.sends.load(), 0u);
   std::lock_guard<std::mutex> lk(sender.mtx);
   BOOST_CHECK(sender.bodies.empty());
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(classify_progress_reports_failure_streaks_and_recovery) try {
   using alert_monitor::classify_progress;
   using alert_monitor::progress;
   alert_monitor::pipeline_stats previous;
   auto current = previous;
   BOOST_CHECK(classify_progress(previous, current, false) == progress::steady);
   current.batches_indexed = 1;
   BOOST_CHECK(classify_progress(previous, current, false) == progress::steady);
   BOOST_CHECK(classify_progress(previous, current, true) == progress::recovered);
   current.batches_failed = 1;
   BOOST_CHECK(classify_progress(previous, current, false) == progress::failing);
   BOOST_CHECK(classify_progress(previous, current, true) == progress::failing);
   previous = current;
   current.snapshots_dropped_queue_full = 1;
   BOOST_CHECK(classify_progress(previous, current, false) == progress::failing);
   previous = current;
   current.documents_dropped_oversize = 1;
   BOOST_CHECK(classify_progress(previous, current, false) == progress::failing);
   previous = current;
   current.documents_dropped_queue_full = 1;
   BOOST_CHECK(classify_progress(previous, current, false) == progress::failing);
   previous = current;
   current.render_failures = 1;
   BOOST_CHECK(classify_progress(previous, current, false) == progress::failing);
   previous = current;
   BOOST_CHECK(classify_progress(previous, current, true) == progress::steady); // failing, nothing new
}
FC_LOG_AND_RETHROW()

// submit_document reports what became of each query document and counts it apart from the status documents.
BOOST_AUTO_TEST_CASE(submit_document_reports_each_admission) try {
   recording_sender sender; // declared before the pipeline: it must outlive the destructor's join
   auto cfg = pipeline_config(small_batch);
   cfg.delivery.max_doc_bytes = static_cast<uint32_t>(filler_document.size());
   alert_monitor::pipeline pipeline{cfg, std::string{action_line}, sender.as_sender()};
   BOOST_CHECK(pipeline.submit_document(std::string{filler_document}) == alert_monitor::admission::queued);
   BOOST_CHECK(pipeline.submit_document(std::string{filler_document} + std::string{filler_document}) ==
               alert_monitor::admission::dropped_oversize);
   wait_for(pipeline, [](const auto& s) { return s.documents_indexed == 1; });
   pipeline.shutdown();
   // Once the delivery worker is stopped, nothing more is admitted.
   BOOST_CHECK(pipeline.submit_document(std::string{filler_document}) == alert_monitor::admission::dropped_queue_full);
   const auto done = pipeline.stats();
   BOOST_CHECK_EQUAL(done.query_documents_queued, 1u);
   BOOST_CHECK_EQUAL(done.query_documents_dropped_oversize, 1u);
   BOOST_CHECK_EQUAL(done.query_documents_dropped_queue_full, 1u);
   BOOST_CHECK_EQUAL(done.documents_indexed, 1u);
   // The status-document counters are untouched.
   BOOST_CHECK_EQUAL(done.documents_queued, 0u);
   BOOST_CHECK_EQUAL(done.documents_dropped_oversize, 0u);
   BOOST_CHECK_EQUAL(done.documents_dropped_queue_full, 0u);
}
FC_LOG_AND_RETHROW()

// --- failure throttle ------------------------------------------------------------------------------------

// The first event is reported at once with nothing held back; events inside failure_report_interval are counted, not
// reported; the first event at or after the interval reports that count; and the count then starts again from zero.
BOOST_AUTO_TEST_CASE(failure_throttle_reports_the_first_event_then_one_per_interval) try {
   using held_back = std::optional<uint64_t>;
   constexpr fc::microseconds interval = alert_monitor::failure_report_interval;
   const fc::time_point first = reference_now;
   alert_monitor::failure_throttle throttle;
   BOOST_CHECK(throttle.admit(first) == held_back{0});
   BOOST_CHECK(throttle.last_report == first);

   BOOST_CHECK(throttle.admit(first + inside_interval_step) == std::nullopt);
   BOOST_CHECK(throttle.admit(first + interval - smallest_step) == std::nullopt);
   BOOST_CHECK_EQUAL(throttle.unlogged, 2u);
   BOOST_CHECK(throttle.last_report == first);

   const fc::time_point second = first + interval;
   BOOST_CHECK(throttle.admit(second) == held_back{2});
   BOOST_CHECK(throttle.last_report == second);
   BOOST_CHECK_EQUAL(throttle.unlogged, 0u);

   // The count restarted: one event held back after the second report, then none after the third.
   BOOST_CHECK(throttle.admit(second + inside_interval_step) == std::nullopt);
   BOOST_CHECK(throttle.admit(second + interval) == held_back{1});
   BOOST_CHECK(throttle.admit(second + interval * 2) == held_back{0});
}
FC_LOG_AND_RETHROW()

// The held-back clause is empty when nothing was held back, and otherwise names the count and the events.
BOOST_AUTO_TEST_CASE(unlogged_events_clause_names_only_held_back_events) try {
   BOOST_CHECK(alert_monitor::unlogged_events_clause(0, throttled_events).empty());
   constexpr uint64_t held_back = 3;
   BOOST_CHECK_EQUAL(alert_monitor::unlogged_events_clause(held_back, throttled_events),
                     fmt::format(alert_monitor::unlogged_events_format, held_back, throttled_events));
}
FC_LOG_AND_RETHROW()

// --- query scheduling ------------------------------------------------------------------------------------

// An entry runs once per new LIB bucket (lib / interval): never in bucket 0, never twice for one bucket (a duplicate
// LIB, or later LIBs of the same bucket), once after a jump with no backfill of the buckets skipped, and at once when
// the first LIB seen is mid-bucket.
BOOST_AUTO_TEST_CASE(is_due_runs_an_entry_once_per_new_bucket) try {
   using lib_list = std::vector<chain::block_num_type>;
   struct due_case {
      uint32_t interval;
      lib_list libs;     ///< the LIBs observed, in order
      lib_list expected; ///< the LIBs at which the entry runs
   };
   const std::vector<due_case> cases{
      // bucket 0 never runs; a duplicate LIB does not run twice; a jump runs once
      {1, {0, 1, 1, 2, 3, 3, 7}, {1, 2, 3, 7}},
      // the bucket boundary, the rest of a bucket, a duplicate, and a jump from bucket 2 to 5 (3 and 4 never run)
      {10, {1, 9, 10, 11, 19, 20, 20, 21, 57, 58, 59, 60}, {10, 20, 57, 60}},
      // the first LIB seen is mid-bucket: it runs at once, then at the next boundary
      {100, {150, 151, 199, 200, 250, 300}, {150, 200, 300}},
      {1000, {999, 1000, 1001, 1999, 2000, 5500, 5999, 6000}, {1000, 2000, 5500, 6000}},
   };
   for (const auto& c : cases) {
      BOOST_TEST_CONTEXT("interval " << c.interval) {
         const auto due = due_libs(c.interval, c.libs);
         BOOST_CHECK_EQUAL_COLLECTIONS(due.begin(), due.end(), c.expected.begin(), c.expected.end());
      }
   }
   // The bucket already admitted is not due again; the next one is.
   BOOST_CHECK(!alert_monitor::is_due(every_ten_blocks, every_ten_blocks, 1));
   BOOST_CHECK(alert_monitor::is_due(2 * every_ten_blocks, every_ten_blocks, 1));
   // The largest interval has a single nonzero bucket, entered at the largest LIB.
   constexpr auto max_interval = std::numeric_limits<uint32_t>::max();
   BOOST_CHECK(!alert_monitor::is_due(max_interval - 1, max_interval, 0));
   BOOST_CHECK(alert_monitor::is_due(max_interval, max_interval, 0));
   // A zero interval, which parse_query_entry rejects, is never due.
   BOOST_CHECK(!alert_monitor::is_due(first_lib, 0, 0));
   BOOST_CHECK(!alert_monitor::is_due(max_interval, 0, 0));
}
FC_LOG_AND_RETHROW()

// --- query runner ----------------------------------------------------------------------------------------

// Without queries the runner creates no callers: dispatch and shutdown do nothing, and the provider is never asked.
BOOST_AUTO_TEST_CASE(query_runner_without_queries_does_nothing) try {
   runner_harness harness{std::vector<std::string>{}};
   std::atomic<uint32_t> provider_calls{0};
   harness.start([&] {
      ++provider_calls;
      return std::shared_ptr<sysio::query_engine::query_service>{};
   });
   harness.runner->dispatch(trigger_at(first_lib));
   harness.runner->shutdown();
   BOOST_CHECK_EQUAL(provider_calls.load(), 0u);
   const auto stats = harness.runner->stats();
   BOOST_CHECK_EQUAL(stats.succeeded, 0u);
   BOOST_CHECK_EQUAL(stats.failed_other, 0u);
}
FC_LOG_AND_RETHROW()

// dispatch() consults each entry's own interval: at LIB 10 the every-block and every-ten entries run, the
// every-hundred entry does not.
BOOST_AUTO_TEST_CASE(query_runner_dispatches_only_due_entries) try {
   constexpr std::string_view every_block_name = "every_block";
   constexpr std::string_view every_ten_name = "every_ten";
   constexpr std::string_view every_hundred_name = "every_hundred";
   runner_harness harness{{spec_of(every_block_name, every_block, scripted_query::answer),
                           spec_of(every_ten_name, every_ten_blocks, scripted_query::answer),
                           spec_of(every_hundred_name, every_hundred_blocks, scripted_query::answer)}};
   harness.start();
   harness.runner->dispatch(trigger_at(every_ten_blocks));
   constexpr uint64_t due_entries = 2;
   wait_for(*harness.pipeline, [](const auto& s) { return s.documents_indexed == due_entries; });
   harness.runner->shutdown(); // joins the callers: every run this dispatch admitted has finished
   BOOST_CHECK_EQUAL(harness.runner->stats().succeeded, due_entries);
   BOOST_CHECK_EQUAL(harness.service->timeouts().size(), due_entries);
   auto keys = delivered_data_keys(harness.sender);
   std::ranges::sort(keys);
   const std::vector<std::string> expected{std::string{every_block_name}, std::string{every_ten_name}};
   BOOST_CHECK_EQUAL_COLLECTIONS(keys.begin(), keys.end(), expected.begin(), expected.end());
}
FC_LOG_AND_RETHROW()

// Every entry has its own caller: more entries than a pool of four could run are all inside execute() at once, and
// each one's document is delivered under its own name.
BOOST_AUTO_TEST_CASE(query_runner_runs_every_entry_on_its_own_caller) try {
   runner_harness harness{repeated_specs(scripted_query::blocked, concurrent_entry_count)};
   harness.start();
   harness.runner->dispatch(trigger_at(first_lib));
   BOOST_REQUIRE(harness.service->wait_until_blocked(concurrent_entry_count));
   BOOST_CHECK_EQUAL(harness.service->peak_blocked(), concurrent_entry_count);
   harness.service->open_gate();
   const auto done = wait_for(*harness.runner, [](const auto& s) { return s.succeeded == concurrent_entry_count; });
   BOOST_CHECK_EQUAL(done.succeeded, concurrent_entry_count);
   BOOST_CHECK_EQUAL(done.skipped_busy, 0u);
   BOOST_CHECK_EQUAL(failed_by_any_kind(done), 0u);
   const auto delivered =
      wait_for(*harness.pipeline, [](const auto& s) { return s.documents_indexed == concurrent_entry_count; });
   BOOST_CHECK_EQUAL(delivered.query_documents_queued, concurrent_entry_count);
   auto keys = delivered_data_keys(harness.sender);
   std::ranges::sort(keys);
   std::vector<std::string> expected;
   for (const auto& entry : harness.cfg.queries)
      expected.push_back(entry.name);
   std::ranges::sort(expected);
   BOOST_CHECK_EQUAL_COLLECTIONS(keys.begin(), keys.end(), expected.begin(), expected.end());
}
FC_LOG_AND_RETHROW()

// An entry due again while its previous run is still active is skipped and counted -- never queued behind it -- and
// runs again once that run has finished.
BOOST_AUTO_TEST_CASE(query_runner_skips_and_counts_an_entry_still_running) try {
   constexpr std::string_view slow_name = "slow_entry";
   runner_harness harness{{spec_of(slow_name, every_block, scripted_query::blocked)}};
   harness.start();
   chain::block_num_type lib = first_lib;
   harness.runner->dispatch(trigger_at(lib));
   BOOST_REQUIRE(harness.service->wait_until_blocked(1));
   for (uint64_t i = 0; i < busy_dispatches; ++i)
      harness.runner->dispatch(trigger_at(++lib));
   // The first run is still blocked in execute(), so the count is exact.
   BOOST_CHECK_EQUAL(harness.runner->stats().skipped_busy, busy_dispatches);
   BOOST_CHECK_EQUAL(harness.service->timeouts().size(), 1u);

   harness.service->open_gate();
   BOOST_REQUIRE_EQUAL(wait_for(*harness.runner, [](const auto& s) { return s.succeeded == 1; }).succeeded, 1u);
   // The entry's running flag clears just after its outcome is counted, which nothing outside the runner observes,
   // so newer LIBs are offered (bounded) until one is admitted.
   const auto deadline = std::chrono::steady_clock::now() + drain_wait;
   while (harness.runner->stats().succeeded < 2 && std::chrono::steady_clock::now() < deadline) {
      harness.runner->dispatch(trigger_at(++lib));
      std::this_thread::sleep_for(drain_poll);
   }
   harness.runner->shutdown();
   const auto done = harness.runner->stats();
   BOOST_CHECK_EQUAL(done.succeeded, 2u);
   BOOST_CHECK(done.skipped_busy >= busy_dispatches);
   BOOST_CHECK_EQUAL(harness.service->timeouts().size(), 2u);
}
FC_LOG_AND_RETHROW()

// A failing entry -- any error kind, or a failure that is not a query_error -- blocks neither the other entries nor the
// status documents. Each failure is counted by kind and logged under its entry's name, never with its text.
BOOST_AUTO_TEST_CASE(a_failing_query_blocks_neither_other_queries_nor_status_documents) try {
   using sysio::query_engine::error_kind;
   constexpr std::string_view syntax_name = "syntax_failure";
   constexpr std::string_view limit_name = "limit_failure";
   constexpr std::string_view unexpected_name = "unexpected_failure";
   constexpr std::string_view healthy_a = "healthy_a";
   constexpr std::string_view healthy_b = "healthy_b";
   runner_harness harness{{spec_of(syntax_name, every_block, scripted_query::syntax_error),
                           spec_of(limit_name, every_block, scripted_query::limit_error),
                           spec_of(unexpected_name, every_block, scripted_query::unexpected_error),
                           spec_of(healthy_a, every_block, scripted_query::answer),
                           spec_of(healthy_b, every_block, scripted_query::answer)}};
   harness.start();
   BOOST_REQUIRE(harness.pipeline->submit(sample_snapshot()));
   harness.runner->dispatch(trigger_at(first_lib));
   constexpr uint64_t healthy_entries = 2;
   constexpr uint64_t delivered_documents = healthy_entries + 1; // plus the status document
   wait_for(*harness.runner, [&](const auto& s) {
      return s.succeeded == healthy_entries && s.failed_other == 1 &&
             s.failed[failure_slot(error_kind::QUERY_SYNTAX)] == 1 &&
             s.failed[failure_slot(error_kind::QUERY_LIMIT)] == 1;
   });
   const auto delivered =
      wait_for(*harness.pipeline, [](const auto& s) { return s.documents_indexed == delivered_documents; });
   harness.runner->shutdown(); // joins the callers, so every failure line has been logged
   const auto done = harness.runner->stats();
   BOOST_CHECK_EQUAL(done.succeeded, healthy_entries);
   BOOST_CHECK_EQUAL(done.failed[failure_slot(error_kind::QUERY_SYNTAX)], 1u);
   BOOST_CHECK_EQUAL(done.failed[failure_slot(error_kind::QUERY_LIMIT)], 1u);
   BOOST_CHECK_EQUAL(failed_by_any_kind(done), 2u);
   BOOST_CHECK_EQUAL(done.failed_other, 1u);
   BOOST_CHECK_EQUAL(done.skipped_busy, 0u);
   BOOST_CHECK_EQUAL(delivered.documents_indexed, delivered_documents);
   BOOST_CHECK_EQUAL(delivered.documents_queued, 1u);
   BOOST_CHECK_EQUAL(delivered.query_documents_queued, healthy_entries);

   auto keys = delivered_data_keys(harness.sender);
   std::ranges::sort(keys);
   const std::vector<std::string> expected{std::string{alert_monitor::record_key}, std::string{healthy_a},
                                           std::string{healthy_b}};
   BOOST_CHECK_EQUAL_COLLECTIONS(keys.begin(), keys.end(), expected.begin(), expected.end());

   BOOST_CHECK(harness.log.has_line_with({quoted_name(syntax_name), magic_enum::enum_name(error_kind::QUERY_SYNTAX)}));
   BOOST_CHECK(harness.log.has_line_with(
      {quoted_name(limit_name), magic_enum::enum_name(error_kind::QUERY_LIMIT), scripted_limit}));
   BOOST_CHECK(harness.log.has_line_with({quoted_name(unexpected_name)}));
   BOOST_CHECK(!harness.log.has_line_with({quoted_name(healthy_a)}));
   BOOST_CHECK(harness.log.never_logged(query_text_marker));
}
FC_LOG_AND_RETHROW()

// A query that keeps failing is logged once per failure_report_interval, not once per run: the failures after the
// first fall inside the interval, so they are counted and held back.
BOOST_AUTO_TEST_CASE(a_query_failing_repeatedly_inside_the_interval_is_logged_once) try {
   using sysio::query_engine::error_kind;
   constexpr std::string_view failing_name = "repeated_failure";
   runner_harness harness{{spec_of(failing_name, every_block, scripted_query::syntax_error)}};
   harness.start();
   const auto syntax_failures = [&harness] {
      return harness.runner->stats().failed[failure_slot(error_kind::QUERY_SYNTAX)];
   };
   chain::block_num_type lib = first_lib;
   harness.runner->dispatch(trigger_at(lib));
   // The entry's running flag clears just after its failure is counted, which nothing outside the runner observes,
   // so newer LIBs are offered (bounded) until the later runs have failed too.
   const auto deadline = std::chrono::steady_clock::now() + drain_wait;
   while (syntax_failures() < repeated_failures && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(drain_poll);
      harness.runner->dispatch(trigger_at(++lib));
   }
   harness.runner->shutdown(); // joins the caller, so every report has been made
   BOOST_REQUIRE_GE(syntax_failures(), repeated_failures);
   const std::string name_fragment = quoted_name(failing_name);
   const auto lines = std::ranges::count_if(harness.log.sink->messages(), [&](const std::string& line) {
      return line.find(name_fragment) != std::string::npos;
   });
   BOOST_CHECK_EQUAL(lines, 1);
}
FC_LOG_AND_RETHROW()

// No service (the engine not started yet, or already stopped) is counted as failed_other and logged by name.
BOOST_AUTO_TEST_CASE(a_missing_query_service_is_counted) try {
   constexpr std::string_view orphan_name = "orphan_entry";
   runner_harness harness{{spec_of(orphan_name, every_block, scripted_query::answer)}};
   harness.start([] { return std::shared_ptr<sysio::query_engine::query_service>{}; });
   harness.runner->dispatch(trigger_at(first_lib));
   wait_for(*harness.runner, [](const auto& s) { return s.failed_other == 1; });
   harness.runner->shutdown();
   const auto done = harness.runner->stats();
   BOOST_CHECK_EQUAL(done.failed_other, 1u);
   BOOST_CHECK_EQUAL(done.succeeded, 0u);
   BOOST_CHECK_EQUAL(failed_by_any_kind(done), 0u);
   BOOST_CHECK_EQUAL(harness.pipeline->stats().query_documents_queued, 0u);
   BOOST_CHECK(harness.log.has_line_with({quoted_name(orphan_name)}));
   BOOST_CHECK(harness.log.never_logged(query_text_marker));
}
FC_LOG_AND_RETHROW()

// A query document above the single-document cap is dropped and counted on the query counters, never sent, and the
// drop is logged against the entry's name -- it is not a query failure.
BOOST_AUTO_TEST_CASE(an_oversize_query_document_is_dropped_and_reported_by_name) try {
   constexpr std::string_view oversize_name = "oversize_entry";
   runner_harness harness{{spec_of(oversize_name, every_block, scripted_query::answer)}};
   harness.cfg.delivery.max_doc_bytes = tiny_doc_byte_cap;
   harness.start();
   harness.runner->dispatch(trigger_at(first_lib));
   wait_for(*harness.pipeline, [](const auto& s) { return s.query_documents_dropped_oversize == 1; });
   harness.runner->shutdown(); // joins the caller, so its report has been logged
   const auto documents = harness.pipeline->stats();
   BOOST_CHECK_EQUAL(documents.query_documents_dropped_oversize, 1u);
   BOOST_CHECK_EQUAL(documents.query_documents_queued, 0u);
   BOOST_CHECK_EQUAL(documents.query_documents_dropped_queue_full, 0u);
   BOOST_CHECK_EQUAL(documents.documents_dropped_oversize, 0u); // the status counter is untouched
   const auto queries = harness.runner->stats();
   BOOST_CHECK_EQUAL(queries.succeeded, 0u);
   BOOST_CHECK_EQUAL(queries.failed_other, 0u);
   BOOST_CHECK_EQUAL(failed_by_any_kind(queries), 0u);
   BOOST_CHECK_EQUAL(harness.sender.sends.load(), 0u);
   BOOST_CHECK(harness.log.has_line_with(
      {quoted_name(oversize_name), magic_enum::enum_name(alert_monitor::admission::dropped_oversize)}));
   BOOST_CHECK(harness.log.never_logged(query_text_marker));
}
FC_LOG_AND_RETHROW()

// A query document behind a full delivery queue is dropped and counted on the query counters, and logged by name.
BOOST_AUTO_TEST_CASE(a_query_document_behind_a_full_delivery_queue_is_dropped) try {
   constexpr std::string_view crowded_name = "crowded_entry";
   runner_harness harness{{spec_of(crowded_name, every_block, scripted_query::answer)}};
   harness.sender.hold_first = true;
   harness.cfg.max_pending_documents = single_pending_document;
   harness.start();
   // The first filler holds the delivery worker inside the sender; the second fills the one-deep queue behind it.
   BOOST_REQUIRE(harness.pipeline->submit_document(std::string{filler_document}) == alert_monitor::admission::queued);
   BOOST_REQUIRE(harness.sender.first_send_started.try_acquire_for(drain_wait));
   BOOST_REQUIRE(harness.pipeline->submit_document(std::string{filler_document}) == alert_monitor::admission::queued);
   harness.runner->dispatch(trigger_at(first_lib));
   wait_for(*harness.pipeline, [](const auto& s) { return s.query_documents_dropped_queue_full == 1; });
   harness.runner->shutdown();
   harness.sender.release();
   const auto documents = harness.pipeline->stats();
   BOOST_CHECK_EQUAL(documents.query_documents_dropped_queue_full, 1u);
   BOOST_CHECK_EQUAL(documents.query_documents_queued, 2u); // the two fillers
   BOOST_CHECK_EQUAL(documents.documents_dropped_queue_full, 0u); // the status counter is untouched
   BOOST_CHECK_EQUAL(harness.runner->stats().succeeded, 0u);
   BOOST_CHECK(harness.log.has_line_with(
      {quoted_name(crowded_name), magic_enum::enum_name(alert_monitor::admission::dropped_queue_full)}));
}
FC_LOG_AND_RETHROW()

// Every execution carries the fixed query_deadline, whatever the query does.
BOOST_AUTO_TEST_CASE(every_query_execution_carries_query_deadline) try {
   constexpr auto names = std::to_array<std::string_view>({"answers", "syntax", "limit", "unexpected"});
   runner_harness harness{{spec_of(names[0], every_block, scripted_query::answer),
                           spec_of(names[1], every_block, scripted_query::syntax_error),
                           spec_of(names[2], every_block, scripted_query::limit_error),
                           spec_of(names[3], every_block, scripted_query::unexpected_error)}};
   harness.start();
   harness.runner->dispatch(trigger_at(first_lib));
   const auto entries = harness.cfg.queries.size();
   wait_for(*harness.runner,
            [&](const auto& s) { return s.succeeded + s.failed_other + failed_by_any_kind(s) == entries; });
   harness.runner->shutdown();
   const auto timeouts = harness.service->timeouts();
   BOOST_REQUIRE_EQUAL(timeouts.size(), entries);
   for (const auto& timeout : timeouts) {
      BOOST_REQUIRE(timeout.has_value());
      BOOST_CHECK_EQUAL(timeout->count(), alert_monitor::query_deadline.count());
   }
}
FC_LOG_AND_RETHROW()

// An execution the engine refuses as QUERY_BUSY is retried until it is admitted: the entry succeeds, and each retry
// carries what is left of the first attempt's query_deadline.
BOOST_AUTO_TEST_CASE(a_busy_query_is_retried_within_its_deadline) try {
   runner_harness harness{{spec_of("busy_then_admitted", every_block, scripted_query::busy_then_answer)}};
   harness.start();
   harness.runner->dispatch(trigger_at(first_lib));
   wait_for(*harness.runner, [](const auto& s) { return s.succeeded == 1; });
   harness.runner->shutdown();
   const auto done = harness.runner->stats();
   BOOST_CHECK_EQUAL(done.succeeded, 1u);
   BOOST_CHECK_EQUAL(failed_by_any_kind(done), 0u);
   const auto timeouts = harness.service->timeouts();
   BOOST_REQUIRE_EQUAL(timeouts.size(), scripted_busy_refusals + 1);
   BOOST_REQUIRE(timeouts.front().has_value());
   BOOST_CHECK_EQUAL(timeouts.front()->count(), alert_monitor::query_deadline.count());
   for (std::size_t i = 1; i < timeouts.size(); ++i) {
      BOOST_REQUIRE(timeouts[i].has_value());
      BOOST_CHECK(*timeouts[i] < *timeouts[i - 1]);
   }
}
FC_LOG_AND_RETHROW()

// A query refused as QUERY_BUSY until its deadline runs out is counted and logged as QUERY_BUSY, after more than one
// attempt.
BOOST_AUTO_TEST_CASE(a_query_busy_until_its_deadline_fails_as_busy) try {
   using sysio::query_engine::error_kind;
   constexpr std::string_view busy_name = "always_busy";
   runner_harness harness{{spec_of(busy_name, every_block, scripted_query::busy)}};
   harness.start();
   harness.runner->dispatch(trigger_at(first_lib));
   wait_for(*harness.runner, [](const auto& s) { return s.failed[failure_slot(error_kind::QUERY_BUSY)] == 1; });
   harness.runner->shutdown(); // joins the caller, so its report has been logged
   const auto done = harness.runner->stats();
   BOOST_CHECK_EQUAL(done.failed[failure_slot(error_kind::QUERY_BUSY)], 1u);
   BOOST_CHECK_EQUAL(done.succeeded, 0u);
   BOOST_CHECK_GT(harness.service->timeouts().size(), 1u);
   BOOST_CHECK(harness.log.has_line_with({quoted_name(busy_name), magic_enum::enum_name(error_kind::QUERY_BUSY)}));
}
FC_LOG_AND_RETHROW()

// shutdown() ends a QUERY_BUSY retry at once instead of letting it run out its deadline.
BOOST_AUTO_TEST_CASE(shutdown_ends_a_busy_retry) try {
   runner_harness harness{{spec_of("busy_at_shutdown", every_block, scripted_query::busy)}};
   harness.start();
   harness.runner->dispatch(trigger_at(first_lib));
   // A second attempt shows the caller is inside the retry loop.
   const auto deadline = std::chrono::steady_clock::now() + drain_wait;
   while (harness.service->timeouts().size() < 2 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(drain_poll);
   BOOST_REQUIRE_GE(harness.service->timeouts().size(), 2u);
   const auto started = std::chrono::steady_clock::now();
   harness.runner->shutdown();
   const auto elapsed = std::chrono::steady_clock::now() - started;
   BOOST_CHECK_MESSAGE(
      elapsed < busy_shutdown_bound,
      fmt::format("shutdown took {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
}
FC_LOG_AND_RETHROW()

// shutdown() with every caller blocked returns once each blocked call reaches its deadline -- bounded, not waiting for
// the service -- and is idempotent; a dispatch afterwards admits nothing.
BOOST_AUTO_TEST_CASE(query_runner_shutdown_is_bounded_with_blocked_callers) try {
   using sysio::query_engine::error_kind;
   runner_harness harness{repeated_specs(scripted_query::blocked, concurrent_entry_count)};
   harness.start();
   harness.runner->dispatch(trigger_at(first_lib));
   BOOST_REQUIRE(harness.service->wait_until_blocked(concurrent_entry_count));
   const auto started = std::chrono::steady_clock::now();
   harness.runner->shutdown();
   const auto elapsed = std::chrono::steady_clock::now() - started;
   BOOST_CHECK_MESSAGE(
      elapsed < shutdown_bound,
      fmt::format("shutdown took {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
   BOOST_CHECK_EQUAL(harness.service->returned(), concurrent_entry_count);
   BOOST_CHECK_EQUAL(harness.service->blocked(), 0u);
   const auto done = harness.runner->stats();
   BOOST_CHECK_EQUAL(done.failed[failure_slot(error_kind::QUERY_TIMEOUT)], concurrent_entry_count);
   BOOST_CHECK_EQUAL(done.succeeded, 0u);

   harness.runner->shutdown(); // idempotent
   harness.runner->dispatch(trigger_at(first_lib + 1));
   BOOST_CHECK_EQUAL(harness.service->timeouts().size(), concurrent_entry_count);
   BOOST_CHECK_EQUAL(harness.runner->stats().skipped_busy, 0u);
}
FC_LOG_AND_RETHROW()

// Destroying a runner that was never shut down joins its callers, bounded the same way.
BOOST_AUTO_TEST_CASE(query_runner_destruction_joins_its_callers) try {
   runner_harness harness{repeated_specs(scripted_query::blocked, concurrent_entry_count)};
   harness.start();
   harness.runner->dispatch(trigger_at(first_lib));
   BOOST_REQUIRE(harness.service->wait_until_blocked(concurrent_entry_count));
   const auto started = std::chrono::steady_clock::now();
   harness.runner.reset(); // the destructor, with no shutdown() before it
   const auto elapsed = std::chrono::steady_clock::now() - started;
   BOOST_CHECK_MESSAGE(
      elapsed < shutdown_bound,
      fmt::format("destruction took {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
   // Every blocked call has returned: the destructor waited for each caller instead of leaving it running.
   BOOST_CHECK_EQUAL(harness.service->returned(), concurrent_entry_count);
   BOOST_CHECK_EQUAL(harness.service->blocked(), 0u);
}
FC_LOG_AND_RETHROW()

// --- plugin class ----------------------------------------------------------------------------------------

namespace {

/// Parse arguments through the plugin's public option surface, as appbase would (no application needed).
bpo::variables_map parse_plugin_options(alert_monitor_plugin& plugin, const std::vector<std::string>& arguments) {
   bpo::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);
   bpo::variables_map options;
   bpo::store(bpo::command_line_parser(arguments).options(cfg).run(), options);
   bpo::notify(options);
   return options;
}

} // anonymous namespace

// Every option is a `cfg` option (config.ini and command line); the empty `cli` set deliberately pins the
// decision that every knob is settable from config.ini.
BOOST_AUTO_TEST_CASE(plugin_registers_the_component_options_as_config_options) try {
   alert_monitor_plugin plugin;
   bpo::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);
   BOOST_CHECK(cli.options().empty());
   for (const auto name : every_alert_monitor_option) {
      BOOST_CHECK_MESSAGE(cfg.find_nothrow(std::string{name}, false) != nullptr, "missing option " + std::string{name});
   }
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(plugin_initialize_propagates_config_errors_and_tolerates_no_target) try {
   const template_file tpl{data_only_template};
   alert_monitor_plugin plugin;
   BOOST_CHECK_NO_THROW(plugin.plugin_initialize(parse_plugin_options(plugin, {})));
   BOOST_CHECK_THROW(
      plugin.plugin_initialize(parse_plugin_options(
         plugin, {"--alert-monitor-target-url=ftp://127.0.0.1:9", std::string{index_argument}, tpl.argument()})),
      chain::plugin_config_exception);
   const std::vector<std::string> active = required_arguments(tpl);
   BOOST_CHECK_NO_THROW(plugin.plugin_initialize(parse_plugin_options(plugin, active)));
}
FC_LOG_AND_RETHROW()

// plugin_initialize runs the startup parse: a query that does not parse fails initialization.
BOOST_AUTO_TEST_CASE(plugin_initialize_rejects_a_query_that_does_not_parse) try {
   const template_file tpl{data_only_template};
   std::vector<std::string> arguments = required_arguments(tpl);
   arguments.push_back(query_argument(spec_of("broken_entry", every_block, unparseable_query)));
   alert_monitor_plugin plugin;
   BOOST_CHECK_THROW(plugin.plugin_initialize(parse_plugin_options(plugin, arguments)), chain::plugin_config_exception);
}
FC_LOG_AND_RETHROW()

// The plugin resolves a relative template path against app().config_dir(): the directory nodeop's --config-dir
// names, so a node directory stays relocatable.
BOOST_AUTO_TEST_CASE(plugin_resolves_a_relative_template_path_against_the_config_dir) try {
   const template_file tpl{data_only_template};
   const std::vector<std::string> relative{std::string{target_argument}, std::string{index_argument},
                                           std::string{template_argument_prefix} + tpl.path.filename().string()};
   // app() is process-wide: restore its config directory for the cases after this one.
   const std::filesystem::path previous_config_dir = appbase::app().config_dir();
   auto restore_config_dir = fc::make_scoped_exit([&] { appbase::app().set_default_config_dir(previous_config_dir); });

   appbase::app().set_default_config_dir(tpl.dir.path());
   alert_monitor_plugin resolved;
   BOOST_CHECK_NO_THROW(resolved.plugin_initialize(parse_plugin_options(resolved, relative)));

   const fc::temp_directory elsewhere;
   appbase::app().set_default_config_dir(elsewhere.path());
   alert_monitor_plugin missing;
   BOOST_CHECK_EXCEPTION(missing.plugin_initialize(parse_plugin_options(missing, relative)),
                         chain::plugin_config_exception, [&](const chain::plugin_config_exception& e) {
                            return e.to_detail_string().find((elsewhere.path() / tpl.path.filename()).string()) !=
                                   std::string::npos;
                         });
}
FC_LOG_AND_RETHROW()

// appbase calls plugin_shutdown() on every running plugin however far plugin_startup() got, so it may not
// require anything from it (plugins/usage_pattern.md). Neither a plugin that was never initialized nor one that
// was initialized and never started has a subscription, a client, a pipeline, or a read-only handle, and shutting
// either down must be a no-op rather than a throw.
BOOST_AUTO_TEST_CASE(plugin_shutdown_tolerates_never_initialized_and_never_started) try {
   alert_monitor_plugin never_initialized;
   BOOST_CHECK_NO_THROW(never_initialized.plugin_shutdown());

   const template_file tpl{data_only_template};
   const std::vector<std::string> active = required_arguments(tpl);
   alert_monitor_plugin never_started;
   BOOST_CHECK_NO_THROW(never_started.plugin_initialize(parse_plugin_options(never_started, active)));
   BOOST_CHECK_NO_THROW(never_started.plugin_shutdown());
}
FC_LOG_AND_RETHROW()

// --- end to end ------------------------------------------------------------------------------------------

// One configured query, end to end: parse_config reads its spec, the runner executes it through a real query engine
// over a real chain (the query engine's own fixture, with the query_fixture contract's seeded positions), and the
// rendered document travels through the pipeline and a real es client to a loopback capture server. The document is
// keyed by the query's name, carries the record, the entry, the trigger, and the engine's result -- and never the
// query's text.
BOOST_FIXTURE_TEST_CASE(a_query_document_reaches_the_endpoint_end_to_end, sysio::query_engine::test::chain_fixture)
try {
   namespace key = alert_monitor::query_document::key;
   namespace qe = sysio::query_engine;
   const template_file tpl{data_only_template};
   fc::test::capture_http_server server;
   const auto cfg = alert_monitor::parse_config(
      parse_options({fmt::format("--{}={}", alert_monitor::option::target_url, server.url()),
                     std::string{index_argument}, tpl.argument(),
                     query_argument(spec_of(end_to_end_query_name, end_to_end_block_interval, end_to_end_query))}),
      tpl.dir.path());
   BOOST_REQUIRE(cfg.has_value());
   BOOST_REQUIRE_EQUAL(cfg->queries.size(), 1u);

   qe::test::read_queue reads;
   // A clock that never advances: no deadline -- query_deadline included -- can expire however slowly a sanitizer
   // build runs; the bounded pump below bounds the case instead.
   const auto frozen_now = qe::query_budget::clock::now();
   auto engine = std::make_shared<qe::query_engine>(qe::query_config{}, reads.create_api(*validating_node),
                                                    [frozen_now] { return frozen_now; });
   captured_log log;
   fc::network::es::es_client client{cfg->delivery, cfg->transport};
   alert_monitor::pipeline pipeline{*cfg, client.action_line(), [&client](std::string body, uint32_t doc_count) {
                                       return client.bulk(std::move(body), doc_count);
                                    }};
   alert_monitor::query_runner runner{*cfg, pipeline, [engine] { return engine; }, log.log};
   // Declared after the runner, so it runs first on every exit path, a failed REQUIRE included: with the frozen clock a
   // caller waiting on a read nobody pumps never times out, and stopping the engine fails it at once (QUERY_CANCELLED),
   // so the runner's join below never waits on it.
   auto stop_engine_on_exit = fc::make_scoped_exit([&] { engine->stop(); });

   // The head block stands in for the LIB (always nonzero, so the entry is due): the runner only copies the trigger.
   const auto& head = validating_node->head();
   const alert_monitor::query_trigger trigger{
      .lib_num = head.block_num(), .lib_id = head.id(), .lib_time = head.block_time()};
   runner.dispatch(trigger);
   // The caller blocks on reads the engine posts to this thread: pump them until the outcome is counted and the
   // document has reached the endpoint.
   const auto deadline = std::chrono::steady_clock::now() + qe::test::test_wait;
   while (runner.stats().succeeded == 0 || server.request_count() == 0) {
      BOOST_REQUIRE_MESSAGE(std::chrono::steady_clock::now() < deadline,
                            "the query document never reached the endpoint");
      reads.run_one();
   }
   runner.shutdown();
   const auto stats = runner.stats();
   BOOST_CHECK_EQUAL(stats.succeeded, 1u);
   BOOST_CHECK_EQUAL(stats.failed_other, 0u);
   BOOST_CHECK_EQUAL(failed_by_any_kind(stats), 0u);

   const std::string body = server.request(0).body;
   BOOST_CHECK(body.find(end_to_end_query) == std::string::npos);
   const auto lines = split_bulk_lines(body);
   BOOST_REQUIRE_EQUAL(lines.size(), 2u); // one action line, one document
   BOOST_CHECK_EQUAL(lines[0] + "\n", client.action_line());
   const auto data = fc::json::from_string(lines[1]).get_object()["data"].get_object();
   BOOST_REQUIRE_EQUAL(data.size(), 1u);
   BOOST_REQUIRE(data.contains(std::string{end_to_end_query_name}));
   const auto& document = data[std::string{end_to_end_query_name}].get_object();
   BOOST_CHECK_EQUAL(document[key::record].as_string(), alert_monitor::query_document::record);
   BOOST_CHECK_EQUAL(document[key::version].as_uint64(), alert_monitor::query_document::version);
   const auto& entry = document[key::query].get_object();
   BOOST_CHECK_EQUAL(entry[key::name].as_string(), end_to_end_query_name);
   BOOST_CHECK_EQUAL(entry[key::ordinal].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(entry[key::block_interval].as_uint64(), end_to_end_block_interval);
   const auto& trigger_member = document[key::trigger].get_object();
   BOOST_CHECK_EQUAL(trigger_member[key::lib_num].as_uint64(), trigger.lib_num);
   BOOST_CHECK_EQUAL(trigger_member[key::lib_id].as_string(), trigger.lib_id.str());
   BOOST_CHECK(document.contains(key::observed_at));
   const auto& result = document[key::result].get_object();
   BOOST_CHECK_EQUAL(result[qe::response_field::schema_version].as_string(), qe::constants::schema_version);
   BOOST_CHECK_EQUAL(result[qe::response_field::rows].get_array().size(), fixture_position_rows);
   BOOST_CHECK_EQUAL(result[qe::response_field::source].get_object()[qe::response_field::table].as_string(),
                     fixture_positions_table);
   BOOST_CHECK(log.never_logged(query_text_marker));
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
