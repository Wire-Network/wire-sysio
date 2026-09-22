#pragma once

#include <fc/crypto/sha256.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/database_utils.hpp>
#include <sysio/chain_plugin/table_read.hpp>

#include <boost/multiprecision/cpp_int.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sysio::query_engine_plugin {

/// Protocol/parser constants shared by the service, parser and schema tests.
namespace constants {
inline constexpr auto endpoint = "/v1/query/execute";
inline constexpr auto method = "query.execute";
inline constexpr auto version = "2.0";
inline constexpr auto schema_version = "1.0";
inline constexpr auto logger = "query";
inline constexpr uint32_t max_id_characters = 128;
inline constexpr int64_t max_numeric_id = 4294967295LL;
inline constexpr uint32_t max_tokens = 4096;
inline constexpr uint32_t max_depth = 64;
inline constexpr uint32_t max_ast_nodes = 2048;
inline constexpr uint32_t decimal_places = 18;
inline constexpr uint32_t page_rows = 512;
inline constexpr int64_t no_timeout = -1;
inline constexpr uint32_t accumulator_bits = 256;
inline constexpr uint32_t comparison_bits = 512;
inline constexpr uint32_t decimal_base = 10;
inline constexpr auto value_namespace = "value";
inline constexpr auto key_namespace = "key";
inline constexpr size_t max_owners = 64;
} // namespace constants

/// Stable response member names, shared by capture, evaluation and RPC serialization.
namespace response_field {
inline constexpr auto schema_version = "schema_version";
inline constexpr auto complete = "complete";
inline constexpr auto source = "source";
inline constexpr auto state = "state";
inline constexpr auto columns = "columns";
inline constexpr auto rows = "rows";
inline constexpr auto stats = "stats";
inline constexpr auto owners = "owners";
inline constexpr auto table = "table";
inline constexpr auto name = "name";
inline constexpr auto logical_type = "logical_type";
inline constexpr auto nullable = "nullable";
inline constexpr auto encoding = "encoding";
inline constexpr auto abi_type = "abi_type";
inline constexpr auto scanned_rows = "scanned_rows";
inline constexpr auto matched_rows = "matched_rows";
inline constexpr auto groups = "groups";
inline constexpr auto returned_rows = "returned_rows";
inline constexpr auto raw_bytes = "raw_bytes";
inline constexpr auto elapsed_us = "elapsed_us";
inline constexpr auto owner = "owner";
inline constexpr auto hash = "hash";
inline constexpr auto chain_id = "chain_id";
inline constexpr auto block_id = "block_id";
inline constexpr auto block_num = "block_num";
inline constexpr auto block_time = "block_time";
inline constexpr auto read_mode = "read_mode";
inline constexpr auto last_irreversible_block_num = "last_irreversible_block_num";
inline constexpr auto abis = "abis";
inline constexpr auto captured_at = "captured_at";
inline constexpr auto synced = "synced";
inline constexpr auto result = "result";
inline constexpr auto error = "error";
inline constexpr auto kind = "kind";
inline constexpr auto retryable = "retryable";
inline constexpr auto line = "line";
inline constexpr auto column = "column";
inline constexpr auto limit = "limit";
inline constexpr auto code = "code";
inline constexpr auto message = "message";
inline constexpr auto data = "data";
} // namespace response_field

/// Resource option names and default values shared by registration and enforcement.
namespace option {
inline constexpr auto worker_threads = "query-worker-threads";
inline constexpr auto max_in_flight = "query-max-in-flight";
inline constexpr auto max_query_bytes = "query-max-query-bytes";
inline constexpr auto timeout_ms = "query-timeout-ms";
inline constexpr auto max_capture_ms = "query-max-capture-ms";
inline constexpr auto max_abi_bytes = "query-max-abi-bytes";
inline constexpr auto max_scan_rows = "query-max-scan-rows";
inline constexpr auto max_raw_bytes = "query-max-raw-bytes";
inline constexpr auto max_memory_bytes = "query-max-memory-bytes";
inline constexpr auto max_groups = "query-max-groups";
inline constexpr auto max_result_rows = "query-max-result-rows";
inline constexpr auto max_response_bytes = "query-max-response-bytes";
} // namespace option
namespace defaults {
inline constexpr uint32_t worker_threads = 2;
inline constexpr uint32_t max_in_flight = 4;
inline constexpr uint32_t max_query_bytes = 16384;
inline constexpr uint32_t timeout_ms = 1000;
inline constexpr uint32_t max_capture_ms = 50;
inline constexpr uint32_t max_abi_bytes = 1048576;
inline constexpr uint64_t max_scan_rows = 100000;
inline constexpr uint64_t max_raw_bytes = 67108864;
inline constexpr uint64_t max_memory_bytes = 134217728;
inline constexpr uint64_t max_groups = 10000;
inline constexpr uint64_t max_result_rows = 10000;
inline constexpr uint64_t max_response_bytes = 8388608;
} // namespace defaults

