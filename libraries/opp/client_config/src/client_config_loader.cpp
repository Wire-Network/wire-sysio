#include <sysio/opp/config/client_config_loader.hpp>

#include <fc/network/ethereum/ethereum_transaction_policy.hpp>
#include <fc/network/http/http_client.hpp>
#include <fc/network/url.hpp>

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include <magic_enum/magic_enum.hpp>

#include <fstream>
#include <set>
#include <string>
#include <utility>

namespace sysio::opp::config {

namespace {

constexpr uint32_t supported_schema_version = 1;

namespace configuration_field {
constexpr std::string_view configuration_file = "configuration_file";
constexpr std::string_view document = "document";
constexpr std::string_view schema_version = "schema_version";
constexpr std::string_view clients = "clients";
constexpr std::string_view connection = "connection";
constexpr std::string_view client_id = "client_id";
constexpr std::string_view signature_provider_id = "signature_provider_id";
constexpr std::string_view rpc_url = "rpc_url";
constexpr std::string_view chain_id = "chain_id";
constexpr std::string_view transaction_policy = "transaction_policy";
constexpr std::string_view max_priority_fee_per_gas_wei = "max_priority_fee_per_gas_wei";
constexpr std::string_view max_fee_per_gas_wei = "max_fee_per_gas_wei";
constexpr std::string_view max_gas_limit = "max_gas_limit";
constexpr std::string_view max_total_native_cost_wei = "max_total_native_cost_wei";
} // namespace configuration_field

namespace diagnostic_observation {
constexpr auto unreadable = "<unreadable>";
constexpr auto invalid = "<invalid>";
constexpr auto invalid_proto_json = "<invalid-proto-json>";
constexpr auto missing = "<missing>";
constexpr auto empty = "<empty>";
constexpr auto incomplete = "<incomplete>";
constexpr auto zero = "0";
} // namespace diagnostic_observation

constexpr std::string_view http_url_scheme = "http";
constexpr std::string_view https_url_scheme = "https";
constexpr auto positive_uint32_range = "1..UINT32_MAX";
constexpr auto unavailable_allowed_value = "n/a";

/** Throw a structured configuration rejection while centralizing string ownership conversion. */
[[noreturn]] void throw_client_config_failure(client_config_reason       reason,
                                              std::string_view           field,
                                              std::string                observed,
                                              std::optional<std::string> allowed = std::nullopt) {
   throw client_config_exception(reason,
                                 std::string(field),
                                 std::move(observed),
                                 std::move(allowed));
}

/** Read a complete configuration document without exposing its path in failures. */
std::string read_bounded_configuration_file(const std::filesystem::path& configuration_file) {
   std::ifstream input(configuration_file, std::ios::binary | std::ios::ate);
   if (!input.is_open()) {
      throw_client_config_failure(client_config_reason::file_unreadable,
                                  configuration_field::configuration_file,
                                  diagnostic_observation::unreadable);
   }

   const auto end = input.tellg();
   if (end < 0) {
      throw_client_config_failure(client_config_reason::file_unreadable,
                                  configuration_field::configuration_file,
                                  diagnostic_observation::unreadable);
   }
   const auto size = static_cast<std::uintmax_t>(end);
   if (size > max_client_configuration_file_size) {
      throw_client_config_failure(client_config_reason::file_too_large,
                                  configuration_field::configuration_file,
                                  std::to_string(size),
                                  std::to_string(max_client_configuration_file_size));
   }

   std::string contents(static_cast<std::size_t>(size), '\0');
   input.seekg(0);
   if (!contents.empty() && !input.read(contents.data(), static_cast<std::streamsize>(contents.size()))) {
      throw_client_config_failure(client_config_reason::file_unreadable,
                                  configuration_field::configuration_file,
                                  diagnostic_observation::unreadable);
   }
   return contents;
}

/** Parse a configured policy cap and translate failures to the configuration error vocabulary. */
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

/** Validate an RPC URL without reflecting credentials, query parameters, or fragments. */
void validate_rpc_url(std::string_view raw_url) {
   if (raw_url.contains('#')) {
      throw_client_config_failure(client_config_reason::rpc_url_invalid,
                                  configuration_field::rpc_url,
                                  diagnostic_observation::invalid);
   }
   try {
      const fc::url parsed{std::string(raw_url)};
      const bool supported_scheme = parsed.proto() == http_url_scheme || parsed.proto() == https_url_scheme;
      if (supported_scheme && parsed.host() && fc::http::is_safe_network_host(*parsed.host())) {
         return;
      }
   } catch (const std::exception&) {
   }
   throw_client_config_failure(client_config_reason::rpc_url_invalid,
                               configuration_field::rpc_url,
                               diagnostic_observation::invalid);
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
                                  allowed.value_or(unavailable_allowed_value)),
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

