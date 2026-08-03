#include <sysio/opp/config/client_config_loader.hpp>

#include <fc/network/ethereum/ethereum_transaction_policy.hpp>
#include <fc/network/http/http_client.hpp>
#include <fc/network/url.hpp>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include <magic_enum/magic_enum.hpp>
#include <rapidjson/document.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>

#include <fstream>
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sysio::opp::config {

namespace {

constexpr uint32_t supported_schema_version = 1;

struct raw_validation_failure {
   client_config_reason       reason;
   std::string                field;
   std::string                observed;
   std::optional<std::string> allowed;
};

[[noreturn]] void throw_client_config_failure(client_config_reason       reason,
                                              std::string                field,
                                              std::string                observed,
                                              std::optional<std::string> allowed = std::nullopt) {
   throw client_config_exception(reason,
                                 std::move(field),
                                 std::move(observed),
                                 std::move(allowed));
}

[[noreturn]] void throw_client_config_failure(raw_validation_failure failure) {
   throw_client_config_failure(failure.reason,
                               std::move(failure.field),
                               std::move(failure.observed),
                               std::move(failure.allowed));
}

/** Read a complete configuration document without exposing its path in failures. */
std::string read_bounded_configuration_file(const std::filesystem::path& configuration_file) {
   std::ifstream input(configuration_file, std::ios::binary | std::ios::ate);
   if (!input.is_open()) {
      throw_client_config_failure(client_config_reason::file_unreadable,
                                  "configuration_file",
                                  "<unreadable>");
   }

   const auto end = input.tellg();
   if (end < 0) {
      throw_client_config_failure(client_config_reason::file_unreadable,
                                  "configuration_file",
                                  "<unreadable>");
   }
   const auto size = static_cast<std::uintmax_t>(end);
   if (size > max_client_configuration_file_size) {
      throw_client_config_failure(client_config_reason::file_too_large,
                                  "configuration_file",
                                  std::to_string(size),
                                  std::to_string(max_client_configuration_file_size));
   }

   std::string contents(static_cast<std::size_t>(size), '\0');
   input.seekg(0);
   if (!contents.empty() && !input.read(contents.data(), static_cast<std::streamsize>(contents.size()))) {
      throw_client_config_failure(client_config_reason::file_unreadable,
                                  "configuration_file",
                                  "<unreadable>");
   }
   return contents;
}

/** Streaming structural guard that retains duplicate keys before a DOM can collapse them. */
class raw_json_safety_handler
   : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, raw_json_safety_handler> {
public:
   bool Null() {
      return fail(client_config_reason::null_value, "document", "<null>");
   }

   bool Bool(bool) { return accept_scalar_root(); }
   bool Int(int) { return accept_scalar_root(); }
   bool Uint(unsigned) { return accept_scalar_root(); }
   bool Int64(std::int64_t) { return accept_scalar_root(); }
   bool Uint64(std::uint64_t) { return accept_scalar_root(); }
   bool Double(double) { return accept_scalar_root(); }

   bool RawNumber(const char* value, rapidjson::SizeType length, bool) {
      _number_tokens.emplace_back(value, length);
      return accept_scalar_root();
   }
   bool String(const char*, rapidjson::SizeType, bool) { return accept_scalar_root(); }

   bool StartObject() {
      if (_scopes.empty()) {
         if (_root_seen) return fail(client_config_reason::json_invalid, "root", "<multiple-values>");
         _root_seen = true;
      }
      if (_scopes.size() >= max_client_configuration_nesting_depth) {
         return fail(client_config_reason::nesting_depth_exceeded,
                     "document",
                     std::to_string(_scopes.size() + 1),
                     std::to_string(max_client_configuration_nesting_depth));
      }
      _scopes.emplace_back(std::set<std::string>{});
      return true;
   }

   bool Key(const char* value, rapidjson::SizeType length, bool) {
      if (_scopes.empty() || !_scopes.back()) {
         return fail(client_config_reason::json_invalid, "document", "<invalid-object-key>");
      }
      if (!_scopes.back()->emplace(value, length).second) {
         return fail(client_config_reason::duplicate_field, "document", "<duplicate-field>");
      }
      return true;
   }

   bool EndObject(rapidjson::SizeType) {
      if (_scopes.empty() || !_scopes.back()) {
         return fail(client_config_reason::json_invalid, "document", "<invalid-object>");
      }
      _scopes.pop_back();
      return true;
   }

   bool StartArray() {
      if (_scopes.empty()) {
         _root_seen = true;
         return fail(client_config_reason::root_type_invalid, "root", "<non-object>");
      }
      if (_scopes.size() >= max_client_configuration_nesting_depth) {
         return fail(client_config_reason::nesting_depth_exceeded,
                     "document",
                     std::to_string(_scopes.size() + 1),
                     std::to_string(max_client_configuration_nesting_depth));
      }
      _scopes.emplace_back(std::nullopt);
      return true;
   }

   bool EndArray(rapidjson::SizeType) {
      if (_scopes.empty() || _scopes.back()) {
         return fail(client_config_reason::json_invalid, "document", "<invalid-array>");
      }
      _scopes.pop_back();
      return true;
   }

   const std::optional<raw_validation_failure>& failure() const { return _failure; }
   const std::vector<std::string>& number_tokens() const { return _number_tokens; }

private:
   bool accept_scalar_root() {
      if (!_scopes.empty()) return true;
      _root_seen = true;
      return fail(client_config_reason::root_type_invalid, "root", "<non-object>");
   }

   bool fail(client_config_reason       reason,
             std::string                field,
             std::string                observed,
             std::optional<std::string> allowed = std::nullopt) {
      if (!_failure) {
         _failure = raw_validation_failure{
            reason,
            std::move(field),
            std::move(observed),
            std::move(allowed),
         };
      }
      return false;
   }

   bool                                                   _root_seen = false;
   std::vector<std::optional<std::set<std::string>>>      _scopes;
   std::optional<raw_validation_failure>                  _failure;
   std::vector<std::string>                               _number_tokens;
};

/** Resolve either a proto field name or its canonical ProtoJSON alias. */
const google::protobuf::FieldDescriptor*
find_json_field(const google::protobuf::Descriptor& descriptor, std::string_view name) {
   if (const auto* field = descriptor.FindFieldByName(std::string(name))) return field;
   for (int index = 0; index < descriptor.field_count(); ++index) {
      const auto* field = descriptor.field(index);
      if (field->json_name() == name) return field;
   }
   return nullptr;
}

std::string child_field_path(std::string_view parent,
                             const google::protobuf::FieldDescriptor& field) {
   const std::string field_name(field.name().data(), field.name().size());
   if (parent.empty()) return field_name;
   return std::string(parent) + "." + field_name;
}

void validate_json_value(const rapidjson::Value&                    value,
                         const google::protobuf::FieldDescriptor& field,
                         std::string_view                          path,
                         const std::vector<std::string>&           number_tokens,
                         std::size_t&                              number_index);

/** Validate a JSON object from the generated protobuf descriptor rather than a parallel schema. */
void validate_json_message(const rapidjson::Value&              value,
                           const google::protobuf::Descriptor& descriptor,
                           std::string_view                    path,
                           const std::vector<std::string>&     number_tokens,
                           std::size_t&                        number_index) {
   if (!value.IsObject()) {
      throw_client_config_failure(client_config_reason::field_type_invalid,
                                  path.empty() ? "root" : std::string(path),
                                  "<non-object>");
   }

   std::set<const google::protobuf::FieldDescriptor*> observed_fields;
   for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
      const std::string_view member_name{member->name.GetString(), member->name.GetStringLength()};
      const auto* field = find_json_field(descriptor, member_name);
      if (!field) {
         throw_client_config_failure(client_config_reason::unknown_field,
                                     path.empty() ? "root" : std::string(path),
                                     "<unknown-field>");
      }
      const auto field_path = child_field_path(path, *field);
      if (!observed_fields.insert(field).second) {
         throw_client_config_failure(client_config_reason::duplicate_field,
                                     field_path,
                                     "<duplicate-field>");
      }

      if (field->is_repeated()) {
         if (!member->value.IsArray()) {
            throw_client_config_failure(client_config_reason::field_type_invalid,
                                        field_path,
                                        "<non-array>");
         }
         for (const auto& element : member->value.GetArray()) {
            validate_json_value(element, *field, field_path, number_tokens, number_index);
         }
      } else {
         validate_json_value(member->value, *field, field_path, number_tokens, number_index);
      }
   }
}

void validate_json_value(const rapidjson::Value&                    value,
                         const google::protobuf::FieldDescriptor& field,
                         std::string_view                          path,
                         const std::vector<std::string>&           number_tokens,
                         std::size_t&                              number_index) {
   const std::string* number_token = nullptr;
   if (value.IsNumber()) {
      if (number_index >= number_tokens.size()) {
         throw_client_config_failure(client_config_reason::json_invalid,
                                     "document",
                                     "<numeric-token-mismatch>");
      }
      number_token = &number_tokens[number_index++];
   }

   using field_descriptor = google::protobuf::FieldDescriptor;
   switch (field.type()) {
   case field_descriptor::TYPE_MESSAGE:
      validate_json_message(value, *field.message_type(), path, number_tokens, number_index);
      return;
   case field_descriptor::TYPE_STRING:
   case field_descriptor::TYPE_BYTES:
      if (value.IsString()) return;
      break;
   case field_descriptor::TYPE_BOOL:
      if (value.IsBool()) return;
      break;
   case field_descriptor::TYPE_UINT32:
   case field_descriptor::TYPE_FIXED32:
      if (value.IsUint() && number_token && !number_token->empty() &&
          (number_token->size() == 1 || number_token->front() != '0') &&
          std::ranges::all_of(*number_token, [](unsigned char character) {
             return character >= '0' && character <= '9';
          })) {
         return;
      }
      throw_client_config_failure(client_config_reason::numeric_token_invalid,
                                  std::string(path),
                                  "<noncanonical-unsigned-integer>");
   case field_descriptor::TYPE_INT32:
   case field_descriptor::TYPE_SINT32:
   case field_descriptor::TYPE_SFIXED32:
      if (value.IsInt()) return;
      break;
   case field_descriptor::TYPE_UINT64:
   case field_descriptor::TYPE_FIXED64:
      if (value.IsUint64() || value.IsString()) return;
      break;
   case field_descriptor::TYPE_INT64:
   case field_descriptor::TYPE_SINT64:
   case field_descriptor::TYPE_SFIXED64:
      if (value.IsInt64() || value.IsString()) return;
      break;
   case field_descriptor::TYPE_FLOAT:
   case field_descriptor::TYPE_DOUBLE:
      if (value.IsNumber() || value.IsString()) return;
      break;
   case field_descriptor::TYPE_ENUM:
      if (value.IsString() || value.IsInt()) return;
      break;
   case field_descriptor::TYPE_GROUP:
      break;
   }

   throw_client_config_failure(client_config_reason::field_type_invalid,
                               std::string(path),
                               "<wrong-type>");
}

/** Run the duplicate/null/depth guard before constructing a JSON DOM. */
std::vector<std::string> validate_raw_json_safety(std::string_view json) {
   rapidjson::Reader reader;
   rapidjson::MemoryStream stream(json.data(), json.size());
   raw_json_safety_handler handler;
   const bool valid = reader.Parse<rapidjson::kParseValidateEncodingFlag |
                                   rapidjson::kParseNumbersAsStringsFlag>(stream, handler);
   if (!valid || stream.Tell() != json.size()) {
      if (handler.failure()) throw_client_config_failure(*handler.failure());
      throw_client_config_failure(client_config_reason::json_invalid, "document", "<invalid-json>");
   }
   return handler.number_tokens();
}

fc::uint256 parse_policy_value(std::string_view value, std::string_view field) {
   try {
      return fc::network::ethereum::parse_canonical_uint256_decimal(value, field);
   } catch (const fc::network::ethereum::ethereum_transaction_policy_exception& rejection) {
      throw_client_config_failure(client_config_reason::policy_value_invalid,
                                  rejection.field(),
                                  rejection.observed(),
                                  rejection.allowed());
   }
}

void validate_rpc_url(std::string_view raw_url) {
   if (raw_url.contains('#')) {
      throw_client_config_failure(client_config_reason::rpc_url_invalid,
                                  "rpc_url",
                                  "<invalid>");
   }
   try {
      const fc::url parsed{std::string(raw_url)};
      const bool supported_scheme = parsed.proto() == "http" || parsed.proto() == "https";
      if (supported_scheme && parsed.host() && fc::http::is_safe_network_host(*parsed.host())) {
         return;
      }
   } catch (const std::exception&) {
   }
   throw_client_config_failure(client_config_reason::rpc_url_invalid, "rpc_url", "<invalid>");
}

} // namespace

