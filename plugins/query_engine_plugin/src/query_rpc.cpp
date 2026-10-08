#include <fc/variant_object.hpp>
#include <sysio/query_engine_plugin/query.hpp>

#include <magic_enum/magic_enum.hpp>
#include <rapidjson/document.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <set>
#include <span>

namespace sysio::query_engine {
namespace {
constexpr uint64_t maximum_json_escape_bytes = 6;
constexpr uint64_t envelope_allowance = 4096;
constexpr auto key_version = "jsonrpc";
constexpr auto key_id = "id";
constexpr auto key_method = "method";
constexpr auto key_params = "params";
constexpr auto invalid_envelope_message = "Invalid JSON-RPC request";
constexpr auto invalid_params_message = "Expected named query parameter";
constexpr auto invalid_parameter_name_message = "Unknown query parameter";
constexpr auto duplicate_parameter_message = "Duplicate query parameter";
constexpr auto lowers_only_infix = " may only lower ";
/// Every member of the root request object.
constexpr auto envelope_members = std::to_array<std::string_view>({key_version, key_id, key_method, key_params});
constexpr uint32_t parse_flags =
   rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag | rapidjson::kParseIterativeFlag;

/// Preflight bounds JSON nesting and retains token kinds without ever parsing a host float. Under
/// kParseNumbersAsStringsFlag the document holds numbers as strings, so the token kind of every
/// top-level params member is recorded here.
struct envelope_preflight : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, envelope_preflight> {
   query_budget& budget;
   uint32_t depth = 0;
   std::string root_key;
   std::string parameter_key;
   bool version_string = false, method_string = false, id_number = false;
   /// params members whose value token was a JSON string, respectively a JSON number.
   std::set<std::string, std::less<>> string_parameters, numeric_parameters;
   explicit envelope_preflight(query_budget& budget)
      : budget(budget) {}
   bool Key(const char* text, rapidjson::SizeType length, bool) {
      budget.check();
      if (depth == 1)
         root_key.assign(text, length);
      else if (depth == 2 && root_key == key_params)
         parameter_key.assign(text, length);
      return true;
   }
   bool String(const char*, rapidjson::SizeType, bool) {
      if (depth == 1 && root_key == key_version)
         version_string = true;
      if (depth == 1 && root_key == key_method)
         method_string = true;
      if (depth == 2 && root_key == key_params)
         string_parameters.insert(parameter_key);
      return true;
   }
   bool RawNumber(const char*, rapidjson::SizeType, bool) {
      if (depth == 1 && root_key == key_id)
         id_number = true;
      if (depth == 2 && root_key == key_params)
         numeric_parameters.insert(parameter_key);
      return true;
   }
   bool StartObject() {
      budget.check();
      return ++depth <= constants::max_depth;
   }
   bool StartArray() { return StartObject(); }
   bool EndObject(rapidjson::SizeType) {
      --depth;
      return true;
   }
   bool EndArray(rapidjson::SizeType) {
      --depth;
      return true;
   }
};

/// Why an object's member names fail a closed member list.
enum class member_violation { unknown, duplicate };

/// The first member of `object` whose name is not in `allowed` or repeats an earlier name.
std::optional<member_violation> find_member_violation(const rapidjson::Value& object,
                                                      std::span<const std::string_view> allowed) {
   std::set<std::string_view> names;
   for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member) {
      const std::string_view name(member->name.GetString(), member->name.GetStringLength());
      if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
         return member_violation::unknown;
      if (!names.insert(name).second)
         return member_violation::duplicate;
   }
   return std::nullopt;
}

/// Count validated UTF-8 code points, the JSON Schema maxLength unit.
size_t characters(std::string_view text) {
   size_t result = 0;
   constexpr unsigned char continuation_mask = 0xc0, continuation_tag = 0x80;
   for (unsigned char byte : text)
      if ((byte & continuation_mask) != continuation_tag)
         ++result;
   return result;
}

/// The exact integer value of a raw JSON number token.
struct integral_value {
   bool negative = false;
   uint64_t magnitude = 0;
};

/// Exact lexical integer of a raw JSON number token: the fraction and exponent are applied to the
/// decimal digits, never through a host float, so `42.0` and `4.2e1` are 42 while `0.1` is not an
/// integer. Empty when the token is not integral or its magnitude exceeds `magnitude_bound`.
std::optional<integral_value> parse_integral(std::string_view number, uint64_t magnitude_bound) {
   integral_value result{.negative = number.starts_with('-')};
   if (result.negative)
      number.remove_prefix(1);
   const auto exponent_start = number.find_first_of("eE");
   int32_t exponent = 0;
   if (exponent_start != std::string_view::npos) {
      auto text = number.substr(exponent_start + 1);
      if (text.starts_with('+'))
         text.remove_prefix(1);
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), exponent);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
         return std::nullopt;
      number = number.substr(0, exponent_start);
   }
   std::string digits;
   const auto point = number.find('.');
   int64_t scale = point == std::string_view::npos ? 0 : number.size() - point - 1;
   for (char character : number)
      if (character != '.')
         digits.push_back(character);
   const auto first = digits.find_first_not_of('0');
   if (first == std::string::npos)
      return result;
   digits.erase(0, first);
   scale -= exponent;
   while (scale > 0 && !digits.empty() && digits.back() == '0') {
      digits.pop_back();
      --scale;
   }
   const auto max_digits = std::to_string(magnitude_bound).size();
   if (scale > 0 || scale < -int64_t(max_digits) || digits.size() + size_t(-scale) > max_digits)
      return std::nullopt;
   digits.append(size_t(-scale), '0');
   for (char character : digits) {
      const uint64_t digit = character - '0';
      // Defensive: the digit-count check above already keeps every current bound (at most 16 digits)
      // far from overflow, but a `magnitude_bound` near UINT64_MAX admits 20-digit tokens that would wrap.
      if (result.magnitude > (std::numeric_limits<uint64_t>::max() - digit) / constants::decimal_base)
         return std::nullopt;
      result.magnitude = result.magnitude * constants::decimal_base + digit;
   }
   if (result.magnitude > magnitude_bound)
      return std::nullopt;
   return result;
}

