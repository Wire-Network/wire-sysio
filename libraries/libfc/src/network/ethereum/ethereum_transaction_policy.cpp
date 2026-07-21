#include <fc/network/ethereum/ethereum_transaction_policy.hpp>

#include <fc/crypto/hex.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <ranges>
#include <utility>

namespace fc::network::ethereum {

namespace {

constexpr std::string_view max_uint256_decimal =
   "115792089237316195423570985008687907853269984665640564039457584007913129639935";
constexpr size_t max_uint256_hex_digits = 64;
constexpr size_t max_diagnostic_value_chars = 96;
constexpr size_t max_client_id_chars = 64;
constexpr std::string_view hex_quantity_prefix = "0x";
constexpr uint64_t gas_headroom_multiplier = 6;
constexpr uint64_t gas_headroom_divisor = 5;
constexpr uint64_t max_fee_base_multiplier = 2;

/** Return the largest uint256 value without relying on a narrowing intermediate. */
const fc::uint256& max_uint256() {
   static const fc::uint256 value{std::string(max_uint256_decimal)};
   return value;
}

/** Bound untrusted diagnostic strings so malformed configuration cannot flood logs. */
std::string diagnostic_value(std::string_view value) {
   if (value.size() <= max_diagnostic_value_chars) return std::string(value);
   return std::string(value.substr(0, max_diagnostic_value_chars)) + "...";
}

/** Check whether every character is an ASCII decimal digit. */
bool is_decimal(std::string_view value) {
   return std::ranges::all_of(value, [](unsigned char c) { return c >= '0' && c <= '9'; });
}

/** Check whether every character is an ASCII hexadecimal digit. */
bool is_hexadecimal(std::string_view value) {
   return std::ranges::all_of(value, [](unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
   });
}

} // namespace

bool is_safe_transaction_policy_identifier(std::string_view identifier) {
   return !identifier.empty() && identifier.size() <= max_client_id_chars &&
          std::ranges::all_of(identifier, [](unsigned char c) {
             const bool ascii_alphanumeric = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                                             (c >= 'a' && c <= 'z');
             return ascii_alphanumeric || c == '-' || c == '_' || c == '.';
          });
}

ethereum_transaction_policy_exception::ethereum_transaction_policy_exception(
   ethereum_transaction_policy_reason reason,
   std::string                        field,
   std::string                        observed,
   std::optional<std::string>         allowed)
   : fc::exception(FC_LOG_MESSAGE(error,
                                  "reason_code={} field={} observed={} allowed={}",
                                  reason_code_name(reason),
                                  field,
                                  observed,
                                  allowed.value_or("n/a")),
                   fc::invalid_arg_exception_code,
                   "ethereum_transaction_policy_exception",
                   "Ethereum transaction policy rejected")
   , _reason(reason)
   , _field(std::move(field))
   , _observed(std::move(observed))
   , _allowed(std::move(allowed)) {}

std::shared_ptr<fc::exception> ethereum_transaction_policy_exception::dynamic_copy_exception() const {
   return std::make_shared<ethereum_transaction_policy_exception>(*this);
}

void ethereum_transaction_policy_exception::rethrow() const {
   throw *this;
}

void throw_transaction_policy_exception(ethereum_transaction_policy_reason reason,
                                        std::string_view                    field,
                                        std::string                         observed,
                                        std::optional<std::string>          allowed) {
   throw ethereum_transaction_policy_exception(
      reason, std::string(field), std::move(observed), std::move(allowed));
}

fc::uint256 parse_canonical_uint256_decimal(std::string_view value,
                                            std::string_view field,
                                            bool             allow_zero) {
   const bool numeric_value = is_decimal(value);
   const auto invalid = [&] {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_value_invalid,
         field,
         numeric_value ? diagnostic_value(value) : "<invalid>");
   };

   if (value.empty() || !numeric_value || (value.size() > 1 && value.front() == '0')) invalid();
   if (value.size() > max_uint256_decimal.size() ||
       (value.size() == max_uint256_decimal.size() && value > max_uint256_decimal)) {
      invalid();
   }