std::string_view client_config_reason_name(client_config_reason reason) {
   return magic_enum::enum_name(reason);
}

client_config_exception::client_config_exception(client_config_reason       reason,
                                                 std::string                field,
                                                 std::string                observed,
                                                 std::optional<std::string> allowed)
   : fc::exception(FC_LOG_MESSAGE(error,
                                  "reason_code={} field={} observed={} allowed={}",
                                  client_config_reason_name(reason),
                                  field,
                                  observed,
                                  allowed.value_or("n/a")),
                   fc::invalid_arg_exception_code,
                   "client_config_exception",
                   "Client configuration rejected")
   , _reason(reason)
   , _field(std::move(field))
   , _observed(std::move(observed))
   , _allowed(std::move(allowed)) {}

std::shared_ptr<fc::exception> client_config_exception::dynamic_copy_exception() const {
   return std::make_shared<client_config_exception>(*this);
}

void client_config_exception::rethrow() const {
   throw *this;
}

void load_client_configuration_json(const std::filesystem::path& configuration_file,
                                    google::protobuf::Message&   destination) {
   const auto contents = read_bounded_configuration_file(configuration_file);
   const auto number_tokens = validate_raw_json_safety(contents);

   rapidjson::Document document;
   document.Parse<rapidjson::kParseValidateEncodingFlag |
                  rapidjson::kParseFullPrecisionFlag>(contents.data(), contents.size());
   if (document.HasParseError()) {
      throw_client_config_failure(client_config_reason::json_invalid, "document", "<invalid-json>");
   }
   if (!document.IsObject()) {
      throw_client_config_failure(client_config_reason::root_type_invalid, "root", "<non-object>");
   }

   std::size_t number_index = 0;
   validate_json_message(document, *destination.GetDescriptor(), "", number_tokens, number_index);
   if (number_index != number_tokens.size()) {
      throw_client_config_failure(client_config_reason::json_invalid,
                                  "document",
                                  "<numeric-token-mismatch>");
   }

   destination.Clear();
   google::protobuf::util::JsonParseOptions options;
   options.ignore_unknown_fields = false;
   const auto status = google::protobuf::util::JsonStringToMessage(contents, &destination, options);
   if (!status.ok()) {
      throw_client_config_failure(client_config_reason::proto_json_invalid,
                                  "document",
                                  "<invalid-proto-json>");
   }
}