/// Accept integral JSON numbers, including exponent notation, within the numeric ID range.
int64_t parse_id(std::string_view number) {
   const auto id = parse_integral(number, constants::max_numeric_id);
   if (!id)
      throw query_error(error_kind::INVALID_REQUEST, invalid_envelope_message);
   const auto magnitude = static_cast<int64_t>(id->magnitude);
   return id->negative ? -magnitude : magnitude;
}

/// Preserve embedded NUL bytes so validation never accepts a truncated spelling.
std::string_view string_view(const rapidjson::Value& value) {
   return {value.GetString(), value.GetStringLength()};
}

/// An optional integer member of params and the inclusive range its exact value must lie in.
struct integer_parameter {
   std::string_view field;
   uint64_t minimum;
   uint64_t maximum;
};
constexpr integer_parameter limit_parameter{request_field::limit, constants::min_request_integer,
                                            constants::max_request_integer};
constexpr integer_parameter offset_parameter{request_field::offset, constants::min_request_integer,
                                             constants::max_request_integer};
constexpr integer_parameter timeout_parameter{request_field::timeout_ms, constants::min_request_timeout_ms,
                                              constants::max_request_integer};

/// One optional integer member of params: absent, or a raw JSON number token (never a string, null
/// or boolean) whose exact value lies in the parameter's range; anything else is INVALID_PARAMS.
std::optional<uint64_t> parse_integer_parameter(const rapidjson::Value& params, const envelope_preflight& preflight,
                                                const integer_parameter& parameter) {
   const auto member = params.FindMember(rapidjson::StringRef(parameter.field.data(), parameter.field.size()));
   if (member == params.MemberEnd())
      return std::nullopt;
   const auto value = preflight.numeric_parameters.contains(parameter.field) && member->value.IsString()
                         ? parse_integral(string_view(member->value), parameter.maximum)
                         : std::nullopt;
   if (!value || (value->negative && value->magnitude) || value->magnitude < parameter.minimum)
      throw query_error(error_kind::INVALID_PARAMS, std::string(parameter.field) + " must be an integer in [" +
                                                       std::to_string(parameter.minimum) + ", " +
                                                       std::to_string(parameter.maximum) + "]");
   return value->magnitude;
}