   fc::uint256 parsed{std::string(value)};
   if (!allow_zero && parsed == 0) invalid();
   return parsed;
}

fc::uint256 parse_rpc_quantity(const fc::variant& value, std::string_view field) {
   if (!value.is_string()) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::rpc_quantity_invalid, field, "<non-string>");
   }

   const std::string encoded = value.as_string();
   if (!encoded.starts_with(hex_quantity_prefix)) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::rpc_quantity_invalid, field, "<invalid>");
   }

   const std::string_view digits{encoded.data() + hex_quantity_prefix.size(),
                                 encoded.size() - hex_quantity_prefix.size()};
   const bool hexadecimal_value = is_hexadecimal(digits);
   if (digits.empty() || !hexadecimal_value || (digits.size() > 1 && digits.front() == '0')) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::rpc_quantity_invalid,
         field,
         hexadecimal_value ? diagnostic_value(encoded) : "<invalid>");
   }
   if (digits.size() > max_uint256_hex_digits) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::rpc_quantity_out_of_range,
                                         field,
                                         diagnostic_value(encoded));
   }

   return fc::uint256(fc::from_hex(encoded));
}

std::string format_rpc_quantity(const fc::uint256& value) {
   return std::string(hex_quantity_prefix) + value.str(0, std::ios_base::hex);
}

void validate_transaction_policy_configuration(const ethereum_transaction_policy& policy) {
   if (!is_safe_transaction_policy_identifier(policy.client_id)) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_value_invalid, "client_id", "<invalid>");
   }
   if (policy.chain_id == 0) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_value_invalid, "chain_id", "0");
   }
   if (policy.max_priority_fee_per_gas == 0) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_value_invalid,
         "max_priority_fee_per_gas_wei",
         "0");
   }
   if (policy.max_fee_per_gas == 0) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::configuration_value_invalid,
                                         "max_fee_per_gas_wei",
                                         "0");
   }
   if (policy.max_gas_limit == 0) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_value_invalid, "max_gas_limit", "0");
   }
   if (policy.max_total_native_cost == 0) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_value_invalid,
         "max_total_native_cost_wei",
         "0");
   }
   if (policy.max_priority_fee_per_gas > policy.max_fee_per_gas) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::fee_relationship_invalid,
                                         "max_priority_fee_per_gas_wei",
                                         policy.max_priority_fee_per_gas.str(),
                                         policy.max_fee_per_gas.str());
   }
}

fc::uint256 derive_max_fee_per_gas(const ethereum_transaction_policy& policy,
                                   const fc::uint256&                  priority_fee,
                                   const fc::uint256&                  base_fee) {
   if (priority_fee > policy.max_priority_fee_per_gas) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::priority_fee_cap_exceeded,
                                         "max_priority_fee_per_gas",
                                         priority_fee.str(),
                                         policy.max_priority_fee_per_gas.str());
   }

   const fc::uint256 maximum_base_fee = max_uint256() / max_fee_base_multiplier;
   if (base_fee > maximum_base_fee) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::max_fee_derivation_overflow,
         "max_fee_per_gas",
         "base_fee=" + base_fee.str() + ",multiplier=" + std::to_string(max_fee_base_multiplier),
         max_uint256().str());
   }
   const fc::uint256 doubled_base_fee = base_fee * max_fee_base_multiplier;
   const fc::uint256 remaining_fee_capacity = max_uint256() - doubled_base_fee;
   if (priority_fee > remaining_fee_capacity) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::max_fee_derivation_overflow,
         "max_fee_per_gas",
         "doubled_base_fee=" + doubled_base_fee.str() + ",priority_fee=" + priority_fee.str(),
         max_uint256().str());
   }

   const fc::uint256 max_fee = doubled_base_fee + priority_fee;
   if (max_fee > policy.max_fee_per_gas) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::max_fee_cap_exceeded,
                                         "max_fee_per_gas",
                                         "derived_max_fee_per_gas=" + max_fee.str() +
                                            ",formula=2*base_fee_per_gas(" + base_fee.str() +
                                            ")+max_priority_fee_per_gas(" + priority_fee.str() + ")",
                                         policy.max_fee_per_gas.str());
   }
   return max_fee;
}

