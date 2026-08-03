#pragma once

#include <fc/exception/exception.hpp>

#include <sysio/opp/config/client_config.pb.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace google::protobuf {
class Message;
}

namespace sysio::opp::config {

/** Maximum accepted size of a host client-configuration JSON document. */
inline constexpr std::size_t max_client_configuration_file_size = 1024 * 1024;

/** Maximum accepted object/array nesting depth in a host client-configuration document. */
inline constexpr std::size_t max_client_configuration_nesting_depth = 32;

/** Stable reason codes for sanitized client-configuration failures. */
enum class client_config_reason {
   file_unreadable,
   file_too_large,
   json_invalid,
   root_type_invalid,
   nesting_depth_exceeded,
   duplicate_field,
   null_value,
   unknown_field,
   field_type_invalid,
   numeric_token_invalid,
   proto_json_invalid,
   schema_version_unsupported,
   client_missing,
   connection_missing,
   client_identifier_invalid,
   signature_provider_identifier_invalid,
   rpc_url_invalid,
   chain_id_invalid,
   client_duplicate,
   policy_incomplete,
   policy_value_invalid,
   policy_fee_relationship_invalid,
};

/** Return the stable log spelling of a client-configuration reason. */
std::string_view client_config_reason_name(client_config_reason reason);

/** Sanitized structured failure raised while loading host client configuration. */
class client_config_exception : public fc::exception {
public:
   /** Construct a client-configuration failure without embedding raw document content. */
   client_config_exception(client_config_reason       reason,
                           std::string                field,
                           std::string                observed,
                           std::optional<std::string> allowed = std::nullopt);

   /** Copy this exception without slicing its structured fields. */
   std::shared_ptr<fc::exception> dynamic_copy_exception() const override;

   /** Rethrow this exception while retaining its dynamic type. */
   void rethrow() const override;

   /** Stable machine-readable rejection reason. */
   client_config_reason reason() const { return _reason; }

   /** Schema field associated with the rejection. */
   const std::string& field() const { return _field; }

   /** Sanitized observed value. */
   const std::string& observed() const { return _observed; }

   /** Sanitized accepted value or range, when applicable. */
   const std::optional<std::string>& allowed() const { return _allowed; }

private:
   client_config_reason       _reason;
   std::string                _field;
   std::string                _observed;
   std::optional<std::string> _allowed;
};

/**
 * Load raw JSON into a generated protobuf message after bounded structural and descriptor-driven validation.
 *
 * The raw pass rejects duplicate keys, duplicate protobuf aliases, explicit nulls, unknown fields, invalid JSON
 * types, noncanonical uint32 tokens, excessive size, and excessive nesting before ProtoJSON conversion.
 */
void load_client_configuration_json(const std::filesystem::path& configuration_file,
                                    google::protobuf::Message&   destination);

/** Validate semantic invariants of a generated Ethereum client configuration. */
void validate_evm_client_configuration(const EvmClientConfigurationFile& configuration);

/** Load and semantically validate a version-1 Ethereum client configuration file. */
EvmClientConfigurationFile
load_evm_client_configuration_file(const std::filesystem::path& configuration_file);

} // namespace sysio::opp::config