/// Validate params: only the request_field members, each at most once; `query` a non-empty string;
/// `limit` and `offset` integers in [0, 2^53-1]; `timeout_ms` an integer of at least one that may only
/// lower the configured `query-timeout-ms`. Every failure throws INVALID_PARAMS.
query_options parse_parameters(const rapidjson::Value& params, const envelope_preflight& preflight,
                               const query_config& config) {
   if (!params.IsObject())
      throw query_error(error_kind::INVALID_PARAMS, invalid_params_message);
   if (const auto violation = find_member_violation(params, request_field::all))
      throw query_error(error_kind::INVALID_PARAMS, *violation == member_violation::unknown
                                                       ? invalid_parameter_name_message
                                                       : duplicate_parameter_message);
   const auto query = params.FindMember(request_field::query);
   if (query == params.MemberEnd() || !preflight.string_parameters.contains(request_field::query) ||
       !query->value.IsString() || query->value.GetStringLength() == 0)
      throw query_error(error_kind::INVALID_PARAMS, invalid_params_message);
   query_options options;
   options.limit = parse_integer_parameter(params, preflight, limit_parameter);
   options.offset = parse_integer_parameter(params, preflight, offset_parameter).value_or(0);
   if (const auto milliseconds = parse_integer_parameter(params, preflight, timeout_parameter)) {
      if (*milliseconds > static_cast<uint64_t>(config.timeout.count()))
         throw query_error(error_kind::INVALID_PARAMS,
                           std::string(request_field::timeout_ms) + lowers_only_infix + option::timeout_ms,
                           std::nullopt, option::timeout_ms);
      options.timeout = std::chrono::milliseconds(*milliseconds);
   }
   return options;
}
} // namespace

query_request parse_request(std::string_view body, query_budget& budget) {
   if (body.size() > uint64_t(budget.config.max_query_bytes) * maximum_json_escape_bytes + envelope_allowance)
      throw query_error(error_kind::INVALID_REQUEST, "JSON-RPC envelope is too large");
   budget.charge_memory(body.size() * maximum_json_escape_bytes);
   rapidjson::Reader reader;
   rapidjson::MemoryStream input(body.data(), body.size());
   envelope_preflight preflight(budget);
   if (!reader.Parse<parse_flags>(input, preflight))
      throw query_error(error_kind::PARSE_ERROR, "Invalid JSON");
   rapidjson::Document document;
   document.Parse<parse_flags>(body.data(), body.size());
   if (document.HasParseError())
      throw query_error(error_kind::PARSE_ERROR, "Invalid JSON");
   if (!document.IsObject())
      throw query_error(error_kind::INVALID_REQUEST, invalid_envelope_message);
   if (find_member_violation(document, envelope_members))
      throw query_error(error_kind::INVALID_REQUEST, invalid_envelope_message);
   if (!document.HasMember(key_version) || !preflight.version_string || !document[key_version].IsString() ||
       string_view(document[key_version]) != constants::version || !document.HasMember(key_method) ||
       !preflight.method_string || !document[key_method].IsString())
      throw query_error(error_kind::INVALID_REQUEST, invalid_envelope_message);
   query_request request;
   request.notification = !document.HasMember(key_id);
   if (!request.notification) {
      const auto& id = document[key_id];
      if (id.IsNull())
         request.id = fc::variant();
      else if (preflight.id_number && id.IsString())
         request.id = parse_id(string_view(id));
      else if (id.IsString() && characters(string_view(id)) <= constants::max_id_characters)
         request.id = std::string(string_view(id));
      else
         throw query_error(error_kind::INVALID_REQUEST, invalid_envelope_message);
   }
   if (string_view(document[key_method]) != constants::method) {
      request.invocation_error.emplace(error_kind::METHOD_NOT_FOUND, "Method not found");
      return request;
   }
   if (!document.HasMember(key_params)) {
      request.invocation_error.emplace(error_kind::INVALID_PARAMS, invalid_params_message);
      return request;
   }
   const auto& params = document[key_params];
   // A params failure is an invocation error: the envelope is valid, so the response echoes its ID.
   try {
      request.options = parse_parameters(params, preflight, budget.config);
   } catch (const query_error& error) {
      request.invocation_error = error;
      return request;
   }
   const auto sql = string_view(params[request_field::query]);
   if (sql.size() > budget.config.max_query_bytes) {
      request.invocation_error.emplace(error_kind::QUERY_LIMIT, "Query resource limit exceeded", std::nullopt,
                                       option::max_query_bytes);
      return request;
   }
   budget.charge_memory(sql.size());
   request.query = sql;
   return request;
}