fc::uint256 derive_buffered_gas_limit(const ethereum_transaction_policy& policy,
                                      const fc::uint256&                  estimated_gas) {
   if (estimated_gas > policy.max_gas_limit) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::gas_limit_cap_exceeded,
                                         "estimated_gas",
                                         estimated_gas.str(),
                                         policy.max_gas_limit.str());
   }

   const fc::uint256 maximum_estimate = max_uint256() / gas_headroom_multiplier;
   if (estimated_gas > maximum_estimate) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::gas_limit_derivation_overflow,
         "gas_limit",
         "estimated_gas=" + estimated_gas.str() +
            ",multiplier=" + std::to_string(gas_headroom_multiplier),
         max_uint256().str());
   }

   const fc::uint256 gas_limit = (estimated_gas * gas_headroom_multiplier) / gas_headroom_divisor;
   if (gas_limit > policy.max_gas_limit) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::gas_limit_cap_exceeded,
                                         "gas_limit",
                                         gas_limit.str(),
                                         policy.max_gas_limit.str());
   }
   return gas_limit;
}

void validate_transaction_against_policy(const ethereum_transaction_policy&               policy,
                                         const fc::crypto::ethereum::eip1559_tx& transaction) {
   if (transaction.chain_id != fc::uint256{policy.chain_id}) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::chain_id_mismatch,
                                         "chain_id",
                                         transaction.chain_id.str(),
                                         std::to_string(policy.chain_id));
   }
   if (transaction.max_fee_per_gas < transaction.max_priority_fee_per_gas) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::fee_relationship_invalid,
                                         "max_fee_per_gas",
                                         transaction.max_fee_per_gas.str(),
                                         transaction.max_priority_fee_per_gas.str());
   }
   if (transaction.max_priority_fee_per_gas > policy.max_priority_fee_per_gas) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::priority_fee_cap_exceeded,
                                         "max_priority_fee_per_gas",
                                         transaction.max_priority_fee_per_gas.str(),
                                         policy.max_priority_fee_per_gas.str());
   }
   if (transaction.max_fee_per_gas > policy.max_fee_per_gas) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::max_fee_cap_exceeded,
                                         "max_fee_per_gas",
                                         transaction.max_fee_per_gas.str(),
                                         policy.max_fee_per_gas.str());
   }
   if (transaction.gas_limit > policy.max_gas_limit) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::gas_limit_cap_exceeded,
                                         "gas_limit",
                                         transaction.gas_limit.str(),
                                         policy.max_gas_limit.str());
   }

   const fc::uint256 maximum_fee_without_overflow =
      transaction.gas_limit == 0 ? max_uint256() : fc::uint256{max_uint256() / transaction.gas_limit};
   if (transaction.max_fee_per_gas > maximum_fee_without_overflow) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::total_cost_multiplication_overflow,
         "max_total_native_cost",
         "gas_limit=" + transaction.gas_limit.str() +
            ",max_fee_per_gas=" + transaction.max_fee_per_gas.str(),
         max_uint256().str());
   }
   const fc::uint256 maximum_gas_cost = transaction.gas_limit * transaction.max_fee_per_gas;
   const fc::uint256 remaining_total_capacity = max_uint256() - maximum_gas_cost;
   if (transaction.value > remaining_total_capacity) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::total_cost_addition_overflow,
         "max_total_native_cost",
         "maximum_gas_cost=" + maximum_gas_cost.str() + ",value=" + transaction.value.str(),
         max_uint256().str());
   }

   const fc::uint256 maximum_total_cost = maximum_gas_cost + transaction.value;
   if (maximum_total_cost > policy.max_total_native_cost) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::total_cost_cap_exceeded,
                                         "max_total_native_cost",
                                         maximum_total_cost.str(),
                                         policy.max_total_native_cost.str());
   }
}

} // namespace fc::network::ethereum
