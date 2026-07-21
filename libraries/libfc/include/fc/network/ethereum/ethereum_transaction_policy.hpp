#pragma once

#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>

#include <magic_enum/magic_enum.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace fc::network::ethereum {

/** Stable reason codes emitted when Ethereum transaction policy validation fails. */
enum class ethereum_transaction_policy_reason {
   configuration_file_unreadable,
   configuration_schema_invalid,
   configuration_version_unsupported,
   configuration_client_missing,
   configuration_client_duplicate,
   configuration_client_unknown,
   configuration_chain_id_mismatch,
   configuration_value_invalid,
   rpc_quantity_invalid,
   rpc_quantity_out_of_range,
   max_fee_derivation_overflow,
   gas_limit_derivation_overflow,
   fee_relationship_invalid,
   chain_id_mismatch,
   priority_fee_cap_exceeded,
   max_fee_cap_exceeded,
   gas_limit_cap_exceeded,
   total_cost_multiplication_overflow,
   total_cost_addition_overflow,
   total_cost_cap_exceeded
};

/** Return the stable wire/log spelling for a transaction-policy reason. */
inline std::string_view reason_code_name(ethereum_transaction_policy_reason reason) {
   return magic_enum::enum_name(reason);
}

/** Return whether an identifier is bounded and ASCII-safe for policy logs and lookups. */
bool is_safe_transaction_policy_identifier(std::string_view identifier);

/**
 * Exception carrying structured, non-sensitive Ethereum transaction-policy rejection details.
 *
 * The reason enum is the stable machine-readable code. Field, observed, and allowed values are
 * intentionally strings so arithmetic-overflow diagnostics can describe both operands without
 * performing another potentially unsafe calculation.
 */
class ethereum_transaction_policy_exception : public fc::exception {
public:
   /** Construct a structured transaction-policy rejection. */
   ethereum_transaction_policy_exception(ethereum_transaction_policy_reason reason,
                                         std::string                        field,
                                         std::string                        observed,
                                         std::optional<std::string>         allowed = std::nullopt);

   /** Copy this exception without slicing its structured fields. */
   std::shared_ptr<fc::exception> dynamic_copy_exception() const override;

   /** Rethrow this exception while preserving its dynamic type. */
   void rethrow() const override;

   /** Stable rejection reason. */
   ethereum_transaction_policy_reason reason() const { return _reason; }

   /** Transaction field or configuration field associated with the rejection. */
   const std::string& field() const { return _field; }

   /** Sanitized observed value or arithmetic operands. */
   const std::string& observed() const { return _observed; }

   /** Sanitized configured limit, when the rejection has one. */
   const std::optional<std::string>& allowed() const { return _allowed; }

private:
   ethereum_transaction_policy_reason _reason;
   std::string                        _field;
   std::string                        _observed;
   std::optional<std::string>         _allowed;
};

/** Throw a structured transaction-policy exception from shared library or plugin validation. */
[[noreturn]] void throw_transaction_policy_exception(
   ethereum_transaction_policy_reason reason,
   std::string_view                    field,
   std::string                         observed,
   std::optional<std::string>          allowed = std::nullopt);

/** Immutable local expenditure policy for one configured Ethereum client and uint32 outpost chain. */
struct ethereum_transaction_policy {
   std::string client_id;
   uint32_t chain_id;
   fc::uint256 max_priority_fee_per_gas;
   fc::uint256 max_fee_per_gas;
   fc::uint256 max_gas_limit;
   fc::uint256 max_total_native_cost;
};

/** Return the maximum value accepted by any uint256 transaction-policy cap. */
const fc::uint256& maximum_ethereum_transaction_policy_value();

/**
 * Parse a canonical unsigned decimal uint256 configuration value.
 *
 * Canonical values contain only ASCII decimal digits and have no leading zeroes. Zero is accepted
 * only when `allow_zero` is true. Values wider than uint256 are rejected before conversion.
 */
fc::uint256 parse_canonical_uint256_decimal(std::string_view value,
                                            std::string_view field,
                                            bool             allow_zero = false);

/**
 * Parse a canonical Ethereum JSON-RPC QUANTITY without truncation.
 *
 * The accepted form is `0x0` or `0x` followed by at most 64 hexadecimal digits with no leading zero.
 */
fc::uint256 parse_rpc_quantity(const fc::variant& value, std::string_view field);

/** Format a uint256 as a canonical Ethereum JSON-RPC QUANTITY. */
std::string format_rpc_quantity(const fc::uint256& value);

/** Validate the policy itself before it is attached to a signing-capable client. */
void validate_transaction_policy_configuration(const ethereum_transaction_policy& policy);

/** Derive and cap `2 * base_fee + priority_fee` using checked uint256 arithmetic. */
fc::uint256 derive_max_fee_per_gas(const ethereum_transaction_policy& policy,
                                   const fc::uint256&                  priority_fee,
                                   const fc::uint256&                  base_fee);

/** Apply checked `(estimated_gas * 6) / 5` headroom and enforce the gas cap. */
fc::uint256 derive_buffered_gas_limit(const ethereum_transaction_policy& policy,
                                      const fc::uint256&                  estimated_gas);

/** Validate every settled EIP-1559 transaction field before a private-key operation. */
void validate_transaction_against_policy(const ethereum_transaction_policy&               policy,
                                         const fc::crypto::ethereum::eip1559_tx& transaction);

} // namespace fc::network::ethereum