/// Immutable startup limits, in the units named by each field.
struct query_config {
   uint32_t worker_threads = defaults::worker_threads;
   uint32_t max_in_flight = defaults::max_in_flight;
   uint32_t max_query_bytes = defaults::max_query_bytes;
   uint32_t timeout_ms = defaults::timeout_ms;
   uint32_t max_capture_ms = defaults::max_capture_ms;
   uint32_t max_abi_bytes = defaults::max_abi_bytes;
   uint64_t max_scan_rows = defaults::max_scan_rows;
   uint64_t max_raw_bytes = defaults::max_raw_bytes;
   uint64_t max_memory_bytes = defaults::max_memory_bytes;
   uint64_t max_groups = defaults::max_groups;
   uint64_t max_result_rows = defaults::max_result_rows;
   uint64_t max_response_bytes = defaults::max_response_bytes;
   /// Reject zero, incompatible limits, and admitted-memory multiplication overflow.
   void validate() const;
};

/// Per-call execution controls. Pagination follows complete evaluation; LIMIT in SQL is also respected.
struct query_options {
   int64_t timeout_ms = constants::no_timeout; ///< -1 disables the end-to-end deadline; zero expires immediately.
   std::optional<uint64_t> limit;              ///< Maximum returned rows, still subject to the configured row cap.
   uint64_t offset = 0;                        ///< Rows skipped after sorting and before applying the limit.
};

/// Stable error kinds; wire codes are described by error_code(), independently of enum layout.
enum class error_kind {
   PARSE_ERROR,
   INVALID_REQUEST,
   METHOD_NOT_FOUND,
   INVALID_PARAMS,
   QUERY_SYNTAX,
   QUERY_SEMANTICS,
   QUERY_LIMIT,
   QUERY_TIMEOUT,
   QUERY_BUSY,
   SCHEMA_CHANGED,
   ROW_DECODE_ERROR,
   VALUE_ERROR,
   STATE_UNAVAILABLE,
   QUERY_CANCELLED,
   INTERNAL_ERROR
};

/// One-based source location; absent for failures outside SQL parsing/binding.
struct source_span {
   uint32_t line = 1;
   uint32_t column = 1;
};

/// Safe client-visible failure. Never embed query text or controller exception details.
struct query_error : std::runtime_error {
   error_kind kind;
   std::optional<source_span> span;
   std::optional<std::string> limit;
   explicit query_error(error_kind, std::string message, std::optional<source_span> = std::nullopt,
                        std::optional<std::string> limit = std::nullopt);
};

/// Budget is owned for the request's full lifetime. Cancellation is cross-thread;
/// counters are touched only by the one running stage. Allocation charges are conservative
/// and never recycled, so released intermediates cannot cause under-accounting.
class query_budget {
public:
   using clock = std::chrono::steady_clock;
   using now_function = std::function<clock::time_point()>;
   explicit query_budget(query_config, now_function now = clock::now,
                         const std::optional<query_options>& options = std::nullopt);
   /// Check cancellation and the absolute admission deadline.
   void check() const;
   /// Check the tighter coherent capture deadline in addition to the request deadline.
   void check_capture(clock::time_point capture_start) const;
   /// Charge before allocation. Arithmetic checks precede counter mutation.
   void charge_memory(uint64_t bytes);
   /// Charge source rows and raw bytes, also charging the total memory budget.
   void charge_raw(uint64_t rows, uint64_t bytes);
   /// Fail with QUERY_LIMIT if value exceeds maximum, naming the violated option.
   void assert_limit(uint64_t value, uint64_t maximum, const char* option) const;
   /// Monotonic elapsed time used for result stats and deterministic tests.
   uint64_t elapsed_us() const;
   query_config config;
   query_options options;
   now_function now;
   clock::time_point started;
   clock::time_point deadline;
   std::atomic<bool> cancelled{false};
   uint64_t scanned_rows = 0;
   uint64_t raw_bytes = 0;
   uint64_t accounted_bytes = 0;
};