void validate_evm_client_configuration(const EvmClientConfigurationFile& configuration) {
   if (!configuration.has_schema_version() ||
       configuration.schema_version() != supported_schema_version) {
      throw_client_config_failure(client_config_reason::schema_version_unsupported,
                                  "schema_version",
                                  configuration.has_schema_version()
                                     ? std::to_string(configuration.schema_version())
                                     : "<missing>",
                                  std::to_string(supported_schema_version));
   }
   if (configuration.clients().empty()) {
      throw_client_config_failure(client_config_reason::client_missing, "clients", "<empty>");
   }

   std::set<std::string> client_ids;
   for (const auto& client : configuration.clients()) {
      if (!client.has_connection()) {
         throw_client_config_failure(client_config_reason::connection_missing,
                                     "connection",
                                     "<missing>");
      }
      const auto& connection = client.connection();
      if (!connection.has_client_id() ||
          !fc::network::ethereum::is_safe_transaction_policy_identifier(connection.client_id())) {
         throw_client_config_failure(client_config_reason::client_identifier_invalid,
                                     "client_id",
                                     "<invalid>");
      }
      if (!client_ids.insert(connection.client_id()).second) {
         throw_client_config_failure(client_config_reason::client_duplicate,
                                     "client_id",
                                     connection.client_id());
      }
      if (!connection.has_signature_provider_id() || connection.signature_provider_id().empty()) {
         throw_client_config_failure(client_config_reason::signature_provider_identifier_invalid,
                                     "signature_provider_id",
                                     "<invalid>");
      }
      if (!connection.has_rpc_url() || connection.rpc_url().empty()) {
         throw_client_config_failure(client_config_reason::rpc_url_invalid,
                                     "rpc_url",
                                     "<invalid>");
      }
      validate_rpc_url(connection.rpc_url());

      if (!client.has_chain_id() || client.chain_id() == 0) {
         throw_client_config_failure(client_config_reason::chain_id_invalid,
                                     "chain_id",
                                     client.has_chain_id() ? "0" : "<missing>",
                                     "1..UINT32_MAX");
      }

      if (!client.has_transaction_policy()) continue;
      const auto& policy = client.transaction_policy();
      const bool complete =
         policy.has_max_priority_fee_per_gas_wei() &&
         !policy.max_priority_fee_per_gas_wei().empty() &&
         policy.has_max_fee_per_gas_wei() &&
         !policy.max_fee_per_gas_wei().empty() &&
         policy.has_max_gas_limit() &&
         !policy.max_gas_limit().empty() &&
         policy.has_max_total_native_cost_wei() &&
         !policy.max_total_native_cost_wei().empty();
      if (!complete) {
         throw_client_config_failure(client_config_reason::policy_incomplete,
                                     "transaction_policy",
                                     "<incomplete>");
      }

      const auto maximum_priority_fee = parse_policy_value(
         policy.max_priority_fee_per_gas_wei(), "max_priority_fee_per_gas_wei");
      const auto maximum_fee = parse_policy_value(
         policy.max_fee_per_gas_wei(), "max_fee_per_gas_wei");
      (void) parse_policy_value(policy.max_gas_limit(), "max_gas_limit");
      (void) parse_policy_value(policy.max_total_native_cost_wei(), "max_total_native_cost_wei");
      if (maximum_priority_fee > maximum_fee) {
         throw_client_config_failure(client_config_reason::policy_fee_relationship_invalid,
                                     "max_priority_fee_per_gas_wei",
                                     maximum_priority_fee.str(),
                                     maximum_fee.str());
      }
   }
}

EvmClientConfigurationFile
load_evm_client_configuration_file(const std::filesystem::path& configuration_file) {
   EvmClientConfigurationFile configuration;
   load_client_configuration_json(configuration_file, configuration);
   validate_evm_client_configuration(configuration);
   return configuration;
}

} // namespace sysio::opp::config
