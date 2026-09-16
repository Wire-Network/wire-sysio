#include <algorithm>
#include <array>
#include <atomic>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <fc-test/capture_http_server.hpp>
#include <fc/exception/exception.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>
#include <fc/io/json_template.hpp>
#include <fc/network/es/es_client.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/variant.hpp>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <magic_enum/magic_enum.hpp>
#include <mutex>
#include <optional>
#include <semaphore>
#include <set>
#include <string>
#include <string_view>
#include <sysio/chain/exceptions.hpp>
#include <sysio/status_monitor_plugin/status_monitor.hpp>
#include <sysio/status_monitor_plugin/status_monitor_plugin.hpp>
#include <thread>
#include <utility>
#include <vector>

using namespace sysio;
using namespace sysio::chain::literals;
using fc::test::split_bulk_lines;
namespace bpo = boost::program_options;

namespace {

constexpr std::string_view target_argument = "--status-monitor-target-url=http://127.0.0.1:9";
constexpr std::string_view index_argument = "--status-monitor-target-index=test-status";
constexpr std::string_view template_option = "--status-monitor-target-template-file=";
/// The smallest useful template: the snapshot and nothing else.
constexpr std::string_view data_only_template = R"({"data":"${data}"})";
/// The get_info_results fields sample_info() populates (fc omits unset std::optional members).
constexpr auto expected_get_info_fields = std::to_array<std::string_view>(
   {"server_version", "chain_id", "head_block_num", "last_irreversible_block_num", "last_irreversible_block_id",
    "head_block_id", "head_block_time", "head_block_producer", "virtual_block_cpu_limit", "virtual_block_net_limit",
    "block_cpu_limit", "block_net_limit", "fork_db_head_block_num", "total_cpu_weight"});
/// A 64-hex block id for the sample snapshot's LIB (block 41).
constexpr std::string_view sample_lib_block_id = "0000002900000000000000000000000000000000000000000000000000000000";
/// Above 0xffffffff, so fc renders it as a JSON string -- the same rule the HTTP body follows.
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
/// Every --status-monitor-* option the component registers; both option cases iterate it.
constexpr auto every_status_monitor_option = std::to_array<std::string_view>(
   {status_monitor::option::target_url, status_monitor::option::target_index,
    status_monitor::option::target_template_file, status_monitor::option::username, status_monitor::option::password,
    status_monitor::option::max_items_per_task, status_monitor::option::max_pending_documents,
    status_monitor::option::connect_timeout_ms, status_monitor::option::request_timeout_ms,
    status_monitor::option::max_retries, status_monitor::option::retry_backoff_ms,
    status_monitor::option::additional_ca_file, status_monitor::option::additional_ca_path,
    status_monitor::option::proxy});
/// Transport arguments: a node-wide proxy and CA file, and the plugin's own proxy.
constexpr std::string_view global_proxy_argument = "--outbound-http-proxy=http://global-proxy:3128";
constexpr std::string_view global_ca_file_argument = "--outbound-http-additional-ca-file=/tmp/global-ca.pem";
constexpr std::string_view status_monitor_proxy_argument = "--status-monitor-proxy=http://status-proxy:3128";
constexpr std::string_view status_monitor_proxy = "http://status-proxy:3128";
constexpr std::string_view global_ca_file = "/tmp/global-ca.pem";

/// A temporary directory holding a template file written from @p text; the path is an option argument.
struct template_file {
   fc::temp_directory dir;
   std::filesystem::path path = dir.path() / "template.json";

