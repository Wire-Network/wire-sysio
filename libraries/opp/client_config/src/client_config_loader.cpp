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

[[noreturn]] void throw_client_config_failure(client_config_reason       reason,
                                              std::string                field,
                                              std::string                observed,
                                              std::optional<std::string> allowed = std::nullopt) {
   throw client_config_exception(reason,
                                 std::move(field),
                                 std::move(observed),
                                 std::move(allowed));
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
