#include <fc/variant_object.hpp>
#include <sysio/query_engine_plugin/query.hpp>

#include <magic_enum/magic_enum.hpp>
#include <rapidjson/document.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>

#include <charconv>
#include <set>

namespace sysio::query_engine_plugin {
namespace {
constexpr uint64_t maximum_json_escape_bytes = 6;
constexpr uint64_t envelope_allowance = 4096;
constexpr auto key_version = "jsonrpc";
constexpr auto key_id = "id";
constexpr auto key_method = "method";
constexpr auto key_params = "params";
constexpr auto key_query = "query";
constexpr auto invalid_envelope_message = "Invalid JSON-RPC request";
constexpr auto invalid_params_message = "Expected named query parameter";
constexpr uint32_t parse_flags =
   rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag | rapidjson::kParseIterativeFlag;

/// Preflight bounds JSON nesting and retains token kinds without ever parsing a host float.
struct envelope_preflight : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, envelope_preflight> {
   query_budget& budget;
   uint32_t depth = 0;
   std::string root_key;
   std::string parameter_key;
   bool version_string = false, method_string = false, id_number = false, query_string = false;
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
      if (depth == 2 && root_key == key_params && parameter_key == key_query)
         query_string = true;
      return true;
   }
   bool RawNumber(const char*, rapidjson::SizeType, bool) {
      if (depth == 1 && root_key == key_id)
         id_number = true;
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

/// Count validated UTF-8 code points, the JSON Schema maxLength unit.
size_t characters(std::string_view text) {
   size_t result = 0;
   constexpr unsigned char continuation_mask = 0xc0, continuation_tag = 0x80;
   for (unsigned char byte : text)
      if ((byte & continuation_mask) != continuation_tag)
         ++result;
   return result;
}

/// Accept integral JSON numbers, including exponent notation, with exact lexical arithmetic.
int64_t parse_id(std::string_view number) {
   const bool negative = number.starts_with('-');
   if (negative)
      number.remove_prefix(1);
   const auto exponent_start = number.find_first_of("eE");
   int32_t exponent = 0;
   if (exponent_start != std::string_view::npos) {
      auto text = number.substr(exponent_start + 1);
      if (text.starts_with('+'))
         text.remove_prefix(1);
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), exponent);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
         throw query_error(error_kind::INVALID_REQUEST, invalid_envelope_message);
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
      return 0;
   digits.erase(0, first);
   scale -= exponent;
   while (scale > 0 && !digits.empty() && digits.back() == '0') {
      digits.pop_back();
      --scale;
   }
   constexpr size_t max_digits = 10;
   if (scale > 0 || scale < -int64_t(max_digits) || digits.size() + size_t(-scale) > max_digits)
      throw query_error(error_kind::INVALID_REQUEST, invalid_envelope_message);
   digits.append(size_t(-scale), '0');
   int64_t result = 0;
   for (char character : digits)
      result = result * constants::decimal_base + character - '0';
   if (result > constants::max_numeric_id)
      throw query_error(error_kind::INVALID_REQUEST, invalid_envelope_message);
   return negative ? -result : result;
}

/// Preserve embedded NUL bytes so validation never accepts a truncated spelling.
std::string_view string_view(const rapidjson::Value& value) {
   return {value.GetString(), value.GetStringLength()};
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
   std::set<std::string> members;
   for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
      const std::string name(member->name.GetString(), member->name.GetStringLength());
      if ((name != key_version && name != key_id && name != key_method && name != key_params) ||
          !members.insert(name).second)
         throw query_error(error_kind::INVALID_REQUEST, invalid_envelope_message);
   }
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
   if (!document.HasMember(key_params) || !document[key_params].IsObject() || document[key_params].MemberCount() != 1 ||
       !document[key_params].HasMember(key_query) || !preflight.query_string ||
       !document[key_params][key_query].IsString() || document[key_params][key_query].GetStringLength() == 0) {
      request.invocation_error.emplace(error_kind::INVALID_PARAMS, invalid_params_message);
      return request;
   }
   const auto sql = string_view(document[key_params][key_query]);
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
} // namespace sysio::query_engine_plugin