   explicit template_file(std::string_view text) {
      std::ofstream out{path};
      out << text;
   }
   std::string argument() const { return std::string{template_option} + path.string(); }
};

/// Parse status-monitor arguments through the component's option surface (no application, no plugin class), plus
/// the node-wide outbound HTTP options http_client_plugin registers in nodeop.
bpo::variables_map parse_options(const std::vector<std::string>& arguments) {
   bpo::options_description cfg;
   status_monitor::add_options(cfg);
   sysio::outbound_http::add_global_transport_program_options(cfg);
   bpo::variables_map options;
   bpo::store(bpo::command_line_parser(arguments).options(cfg).run(), options);
   bpo::notify(options);
   return options;
}

/// parse_config over @p arguments plus the target, index, and template every active configuration needs.
std::optional<status_monitor::config> active_config(const template_file& tpl, std::vector<std::string> arguments = {}) {
   arguments.emplace_back(target_argument);
   arguments.emplace_back(index_argument);
   arguments.push_back(tpl.argument());
   return status_monitor::parse_config(parse_options(arguments));
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

status_monitor::status_snapshot sample_snapshot(uint32_t head_block_num = sample_head_block_num) {
   return {sample_info(head_block_num), reference_now};
}

/// A config for pipeline tests; the delivery settings are unused because the sender is a recorder.
status_monitor::config pipeline_config(uint32_t max_items_per_task) {
   status_monitor::config cfg;
   cfg.document_template = fc::json_template::parse(data_only_template);
   cfg.max_items_per_task = max_items_per_task;
   return cfg;
}

/// Three small newline-terminated documents for the body-assembly cases (assemble_bulk_bodies consumes them).
std::vector<std::string> sample_documents() {
   return {"{\"a\":1}\n", "{\"b\":2}\n", "{\"c\":3}\n"};
}

/// The snapshot object inside a rendered data-only document.
fc::variant_object status_of(const std::string& document) {
   return fc::json::from_string(document)
      .get_object()["data"]
      .get_object()[std::string{status_monitor::record_key}]
      .get_object();
}

/// Records every body a pipeline sends. Replies `indexed` unless `fail` is set; when `hold_first` is set, the
/// first send blocks until release() is called, so later documents accumulate in the queue.
struct recording_sender {
   std::mutex mtx;
   std::vector<status_monitor::bulk_body> bodies; ///< guarded by mtx
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

   status_monitor::pipeline::sender as_sender() {
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

   status_monitor::pipeline::failure_reporter as_reporter() {
      return [this](const fc::network::es::es_bulk_result& result, uint32_t doc_count) {
         std::lock_guard<std::mutex> lk(mtx);
         calls.push_back({result, doc_count});
      };
   }
};

/// Poll @p pipeline until @p done(stats) or drain_wait elapses; returns the final stats.
template <typename Predicate>
status_monitor::pipeline_stats wait_for(const status_monitor::pipeline& pipeline, Predicate done) {
   const auto deadline = std::chrono::steady_clock::now() + drain_wait;
   auto stats = pipeline.stats();
   while (!done(stats) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(drain_poll);
      stats = pipeline.stats();
   }
   return stats;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(status_monitor_plugin_tests)

// --- options ---------------------------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(every_option_is_registered) try {
   bpo::options_description cfg;
   status_monitor::add_options(cfg);
   std::set<std::string> names;
   for (const auto& option : cfg.options())
      names.insert(option->long_name());
   for (const auto name : every_status_monitor_option) {
      BOOST_CHECK_MESSAGE(names.contains(std::string{name}), "missing option " + std::string{name});
   }
   // The template option's help text is generated from supplied_tokens, so it lists exactly the names
   // parse_config accepts -- and never the extra-fields token, which parse_config rejects.
   const auto* template_option = cfg.find_nothrow(status_monitor::option::target_template_file, false);
   BOOST_REQUIRE(template_option != nullptr);
   const std::string& template_help = template_option->description();
   BOOST_CHECK(template_help.find(rejected_token) == std::string::npos);
   for (const auto name : status_monitor::supplied_tokens) {
      BOOST_CHECK_MESSAGE(template_help.find(name) != std::string::npos,
                          "template help is missing supplied token " + std::string{name});
   }
   // A ceiling is formatted from the constant that enforces it, not spelled out.
   const auto* max_items_option = cfg.find_nothrow(status_monitor::option::max_items_per_task, false);
   BOOST_REQUIRE(max_items_option != nullptr);
   BOOST_CHECK(max_items_option->description().find(std::to_string(status_monitor::max_items_per_task_ceiling)) !=
               std::string::npos);
   // Each half of the auth pair names the other from its constant, so renaming one cannot leave the help stale.
   const auto* username_option = cfg.find_nothrow(status_monitor::option::username, false);
   BOOST_REQUIRE(username_option != nullptr);
   BOOST_CHECK(username_option->description().find(fmt::format("--{}", status_monitor::option::password)) !=
               std::string::npos);
}
FC_LOG_AND_RETHROW()

// No target URL: disabled, and the other options are not even validated (a config.ini can carry the knobs).
BOOST_AUTO_TEST_CASE(absent_target_url_disables_without_validating_the_rest) try {
   BOOST_CHECK(!status_monitor::parse_config(parse_options({})).has_value());
   BOOST_CHECK(!status_monitor::parse_config(parse_options({"--status-monitor-max-items-per-task=0"})).has_value());
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(defaults_apply_when_target_index_and_template_are_given) try {
   const template_file tpl{data_only_template};
   const auto cfg = active_config(tpl);
   BOOST_REQUIRE(cfg.has_value());
   BOOST_CHECK_EQUAL(cfg->delivery.url, "http://127.0.0.1:9");
   BOOST_CHECK_EQUAL(cfg->delivery.index, "test-status");
   BOOST_CHECK(!cfg->delivery.username.has_value());
   BOOST_CHECK_EQUAL(cfg->delivery.max_batch_bytes, status_monitor::max_batch_bytes);
   BOOST_CHECK_EQUAL(cfg->delivery.max_doc_bytes, status_monitor::max_doc_bytes);
   BOOST_CHECK_EQUAL(cfg->delivery.max_retries, fc::network::es::es_default_max_retries);
   BOOST_CHECK_EQUAL(cfg->delivery.retry_backoff_ms, fc::network::es::es_default_retry_backoff_ms);
   BOOST_CHECK_EQUAL(cfg->delivery.connect_timeout_ms, fc::network::es::es_default_connect_timeout_ms);
   BOOST_CHECK_EQUAL(cfg->delivery.request_timeout_ms, fc::network::es::es_default_request_timeout_ms);
   BOOST_CHECK(cfg->document_template.contains(fc::json_template_default_token::data));
   BOOST_CHECK_EQUAL(cfg->max_items_per_task, status_monitor::default_max_items_per_task);
   BOOST_CHECK_EQUAL(cfg->max_pending_documents, status_monitor::default_max_pending_documents);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(explicit_values_override_the_defaults) try {
   const template_file tpl{data_only_template};
   const auto cfg =
      active_config(tpl, {"--status-monitor-username=u", "--status-monitor-password=p",
                          "--status-monitor-max-items-per-task=7", "--status-monitor-max-pending-documents=9",
                          "--status-monitor-connect-timeout-ms=11", "--status-monitor-request-timeout-ms=13",
                          "--status-monitor-max-retries=0", "--status-monitor-retry-backoff-ms=17"});
   BOOST_REQUIRE(cfg.has_value());
   BOOST_CHECK_EQUAL(cfg->delivery.username.value_or(""), "u");
   BOOST_CHECK_EQUAL(cfg->delivery.password.value_or(""), "p");
   BOOST_CHECK_EQUAL(cfg->max_items_per_task, 7u);
   BOOST_CHECK_EQUAL(cfg->max_pending_documents, 9u);
   BOOST_CHECK_EQUAL(cfg->delivery.connect_timeout_ms, 11u);
   BOOST_CHECK_EQUAL(cfg->delivery.request_timeout_ms, 13u);
   BOOST_CHECK_EQUAL(cfg->delivery.max_retries, 0u);
   BOOST_CHECK_EQUAL(cfg->delivery.retry_backoff_ms, 17u);
}
FC_LOG_AND_RETHROW()

// Each status-monitor-* transport option overrides its outbound-http-* counterpart, which applies when the
// plugin's own option is absent; with neither, the transport stays at its defaults.
BOOST_AUTO_TEST_CASE(transport_options_overlay_the_node_wide_values) try {
   const template_file tpl{data_only_template};
   const auto cfg = active_config(tpl, {std::string{global_proxy_argument}, std::string{global_ca_file_argument},
                                        std::string{status_monitor_proxy_argument}});
   BOOST_REQUIRE(cfg.has_value());
   BOOST_REQUIRE(cfg->transport.proxy.has_value());
   BOOST_CHECK_EQUAL(*cfg->transport.proxy, status_monitor_proxy);
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
   // the index and the template are required once a target is set
   BOOST_CHECK_THROW(status_monitor::parse_config(parse_options({std::string{target_argument}, tpl.argument()})),
                     plugin_config_exception);
   BOOST_CHECK_THROW(
      status_monitor::parse_config(parse_options({std::string{target_argument}, std::string{index_argument}})),
      plugin_config_exception);
   // The empty index rides in as a separate token: boost::program_options rejects the `--option=` spelling with
   // an empty value during parsing, so parse_config's own emptiness check is only reachable this way.
   BOOST_CHECK_THROW(status_monitor::parse_config(parse_options(
                        {std::string{target_argument}, "--status-monitor-target-index", "", tpl.argument()})),
                     plugin_config_exception);
   // a template that is missing, not JSON, not an object, or malformed
   BOOST_CHECK_THROW(status_monitor::parse_config(
                        parse_options({std::string{target_argument}, std::string{index_argument},
                                       std::string{template_option} + (tpl.dir.path() / "missing.json").string()})),
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
   BOOST_CHECK_THROW(active_config(tpl, {"--status-monitor-username=u"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--status-monitor-password=p"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--status-monitor-max-items-per-task=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--status-monitor-max-pending-documents=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--status-monitor-max-items-per-task=" +
                                         std::to_string(status_monitor::max_items_per_task_ceiling + 1)}),
                     plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--status-monitor-max-pending-documents=" +
                                         std::to_string(status_monitor::max_pending_documents_ceiling + 1)}),
                     plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--status-monitor-connect-timeout-ms=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--status-monitor-request-timeout-ms=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(active_config(tpl, {"--status-monitor-retry-backoff-ms=0"}), plugin_config_exception);
   BOOST_CHECK_THROW(
      active_config(tpl, {"--status-monitor-max-retries=" + std::to_string(status_monitor::max_retries_ceiling + 1)}),
      plugin_config_exception);
   BOOST_CHECK_THROW(status_monitor::parse_config(parse_options({"--status-monitor-target-url=ftp://127.0.0.1:9",
                                                                 std::string{index_argument}, tpl.argument()})),
                     plugin_config_exception);
   // An empty target URL rides in as a separate token for the same reason the empty index does; the plugin reports
   // the client's own "url is required" against the target option.
   BOOST_CHECK_THROW(status_monitor::parse_config(parse_options(
                        {"--status-monitor-target-url", "", std::string{index_argument}, tpl.argument()})),
                     plugin_config_exception);
}
FC_LOG_AND_RETHROW()

// --- documents -------------------------------------------------------------------------------------------

// The shipped sample compiles, and every token in it is replaced: the timestamp is a bare number, the level
// is INFO, message/category are the plugin's names, the location points at the renderer, the snapshot sits at
// data.status_monitor, and the literal identity members come through as written.
BOOST_AUTO_TEST_CASE(render_document_replaces_every_token_of_the_shipped_sample) try {
   const auto tpl = fc::json_template::from_file(std::filesystem::path{STATUS_MONITOR_SAMPLE_TEMPLATE});
   const auto line = status_monitor::render_document(sample_snapshot(), tpl);
   BOOST_REQUIRE(!line.empty());
   BOOST_CHECK_EQUAL(line.back(), '\n');
   BOOST_CHECK_EQUAL(line.find('\n'), line.size() - 1);
   BOOST_CHECK(line.find(R"("@timestamp":)" + std::to_string(reference_now_millis()) + ",") != std::string::npos);
   const auto doc = fc::json::from_string(line).get_object();
   BOOST_CHECK_EQUAL(doc["@timestamp"].as_int64(), reference_now_millis());
   BOOST_CHECK_EQUAL(doc["level"].as_string(), "INFO");
   BOOST_CHECK_EQUAL(doc["message"].as_string(), std::string{status_monitor::record_message});
   BOOST_CHECK_EQUAL(doc["category"].as_string(), std::string{status_monitor::logger_name});
   const auto source_location = doc["sourceLocation"].as_string();
   BOOST_CHECK(source_location.find("status_monitor.cpp:") != std::string::npos);
   BOOST_CHECK(source_location.ends_with(" render_document"));
   const auto& status = doc["data"].get_object()[std::string{status_monitor::record_key}].get_object();
   BOOST_CHECK_EQUAL(status["head_block_num"].as_uint64(), sample_head_block_num);
   BOOST_CHECK_EQUAL(status["last_irreversible_block_num"].as_uint64(), sample_head_block_num - 1);
   BOOST_CHECK_EQUAL(status["server_version"].as_string(), "0badf00d");
   BOOST_CHECK_EQUAL(status["head_block_producer"].as_string(), "sysio");
   BOOST_CHECK(status["total_cpu_weight"].is_string());
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
   for (const auto name : status_monitor::supplied_tokens)
      text += fmt::format(R"("{0}":"${{{0}}}",)", name);
   text.back() = '}';
   const auto line = status_monitor::render_document(sample_snapshot(), fc::json_template::parse(text));
   const auto doc = fc::json::from_string(line).get_object();
   BOOST_CHECK_EQUAL(doc.size(), status_monitor::supplied_tokens.size());
   BOOST_CHECK_EQUAL(doc["level"].as_string(), std::string{status_monitor::record_level});
   BOOST_CHECK_EQUAL(doc["epoch_millis"].as_int64(), reference_now_millis());
   BOOST_CHECK_EQUAL(doc["data"].get_object().size(), 1u);
}
FC_LOG_AND_RETHROW()

// Byte parity with /v1/chain/get_info: the snapshot is embedded verbatim -- the text the HTTP layer sends for
// the same struct (beast_http_session: fc::json::to_string(fc::variant(result), fc::time_point::maximum())) --
// and the document is exactly what the template says, nothing added.
BOOST_AUTO_TEST_CASE(render_document_embeds_the_http_body_verbatim_and_adds_nothing) try {
   const auto snapshot = sample_snapshot();
   const std::string http_body = fc::json::to_string(fc::variant{snapshot.info}, fc::time_point::maximum());
   const auto data_only = fc::json_template::parse(data_only_template);
   const std::string line = status_monitor::render_document(snapshot, data_only);
   BOOST_CHECK_EQUAL(line, R"({"data":{")" + std::string{status_monitor::record_key} + R"(":)" + http_body + "}}\n");
   const std::string literal_only = status_monitor::render_document(snapshot, fc::json_template::parse(R"({"x":"y"})"));
   BOOST_CHECK_EQUAL(literal_only, R"({"x":"y"})"
                                   "\n");
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(assemble_bulk_bodies_pairs_each_document_with_the_action_line_and_consumes_them) try {
   auto documents = sample_documents();
   const auto bodies = status_monitor::assemble_bulk_bodies(documents, action_line, status_monitor::max_batch_bytes);
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
   BOOST_CHECK(status_monitor::assemble_bulk_bodies({}, action_line, status_monitor::max_batch_bytes).empty());
}
FC_LOG_AND_RETHROW()

// A cap that fits exactly one pair splits three documents into three bodies; every body stays within the cap
// except by the one pair es_sink also allows.
BOOST_AUTO_TEST_CASE(assemble_bulk_bodies_splits_at_the_body_byte_cap) try {
   auto documents = sample_documents();
   const std::size_t one_pair = action_line.size() + documents[0].size();
   const auto bodies = status_monitor::assemble_bulk_bodies(documents, action_line, one_pair);
   BOOST_REQUIRE_EQUAL(bodies.size(), 3u);
   for (const auto& body : bodies) {
      BOOST_CHECK_EQUAL(body.doc_count, 1u);
      BOOST_CHECK_EQUAL(body.body.size(), one_pair);
   }
   auto more = sample_documents(); // the first call consumed `documents`
   const auto two_per_body = status_monitor::assemble_bulk_bodies(more, action_line, 2 * one_pair);
   BOOST_REQUIRE_EQUAL(two_per_body.size(), 2u);
   BOOST_CHECK_EQUAL(two_per_body[0].doc_count, 2u);
   BOOST_CHECK_EQUAL(two_per_body[1].doc_count, 1u);

   // A cap below even one pair loses nothing: the close only fires once a body already holds a pair, so every
   // document still ships, alone in a body of its own and in order.
   const auto expected = sample_documents();
   auto below_one_pair = sample_documents();
   const auto singles = status_monitor::assemble_bulk_bodies(below_one_pair, action_line, one_pair - 1);
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
   status_monitor::pipeline pipeline{pipeline_config(small_batch), std::string{action_line}, sender.as_sender()};
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
   status_monitor::pipeline pipeline{pipeline_config(small_batch), std::string{action_line}, sender.as_sender()};
   for (uint64_t i = 0; i < failing_count; ++i)
      BOOST_REQUIRE(pipeline.submit(sample_snapshot(static_cast<uint32_t>(i))));
   // batches_failed is incremented after documents_failed, so a snapshot taken between the two would see the
   // documents without their batch: the counters are read after shutdown()'s join, once both workers are done.
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
      status_monitor::pipeline pipeline{pipeline_config(small_batch), std::string{action_line}, sender.as_sender(),
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
   status_monitor::pipeline indexing_pipeline{pipeline_config(small_batch), std::string{action_line},
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
   status_monitor::pipeline pipeline{cfg, std::string{action_line}, sender.as_sender()};
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
   status_monitor::pipeline pipeline{cfg, std::string{action_line}, sender.as_sender()};
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
   using status_monitor::classify_progress;
   using status_monitor::progress;
   status_monitor::pipeline_stats previous;
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

// --- plugin class ----------------------------------------------------------------------------------------

namespace {

/// Parse arguments through the plugin's public option surface, as appbase would (no application needed).
bpo::variables_map parse_plugin_options(status_monitor_plugin& plugin, const std::vector<std::string>& arguments) {
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
   status_monitor_plugin plugin;
   bpo::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);
   BOOST_CHECK(cli.options().empty());
   for (const auto name : every_status_monitor_option) {
      BOOST_CHECK_MESSAGE(cfg.find_nothrow(std::string{name}, false) != nullptr, "missing option " + std::string{name});
   }
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(plugin_initialize_propagates_config_errors_and_tolerates_no_target) try {
   const template_file tpl{data_only_template};
   status_monitor_plugin plugin;
   BOOST_CHECK_NO_THROW(plugin.plugin_initialize(parse_plugin_options(plugin, {})));
   BOOST_CHECK_THROW(
      plugin.plugin_initialize(parse_plugin_options(
         plugin, {"--status-monitor-target-url=ftp://127.0.0.1:9", std::string{index_argument}, tpl.argument()})),
      chain::plugin_config_exception);
   const std::vector<std::string> active{std::string{target_argument}, std::string{index_argument}, tpl.argument()};
   BOOST_CHECK_NO_THROW(plugin.plugin_initialize(parse_plugin_options(plugin, active)));
}
FC_LOG_AND_RETHROW()

// appbase calls plugin_shutdown() on every running plugin however far plugin_startup() got, so it may not
// require anything from it (plugins/usage_pattern.md). Neither a plugin that was never initialized nor one that
// was initialized and never started has a subscription, a client, a pipeline, or a read-only handle, and shutting
// either down must be a no-op rather than a throw.
BOOST_AUTO_TEST_CASE(plugin_shutdown_tolerates_never_initialized_and_never_started) try {
   status_monitor_plugin never_initialized;
   BOOST_CHECK_NO_THROW(never_initialized.plugin_shutdown());

   const template_file tpl{data_only_template};
   const std::vector<std::string> active{std::string{target_argument}, std::string{index_argument}, tpl.argument()};
   status_monitor_plugin never_started;
   BOOST_CHECK_NO_THROW(never_started.plugin_initialize(parse_plugin_options(never_started, active)));
   BOOST_CHECK_NO_THROW(never_started.plugin_shutdown());
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
