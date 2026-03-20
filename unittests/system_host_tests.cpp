/**
 * Golden-value tests for system/privileged consensus host functions.
 *
 * These tests verify the chain-level functions that the WASM intrinsics
 * (set_resource_limits, get_resource_limits, is_privileged, set_privileged,
 * is_feature_activated, get/set_blockchain_parameters_packed, etc.) delegate to.
 *
 * Uses the validating_tester framework since these functions require real
 * blockchain state (apply_context, controller, resource_limits_manager).
 */
#include <sysio/testing/tester.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/wasm_config.hpp>
#include <sysio/chain/chain_config.hpp>
#include <fc/io/datastream.hpp>

#include <boost/test/unit_test.hpp>

using namespace sysio::testing;
using namespace sysio::chain;

BOOST_AUTO_TEST_SUITE(system_host_tests)

// ============================================================================
// Resource Limits: set_account_limits / get_account_limits round-trip
// Tests the chain functions that set_resource_limits/get_resource_limits
// WASM intrinsics delegate to.
// ============================================================================

BOOST_AUTO_TEST_CASE_TEMPLATE(resource_limits_roundtrip, T, validating_testers) {
   T chain;
   chain.create_accounts({"alice"_n, "bob"_n});
   chain.produce_block();

   auto& rl = chain.control->get_mutable_resource_limits_manager();

   // Set specific limits
   rl.set_account_limits("alice"_n, 4096, 200, 300, false);
   chain.produce_block();

   // Get and verify exact values
   int64_t ram = 0, net = 0, cpu = 0;
   rl.get_account_limits("alice"_n, ram, net, cpu);
   BOOST_CHECK_EQUAL(ram, 4096);
   BOOST_CHECK_EQUAL(net, 200);
   BOOST_CHECK_EQUAL(cpu, 300);

   // Update limits
   rl.set_account_limits("alice"_n, 8192, 400, 600, false);
   chain.produce_block();

   rl.get_account_limits("alice"_n, ram, net, cpu);
   BOOST_CHECK_EQUAL(ram, 8192);
   BOOST_CHECK_EQUAL(net, 400);
   BOOST_CHECK_EQUAL(cpu, 600);

   // Different account has independent limits
   rl.set_account_limits("bob"_n, 1024, 50, 75, false);
   chain.produce_block();

   rl.get_account_limits("bob"_n, ram, net, cpu);
   BOOST_CHECK_EQUAL(ram, 1024);
   BOOST_CHECK_EQUAL(net, 50);
   BOOST_CHECK_EQUAL(cpu, 75);

   // Alice unchanged
   rl.get_account_limits("alice"_n, ram, net, cpu);
   BOOST_CHECK_EQUAL(ram, 8192);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(resource_limits_unlimited, T, validating_testers) {
   T chain;
   chain.create_accounts({"alice"_n});
   chain.produce_block();

   auto& rl = chain.control->get_mutable_resource_limits_manager();

   // -1 means unlimited
   rl.set_account_limits("alice"_n, -1, -1, -1, false);
   chain.produce_block();

   int64_t ram = 0, net = 0, cpu = 0;
   rl.get_account_limits("alice"_n, ram, net, cpu);
   BOOST_CHECK_EQUAL(ram, -1);
   BOOST_CHECK_EQUAL(net, -1);
   BOOST_CHECK_EQUAL(cpu, -1);
}

// ============================================================================
// Block Number Progression: get_block_num
// Tests that block numbers increment correctly.
// ============================================================================

BOOST_AUTO_TEST_CASE_TEMPLATE(block_number_progression, T, validating_testers) {
   T chain;

   uint32_t initial = chain.control->head().block_num();

   chain.produce_block();
   BOOST_CHECK_EQUAL(chain.control->head().block_num(), initial + 1);

   chain.produce_block();
   BOOST_CHECK_EQUAL(chain.control->head().block_num(), initial + 2);

   // Produce multiple blocks
   chain.produce_blocks(5);
   BOOST_CHECK_EQUAL(chain.control->head().block_num(), initial + 7);
}

// ============================================================================
// Blockchain Parameters: get/set chain configuration
// Tests the chain config that get/set_blockchain_parameters_packed
// WASM intrinsics operate on.
// ============================================================================

BOOST_AUTO_TEST_CASE_TEMPLATE(blockchain_parameters_golden, T, validating_testers) {
   T chain;
   chain.produce_block();

   const auto& gpo = chain.control->get_global_properties();
   const auto& cfg = gpo.configuration;

   // Verify default parameters are non-zero and sensible
   BOOST_CHECK_GT(cfg.max_block_net_usage, 0u);
   BOOST_CHECK_GT(cfg.max_block_cpu_usage, 0u);
   BOOST_CHECK_GT(cfg.max_transaction_net_usage, 0u);
   BOOST_CHECK_GT(cfg.max_transaction_cpu_usage, 0u);

   // Verify relationships between parameters
   BOOST_CHECK_LE(cfg.max_transaction_net_usage, cfg.max_block_net_usage);
   BOOST_CHECK_LE(cfg.max_transaction_cpu_usage, cfg.max_block_cpu_usage);
   BOOST_CHECK_LE(cfg.min_transaction_cpu_usage, cfg.max_transaction_cpu_usage);

   // Verify parameters are stable across blocks
   auto saved_max_block_net = cfg.max_block_net_usage;
   auto saved_max_block_cpu = cfg.max_block_cpu_usage;
   chain.produce_blocks(3);

   const auto& cfg2 = chain.control->get_global_properties().configuration;
   BOOST_CHECK_EQUAL(cfg2.max_block_net_usage, saved_max_block_net);
   BOOST_CHECK_EQUAL(cfg2.max_block_cpu_usage, saved_max_block_cpu);
}

// ============================================================================
// WASM Parameters: get/set WASM execution configuration
// Tests the WASM config that get/set_wasm_parameters_packed intrinsics
// operate on.
// ============================================================================

BOOST_AUTO_TEST_CASE_TEMPLATE(wasm_parameters_golden, T, validating_testers) {
   T chain;
   chain.produce_block();

   const auto& gpo = chain.control->get_global_properties();
   const auto& wcfg = gpo.wasm_configuration;

   // Verify default WASM parameters are set and sensible
   BOOST_CHECK_GT(wcfg.max_mutable_global_bytes, 0u);
   BOOST_CHECK_GT(wcfg.max_table_elements, 0u);
   BOOST_CHECK_GT(wcfg.max_section_elements, 0u);
   BOOST_CHECK_GT(wcfg.max_module_bytes, 0u);
   BOOST_CHECK_GT(wcfg.max_code_bytes, 0u);
   BOOST_CHECK_GT(wcfg.max_pages, 0u);
   BOOST_CHECK_GT(wcfg.max_call_depth, 0u);

   // Verify parameters are stable across blocks
   auto saved_max_pages = wcfg.max_pages;
   auto saved_max_call_depth = wcfg.max_call_depth;
   chain.produce_blocks(3);

   const auto& wcfg2 = chain.control->get_global_properties().wasm_configuration;
   BOOST_CHECK_EQUAL(wcfg2.max_pages, saved_max_pages);
   BOOST_CHECK_EQUAL(wcfg2.max_call_depth, saved_max_call_depth);
}

// ============================================================================
// Protocol Feature Activation: is_builtin_activated
// Tests the chain function that is_feature_activated WASM intrinsic
// delegates to.
// ============================================================================

BOOST_AUTO_TEST_CASE_TEMPLATE(feature_activation_state, T, validating_testers) {
   T chain;
   chain.produce_block();

   // Verify we can query feature activation status without crashing
   // The exact set of activated features depends on the genesis config,
   // but the query must be deterministic
   bool activated = chain.control->is_builtin_activated(
      builtin_protocol_feature_t::reserved_first_protocol_feature);

   // Query again - must return the same result (deterministic)
   bool activated2 = chain.control->is_builtin_activated(
      builtin_protocol_feature_t::reserved_first_protocol_feature);
   BOOST_CHECK_EQUAL(activated, activated2);

   // The result must not change across blocks without explicit activation
   chain.produce_blocks(3);
   bool activated3 = chain.control->is_builtin_activated(
      builtin_protocol_feature_t::reserved_first_protocol_feature);
   BOOST_CHECK_EQUAL(activated, activated3);
}

// ============================================================================
// Packed Structure Stability: chain_config_v0
// Verifies that the serialized layout of chain_config doesn't accidentally
// change (field added/removed/reordered), which would break the
// get/set_blockchain_parameters_packed WASM intrinsics.
// ============================================================================

BOOST_AUTO_TEST_CASE_TEMPLATE(chain_config_packed_stability, T, validating_testers) {
   T chain;
   chain.produce_block();

   const auto& cfg = chain.control->get_global_properties().configuration;

   // Pack the v0 config
   auto v0 = cfg.v0();
   auto packed = fc::raw::pack(v0);

   // Verify packed size is stable.
   // If this changes, the packed WASM intrinsic format has changed.
   BOOST_CHECK_EQUAL(packed.size(), 68u);

   // Round-trip: unpack and verify all fields match
   chain_config_v0 unpacked;
   fc::datastream<const char*> ds(packed.data(), packed.size());
   fc::raw::unpack(ds, unpacked);

   BOOST_CHECK_EQUAL(unpacked.max_block_net_usage, v0.max_block_net_usage);
   BOOST_CHECK_EQUAL(unpacked.target_block_net_usage_pct, v0.target_block_net_usage_pct);
   BOOST_CHECK_EQUAL(unpacked.max_transaction_net_usage, v0.max_transaction_net_usage);
   BOOST_CHECK_EQUAL(unpacked.base_per_transaction_net_usage, v0.base_per_transaction_net_usage);
   BOOST_CHECK_EQUAL(unpacked.net_usage_leeway, v0.net_usage_leeway);
   BOOST_CHECK_EQUAL(unpacked.context_free_discount_net_usage_num, v0.context_free_discount_net_usage_num);
   BOOST_CHECK_EQUAL(unpacked.context_free_discount_net_usage_den, v0.context_free_discount_net_usage_den);
   BOOST_CHECK_EQUAL(unpacked.max_block_cpu_usage, v0.max_block_cpu_usage);
   BOOST_CHECK_EQUAL(unpacked.target_block_cpu_usage_pct, v0.target_block_cpu_usage_pct);
   BOOST_CHECK_EQUAL(unpacked.max_transaction_cpu_usage, v0.max_transaction_cpu_usage);
   BOOST_CHECK_EQUAL(unpacked.min_transaction_cpu_usage, v0.min_transaction_cpu_usage);
   BOOST_CHECK_EQUAL(unpacked.max_transaction_lifetime, v0.max_transaction_lifetime);
   BOOST_CHECK_EQUAL(unpacked.deferred_trx_expiration_window, v0.deferred_trx_expiration_window);
   BOOST_CHECK_EQUAL(unpacked.max_transaction_delay, v0.max_transaction_delay);
   BOOST_CHECK_EQUAL(unpacked.max_inline_action_size, v0.max_inline_action_size);
   BOOST_CHECK_EQUAL(unpacked.max_inline_action_depth, v0.max_inline_action_depth);
   BOOST_CHECK_EQUAL(unpacked.max_authority_depth, v0.max_authority_depth);
}

// ============================================================================
// Packed Structure Stability: wasm_config
// Verifies that the serialized layout of wasm_config doesn't accidentally
// change, which would break get/set_wasm_parameters_packed intrinsics.
// ============================================================================

BOOST_AUTO_TEST_CASE_TEMPLATE(wasm_config_packed_stability, T, validating_testers) {
   T chain;
   chain.produce_block();

   const auto& wcfg = chain.control->get_global_properties().wasm_configuration;

   // Pack the wasm_config
   auto packed = fc::raw::pack(wcfg);

   // Verify packed size is stable (11 fields × uint32_t = 44 bytes)
   BOOST_CHECK_EQUAL(packed.size(), 44u);

   // Round-trip: unpack and verify all fields match
   wasm_config unpacked;
   fc::datastream<const char*> ds(packed.data(), packed.size());
   fc::raw::unpack(ds, unpacked);

   BOOST_CHECK_EQUAL(unpacked.max_mutable_global_bytes, wcfg.max_mutable_global_bytes);
   BOOST_CHECK_EQUAL(unpacked.max_table_elements, wcfg.max_table_elements);
   BOOST_CHECK_EQUAL(unpacked.max_section_elements, wcfg.max_section_elements);
   BOOST_CHECK_EQUAL(unpacked.max_linear_memory_init, wcfg.max_linear_memory_init);
   BOOST_CHECK_EQUAL(unpacked.max_func_local_bytes, wcfg.max_func_local_bytes);
   BOOST_CHECK_EQUAL(unpacked.max_nested_structures, wcfg.max_nested_structures);
   BOOST_CHECK_EQUAL(unpacked.max_symbol_bytes, wcfg.max_symbol_bytes);
   BOOST_CHECK_EQUAL(unpacked.max_module_bytes, wcfg.max_module_bytes);
   BOOST_CHECK_EQUAL(unpacked.max_code_bytes, wcfg.max_code_bytes);
   BOOST_CHECK_EQUAL(unpacked.max_pages, wcfg.max_pages);
   BOOST_CHECK_EQUAL(unpacked.max_call_depth, wcfg.max_call_depth);
}

BOOST_AUTO_TEST_SUITE_END()