int error_code(error_kind kind) {
   switch (kind) {
   case error_kind::PARSE_ERROR:
      return -32700;
   case error_kind::INVALID_REQUEST:
      return -32600;
   case error_kind::METHOD_NOT_FOUND:
      return -32601;
   case error_kind::INVALID_PARAMS:
      return -32602;
   case error_kind::INTERNAL_ERROR:
      return -32603;
   case error_kind::QUERY_SYNTAX:
      return -32010;
   case error_kind::QUERY_SEMANTICS:
      return -32011;
   case error_kind::QUERY_LIMIT:
      return -32012;
   case error_kind::QUERY_TIMEOUT:
      return -32013;
   case error_kind::QUERY_BUSY:
      return -32014;
   case error_kind::SCHEMA_CHANGED:
      return -32015;
   case error_kind::ROW_DECODE_ERROR:
      return -32016;
   case error_kind::VALUE_ERROR:
      return -32017;
   case error_kind::STATE_UNAVAILABLE:
      return -32018;
   case error_kind::QUERY_CANCELLED:
      return -32019;
   }
   return -32603;
}

bool retryable(error_kind kind) {
   return kind == error_kind::QUERY_TIMEOUT || kind == error_kind::QUERY_BUSY || kind == error_kind::SCHEMA_CHANGED ||
          kind == error_kind::STATE_UNAVAILABLE || kind == error_kind::QUERY_CANCELLED;
}

fc::variant create_success(const query_request& request, query_result&& result) {
   return fc::mutable_variant_object()(key_version, constants::version)(key_id, request.id)(response_field::result,
                                                                                            result);
}

fc::variant create_error(const query_request* request, const query_error& error) {
   auto data = fc::mutable_variant_object()(response_field::kind, std::string(magic_enum::enum_name(error.kind)))(
      response_field::retryable, retryable(error.kind))(response_field::line,
                                                        error.span ? fc::variant(error.span->line) : fc::variant())(
      response_field::column, error.span ? fc::variant(error.span->column) : fc::variant())(
      response_field::limit, error.limit ? fc::variant(*error.limit) : fc::variant());
   return fc::mutable_variant_object()(key_version, constants::version)(key_id, request ? request->id : fc::variant())(
      response_field::error,
      fc::mutable_variant_object()(response_field::code, error_code(error.kind))(
         response_field::message, std::string(error.what()))(response_field::data, std::move(data)));
}
} // namespace sysio::query_engine