/// Exact, checked integer arithmetic; no host floating-point conversion is permitted.
using integer = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
   constants::accumulator_bits, constants::accumulator_bits, boost::multiprecision::signed_magnitude,
   boost::multiprecision::checked, void>>;
using comparison_integer = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
   constants::comparison_bits, constants::comparison_bits, boost::multiprecision::signed_magnitude,
   boost::multiprecision::checked, void>>;

/// Public logical types and serialization encodings; names are their wire spellings.
enum class logical_type { integer, decimal, boolean, text, time, asset, extended_asset, json, ieee_hex };
enum class encoding { decimal_string, boolean, text, asset_object, json, ieee_hex };
enum class truth { false_value, true_value, unknown };

/// Resolved ABI primitives. Aliases retain their public ABI name in the descriptor.
enum class primitive_type {
   boolean,
   int8,
   uint8,
   int16,
   uint16,
   int32,
   uint32,
   int64,
   uint64,
   int128,
   uint128,
   varint32,
   varuint32,
   varint_int64,
   varint_uint64,
   float32,
   float64,
   float128,
   name,
   slug_name,
   string,
   bytes,
   checksum160,
   checksum256,
   checksum512,
   public_key,
   signature,
   symbol,
   symbol_code,
   asset,
   extended_asset,
   time_point,
   time_point_sec,
   block_timestamp_type,
   bitset
};

/// Exact scalar or already-normalized container. AVG remains numerator/denominator until rendering.
struct value {
   logical_type type = logical_type::text;
   primitive_type primitive = primitive_type::string;
   bool null = true;
   integer numerator = 0;
   integer denominator = 1;
   std::string text;
   std::string symbol;
   std::string contract;
   uint8_t precision = 0;
   fc::variant container;
};

/// SQL field paths preserve each identifier, including quoted components containing dots.
using field_path = std::vector<std::string>;
enum class aggregate_function { COUNT, SUM, AVG, MIN, MAX };
enum class expression_kind { field, literal, aggregate };
enum class predicate_kind { comparison, is_null, logical_not, logical_and, logical_or };
enum class comparison_operator { equal, not_equal, less, less_equal, greater, greater_equal };

/// One owned expression; field/aggregate slots are assigned only by the planner.
struct expression {
   expression_kind kind = expression_kind::field;
   source_span span;
   field_path path;
   value literal;
   aggregate_function aggregate = aggregate_function::COUNT;
   bool star = false;
   std::optional<size_t> field_slot;
   std::optional<size_t> aggregate_slot;
};

/// Owned predicate tree with explicit SQL precedence and three-valued logic.
struct predicate {
   predicate_kind kind = predicate_kind::comparison;
   comparison_operator operation = comparison_operator::equal;
   expression left;
   expression right;
   bool negated = false;
   std::vector<predicate> children;
};

/// Projection item; SELECT * is represented explicitly for semantic validation.
struct select_item {
   expression source;
   std::optional<std::string> alias;
   bool star = false;
};
/// Ordering references output names only, then a bound output slot.
struct order_item {
   std::string name;
   bool descending = false;
   size_t slot = 0;
};

/// AST is completely owned; no ANTLR object or view survives parse_query().
struct ast_query {
   std::vector<std::string> owners;
   std::string table;
   std::vector<select_item> select;
   std::optional<predicate> where;
   std::vector<field_path> group_by;
   std::optional<predicate> having;
   std::vector<order_item> order_by;
   std::optional<uint64_t> limit;
};

/// ABI structure kinds, distinct from the scalar logical types exposed in responses.
enum class type_kind { primitive, structure, array, optional, extension, variant };
struct type_descriptor;
/// A named field in a compiled ABI descriptor.
struct field_descriptor {
   std::string name;
   std::shared_ptr<const type_descriptor> type;
};
/// Copied, resolved ABI type, built on a worker. Recursive ABI graphs are rejected.
struct type_descriptor {
   std::string abi_type;
   type_kind kind = type_kind::primitive;
   logical_type logical = logical_type::text;
   primitive_type primitive = primitive_type::string;
   std::vector<field_descriptor> fields;
   std::shared_ptr<const type_descriptor> element;
   std::vector<std::shared_ptr<const type_descriptor>> alternatives;
};

/// Metadata copied under the read queue; descriptor compilation never touches the controller.
struct table_schema {
   chain::name owner;
   chain::abi_def abi;
   chain::table_def table;
   fc::sha256 abi_hash;
   uint64_t abi_sequence = 0;
   std::shared_ptr<const type_descriptor> row_type;
   std::shared_ptr<const type_descriptor> key_type;
   std::vector<chain::be_key_codec::key_shape> key_shapes;
};

