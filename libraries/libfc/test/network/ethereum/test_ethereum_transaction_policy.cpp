#include <boost/test/unit_test.hpp>

#include <fc/network/ethereum/ethereum_transaction_policy.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>

using namespace fc::crypto::ethereum;
using namespace fc::network::ethereum;

namespace {

constexpr std::string_view max_uint256_decimal =
   "115792089237316195423570985008687907853269984665640564039457584007913129639935";
constexpr std::string_view above_max_uint256_decimal =
   "115792089237316195423570985008687907853269984665640564039457584007913129639936";

static_assert(std::is_same_v<decltype(ethereum_transaction_policy::chain_id), uint32_t>);

ethereum_transaction_policy bounded_policy() {
   return ethereum_transaction_policy{
      .client_id = "client-a",
      .chain_id = 31337,
      .max_priority_fee_per_gas = 10,
      .max_fee_per_gas = 100,
      .max_gas_limit = 1000,
      .max_total_native_cost = 100000,
   };
}

ethereum_transaction_policy uint256_wide_policy() {
   const fc::uint256 maximum{std::string(max_uint256_decimal)};
   return ethereum_transaction_policy{
      .client_id = "client-a",
      .chain_id = 31337,
      .max_priority_fee_per_gas = maximum,
      .max_fee_per_gas = maximum,
      .max_gas_limit = maximum,
      .max_total_native_cost = maximum,
   };
}

eip1559_tx bounded_transaction() {
   return eip1559_tx{
      .chain_id = 31337,
      .nonce = 0,
      .max_priority_fee_per_gas = 10,
      .max_fee_per_gas = 100,
      .gas_limit = 1000,
      .to = {},
      .value = 0,
      .data = {},
      .access_list = {},
   };
}

void check_rejection_reason(const std::function<void()>& operation,
                            ethereum_transaction_policy_reason expected_reason) {
   try {
      operation();
      BOOST_FAIL("expected ethereum transaction policy rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.reason() == expected_reason);
      BOOST_CHECK_EQUAL(reason_code_name(rejection.reason()), reason_code_name(expected_reason));
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(ethereum_transaction_policy_tests)

BOOST_AUTO_TEST_CASE(canonical_configuration_values_preserve_uint256_range) {
   const auto maximum = parse_canonical_uint256_decimal(max_uint256_decimal, "limit");
   BOOST_CHECK_EQUAL(maximum.str(), max_uint256_decimal);

   check_rejection_reason(
      [] { parse_canonical_uint256_decimal(above_max_uint256_decimal, "limit"); },
      ethereum_transaction_policy_reason::configuration_value_invalid);
   check_rejection_reason(
      [] { parse_canonical_uint256_decimal("01", "limit"); },
      ethereum_transaction_policy_reason::configuration_value_invalid);
   check_rejection_reason(
      [] { parse_canonical_uint256_decimal("0", "limit"); },
      ethereum_transaction_policy_reason::configuration_value_invalid);
}

BOOST_AUTO_TEST_CASE(rpc_quantities_are_canonical_and_non_truncating) {
   const std::string maximum_quantity = "0x" + std::string(64, 'f');
   BOOST_CHECK_EQUAL(parse_rpc_quantity(fc::variant(maximum_quantity), "nonce").str(), max_uint256_decimal);
   BOOST_CHECK_EQUAL(format_rpc_quantity(fc::uint256{0}), "0x0");
   BOOST_CHECK_EQUAL(format_rpc_quantity(fc::uint256{127}), "0x7f");
   BOOST_CHECK_EQUAL(format_rpc_quantity(fc::uint256{128}), "0x80");
   BOOST_CHECK_EQUAL(format_rpc_quantity(fc::uint256{"18446744073709551617"}),
                     "0x10000000000000001");
   BOOST_CHECK_EQUAL(format_rpc_quantity(parse_rpc_quantity(fc::variant(maximum_quantity), "nonce")),
                     maximum_quantity);

   check_rejection_reason(
      [] { parse_rpc_quantity(fc::variant("0x" + std::string(65, '1')), "nonce"); },
      ethereum_transaction_policy_reason::rpc_quantity_out_of_range);
   check_rejection_reason(
      [] { parse_rpc_quantity(fc::variant("0x01"), "nonce"); },
      ethereum_transaction_policy_reason::rpc_quantity_invalid);
   check_rejection_reason(
      [] { parse_rpc_quantity(fc::variant(uint64_t{1}), "nonce"); },
      ethereum_transaction_policy_reason::rpc_quantity_invalid);
}

BOOST_AUTO_TEST_CASE(malformed_values_do_not_reappear_in_policy_diagnostics) {
   constexpr std::string_view sensitive_url = "https://user:password@example.invalid/rpc?token=secret";
   try {
      parse_rpc_quantity(fc::variant(std::string(sensitive_url)), "nonce");
      BOOST_FAIL("expected malformed RPC quantity rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.observed().find(sensitive_url) == std::string::npos);
      BOOST_CHECK(rejection.to_detail_string().find(sensitive_url) == std::string::npos);
   }

   auto policy = bounded_policy();
   policy.client_id = sensitive_url;
   try {
      validate_transaction_policy_configuration(policy);
      BOOST_FAIL("expected malformed client id rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.observed().find(sensitive_url) == std::string::npos);
      BOOST_CHECK(rejection.to_detail_string().find(sensitive_url) == std::string::npos);
   }
}

BOOST_AUTO_TEST_CASE(policy_configuration_requires_positive_consistent_limits) {
   auto policy = bounded_policy();
   policy.chain_id = 0;
   check_rejection_reason(
      [&] { validate_transaction_policy_configuration(policy); },
      ethereum_transaction_policy_reason::configuration_value_invalid);

   policy = bounded_policy();
   policy.max_priority_fee_per_gas = 0;
   check_rejection_reason(
      [&] { validate_transaction_policy_configuration(policy); },
      ethereum_transaction_policy_reason::configuration_value_invalid);

   policy = bounded_policy();
   policy.max_fee_per_gas = 0;
   check_rejection_reason(
      [&] { validate_transaction_policy_configuration(policy); },
      ethereum_transaction_policy_reason::configuration_value_invalid);

   policy = bounded_policy();
   policy.max_gas_limit = 0;
   check_rejection_reason(
      [&] { validate_transaction_policy_configuration(policy); },
      ethereum_transaction_policy_reason::configuration_value_invalid);

   policy = bounded_policy();
   policy.max_total_native_cost = 0;
   check_rejection_reason(
      [&] { validate_transaction_policy_configuration(policy); },
      ethereum_transaction_policy_reason::configuration_value_invalid);

   policy = bounded_policy();
   policy.max_priority_fee_per_gas = policy.max_fee_per_gas + 1;
   check_rejection_reason(
      [&] { validate_transaction_policy_configuration(policy); },
      ethereum_transaction_policy_reason::fee_relationship_invalid);
}

BOOST_AUTO_TEST_CASE(fee_caps_document_dynamic_base_fee_headroom) {
   auto policy = bounded_policy();
   policy.max_priority_fee_per_gas = 3;
   policy.max_fee_per_gas = 4;
   BOOST_CHECK_NO_THROW(validate_transaction_policy_configuration(policy));

   try {
      derive_max_fee_per_gas(policy, 3, 1);
      BOOST_FAIL("expected insufficient fee-cap headroom rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.reason() == ethereum_transaction_policy_reason::max_fee_cap_exceeded);
      BOOST_CHECK_EQUAL(rejection.field(), "max_fee_per_gas_wei");
      BOOST_CHECK_EQUAL(rejection.observed(), "5");
      BOOST_REQUIRE(rejection.allowed().has_value());
      BOOST_CHECK_EQUAL(*rejection.allowed(), "4");
   }
}

BOOST_AUTO_TEST_CASE(exact_caps_pass_and_each_cap_plus_one_rejects) {
   const auto policy = bounded_policy();
   const auto exact = bounded_transaction();
   BOOST_CHECK_NO_THROW(validate_transaction_against_policy(policy, exact));

   auto transaction = exact;
   ++transaction.max_priority_fee_per_gas;
   check_rejection_reason(
      [&] { validate_transaction_against_policy(policy, transaction); },
      ethereum_transaction_policy_reason::priority_fee_cap_exceeded);

   transaction = exact;
   ++transaction.max_fee_per_gas;
   check_rejection_reason(
      [&] { validate_transaction_against_policy(policy, transaction); },
      ethereum_transaction_policy_reason::max_fee_cap_exceeded);

   transaction = exact;
   transaction.max_fee_per_gas = 99;
   ++transaction.gas_limit;
   check_rejection_reason(
      [&] { validate_transaction_against_policy(policy, transaction); },
      ethereum_transaction_policy_reason::gas_limit_cap_exceeded);

   transaction = exact;
   transaction.gas_limit = 999;
   transaction.value = 101;
   check_rejection_reason(
      [&] { validate_transaction_against_policy(policy, transaction); },
      ethereum_transaction_policy_reason::total_cost_cap_exceeded);
}

BOOST_AUTO_TEST_CASE(nonzero_value_is_included_in_the_total_cost) {
   const auto policy = bounded_policy();
   auto       transaction = bounded_transaction();
   transaction.gas_limit = 999;

   transaction.value = 99;
   BOOST_CHECK_NO_THROW(validate_transaction_against_policy(policy, transaction));

   transaction.value = 100;
   BOOST_CHECK_NO_THROW(validate_transaction_against_policy(policy, transaction));

   ++transaction.value;
   check_rejection_reason(
      [&] { validate_transaction_against_policy(policy, transaction); },
      ethereum_transaction_policy_reason::total_cost_cap_exceeded);
}

BOOST_AUTO_TEST_CASE(fee_relation_and_chain_domain_are_enforced) {
   const auto policy = bounded_policy();
   auto       transaction = bounded_transaction();
   transaction.max_fee_per_gas = transaction.max_priority_fee_per_gas - 1;
   check_rejection_reason(
      [&] { validate_transaction_against_policy(policy, transaction); },
      ethereum_transaction_policy_reason::fee_relationship_invalid);

   transaction = bounded_transaction();
   ++transaction.chain_id;
   check_rejection_reason(
      [&] { validate_transaction_against_policy(policy, transaction); },
      ethereum_transaction_policy_reason::chain_id_mismatch);
}

BOOST_AUTO_TEST_CASE(total_cost_multiplication_and_addition_overflow_reject) {
   const auto policy = uint256_wide_policy();
   const fc::uint256 maximum{std::string(max_uint256_decimal)};
   auto transaction = bounded_transaction();
   transaction.max_priority_fee_per_gas = 1;
   transaction.max_fee_per_gas = maximum;
   transaction.gas_limit = 2;
   check_rejection_reason(
      [&] { validate_transaction_against_policy(policy, transaction); },
      ethereum_transaction_policy_reason::total_cost_multiplication_overflow);

   transaction.max_fee_per_gas = maximum - 1;
   transaction.gas_limit = 1;
   transaction.value = 2;
   check_rejection_reason(
      [&] { validate_transaction_against_policy(policy, transaction); },
      ethereum_transaction_policy_reason::total_cost_addition_overflow);
}

BOOST_AUTO_TEST_CASE(fee_derivation_checks_each_intermediate_and_caps_the_result) {
   const auto wide_policy = uint256_wide_policy();
   const fc::uint256 maximum{std::string(max_uint256_decimal)};
   check_rejection_reason(
      [&] { derive_max_fee_per_gas(wide_policy, 1, maximum / 2 + 1); },
      ethereum_transaction_policy_reason::max_fee_derivation_overflow);
   check_rejection_reason(
      [&] { derive_max_fee_per_gas(wide_policy, 2, maximum / 2); },
      ethereum_transaction_policy_reason::max_fee_derivation_overflow);

   const auto policy = bounded_policy();
   BOOST_CHECK_EQUAL(derive_max_fee_per_gas(policy, 10, 45), 100);
   check_rejection_reason(
      [&] { derive_max_fee_per_gas(policy, 10, 46); },
      ethereum_transaction_policy_reason::max_fee_cap_exceeded);
   check_rejection_reason(
      [&] { derive_max_fee_per_gas(policy, 11, 1); },
      ethereum_transaction_policy_reason::priority_fee_cap_exceeded);
}

BOOST_AUTO_TEST_CASE(gas_headroom_checks_multiplication_and_final_cap) {
   const auto wide_policy = uint256_wide_policy();
   const fc::uint256 maximum{std::string(max_uint256_decimal)};
   check_rejection_reason(
      [&] { derive_buffered_gas_limit(wide_policy, maximum / 6 + 1); },
      ethereum_transaction_policy_reason::gas_limit_derivation_overflow);

   const auto policy = bounded_policy();
   BOOST_CHECK_EQUAL(derive_buffered_gas_limit(policy, 833), 999);
   BOOST_CHECK_EQUAL(derive_buffered_gas_limit(policy, 834), 1000);
   check_rejection_reason(
      [&] { derive_buffered_gas_limit(policy, 835); },
      ethereum_transaction_policy_reason::gas_limit_cap_exceeded);
}

BOOST_AUTO_TEST_SUITE_END()