   destination.Clear();
   google::protobuf::util::JsonParseOptions options;
   options.ignore_unknown_fields = false;
   const auto status = google::protobuf::util::JsonStringToMessage(contents, &destination, options);
   if (!status.ok()) {
      throw_client_config_failure(client_config_reason::proto_json_invalid,
                                  configuration_field::document,
                                  diagnostic_observation::invalid_proto_json);
   }
}

void validate_evm_client_configuration(const EvmClientConfigurationFile& configuration) {
   if (!configuration.has_schema_version() ||
       configuration.schema_version() != supported_schema_version) {
      throw_client_config_failure(client_config_reason::schema_version_unsupported,
                                  configuration_field::schema_version,
                                  configuration.has_schema_version()
                                     ? std::to_string(configuration.schema_version())
                                     : diagnostic_observation::missing,
                                  std::to_string(supported_schema_version));
   }
   if (configuration.clients().empty()) {
      throw_client_config_failure(client_config_reason::client_missing,
                                  configuration_field::clients,
                                  diagnostic_observation::empty);
   }

   std::set<std::string> client_ids;
   for (const auto& client : configuration.clients()) {
      if (!client.has_connection()) {
         throw_client_config_failure(client_config_reason::connection_missing,
                                     configuration_field::connection,
                                     diagnostic_observation::missing);
      }
      const auto& connection = client.connection();
      if (!connection.has_client_id() ||
          !fc::network::ethereum::is_safe_transaction_policy_identifier(connection.client_id())) {
         throw_client_config_failure(client_config_reason::client_identifier_invalid,
                                     configuration_field::client_id,
                                     diagnostic_observation::invalid);
      }
      if (!client_ids.insert(connection.client_id()).second) {
         throw_client_config_failure(client_config_reason::client_duplicate,
                                     configuration_field::client_id,
                                     connection.client_id());
      }
      if (!connection.has_signature_provider_id() || connection.signature_provider_id().empty()) {
         throw_client_config_failure(client_config_reason::signature_provider_identifier_invalid,
                                     configuration_field::signature_provider_id,
                                     diagnostic_observation::invalid);
      }
      if (!connection.has_rpc_url() || connection.rpc_url().empty()) {
         throw_client_config_failure(client_config_reason::rpc_url_invalid,
                                     configuration_field::rpc_url,
                                     diagnostic_observation::invalid);
      }
      validate_rpc_url(connection.rpc_url());

      if (!client.has_chain_id() || client.chain_id() == 0) {
         throw_client_config_failure(client_config_reason::chain_id_invalid,
                                     configuration_field::chain_id,
                                     client.has_chain_id() ? diagnostic_observation::zero
                                                           : diagnostic_observation::missing,
                                     positive_uint32_range);
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
                                     configuration_field::transaction_policy,
                                     diagnostic_observation::incomplete);
      }

      const auto maximum_priority_fee = parse_policy_value(
         policy.max_priority_fee_per_gas_wei(), configuration_field::max_priority_fee_per_gas_wei);
      const auto maximum_fee = parse_policy_value(
         policy.max_fee_per_gas_wei(), configuration_field::max_fee_per_gas_wei);
      (void) parse_policy_value(policy.max_gas_limit(), configuration_field::max_gas_limit);
      (void) parse_policy_value(policy.max_total_native_cost_wei(),
                                configuration_field::max_total_native_cost_wei);
      if (maximum_priority_fee > maximum_fee) {
         throw_client_config_failure(client_config_reason::policy_fee_relationship_invalid,
                                     configuration_field::max_priority_fee_per_gas_wei,
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