/// One source field and the ABI scalar/container type established by planning.
struct bound_field {
   field_path path;
   bool key = false;
   bool nullable = false;
   std::shared_ptr<const type_descriptor> type;
};
/// Shared aggregate slot for equivalent calls in SELECT and HAVING.
struct aggregate_slot {
   aggregate_function function = aggregate_function::COUNT;
   std::optional<size_t> field;
   logical_type type = logical_type::integer;
};
/// Response column description; ABI type is absent on aggregate outputs.
struct output_column {
   std::string name;
   logical_type type = logical_type::text;
   std::optional<std::string> abi_type;
   bool nullable = false;
   encoding representation = encoding::text;
};
/// Compatible row/key schemas across explicit owners, with a common primary range and residual predicate.
struct typed_plan {
   ast_query ast;
   std::vector<std::shared_ptr<const table_schema>> schemas;
   std::vector<bound_field> fields;
   std::vector<size_t> groups;
   std::vector<aggregate_slot> aggregates;
   std::vector<output_column> columns;
   chain_apis::primary_scan_request scan;
   bool aggregate = false;
   bool empty_range = false;
};

/// A captured row retains its owning account for stable ordering across equal primary keys.
struct captured_row {
   chain_apis::owned_table_row row;
   chain::name owner;
};
/// State identity captured alongside raw rows; rendered only after leaving the read queue.
struct captured_input {
   std::vector<captured_row> rows;
   fc::variant state;
   uint64_t native_pages = 0;
   uint64_t capture_us = 0;
};
/// Evaluated result, containing no references to source/controller storage.
struct query_result {
   std::string schema_version = constants::schema_version;
   bool complete = true;
   fc::variant_object source;
   fc::variant_object state;
   std::vector<fc::variant_object> columns;
   std::vector<fc::variant_object> rows;
   fc::variant_object stats;
};
/// Parsed JSON-RPC request. A missing ID is a notification; null is a valid response ID.
struct query_request {
   fc::variant id;
   bool notification = false;
   std::string query;
   std::optional<query_error> invocation_error;
};

/// Parse the bounded SQL subset, throwing position-bearing QUERY_SYNTAX errors.
ast_query parse_query(std::string_view, query_budget&);
/// Compile copied ABI, bind the query, and select a provably safe primary range.
typed_plan create_plan(ast_query, std::vector<table_schema>, query_budget&);
/// Evaluate all captured input with exact types before applying LIMIT.
query_result evaluate(const typed_plan&, captured_input, query_budget&);
/// Parse a decimal directly from token text with checked coefficient and scale.
value parse_number(std::string_view);
/// Render exact numbers to at most 18 decimal places with round-half-even.
std::string render_number(const value&);
/// Compare compatible non-null scalars exactly; containers/opaque floats are rejected.
int compare_values(const value&, const value&);
/// Normalize a value into the public, recursively number-free cell encoding.
fc::variant to_cell(const value&, query_budget&);
/// Decode and bind one raw row to the plan's source field slots.
std::vector<value> decode_fields(const typed_plan&, const chain_apis::owned_table_row&, query_budget&);
/// Convert a string literal using a known scalar ABI type; no implicit general coercion.
value coerce_literal(value, const type_descriptor&);
/// Resolve aliases, inheritance and bounded container descriptors in copied ABI metadata.
void compile_schema(table_schema&, query_budget&);
/// Exact rational addition with checked accumulator narrowing and unit compatibility.
void add_value(value&, const value&);
/// Validate scalar compatibility without inspecting arbitrary variant values.
bool is_numeric(logical_type);
bool is_ordered(logical_type);
/// Compare encoded primary keys as unsigned bytes, matching the database index.
int compare_keys(const std::vector<char>&, const std::vector<char>&);
/// Derive the required encoding from the logical type.
encoding value_encoding(logical_type);
/// Numeric JSON-RPC code and retry policy for a stable error kind.
int error_code(error_kind);
bool retryable(error_kind);
/// Produce protocol success/error envelopes; safe null ID if the request was invalid.
fc::variant create_success(const query_request&, query_result&&);
fc::variant create_error(const query_request*, const query_error&);
/// Validate the single-request profile, retaining valid notification status on invocation errors.
query_request parse_request(std::string_view, query_budget&);

} // namespace sysio::query_engine_plugin

FC_REFLECT(sysio::query_engine_plugin::query_result, (schema_version)(complete)(source)(state)(columns)(rows)(stats))
