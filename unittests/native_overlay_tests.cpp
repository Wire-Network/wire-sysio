#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(native_overlay_tests)

// When native-module runtime is not available, provide a placeholder so the
// suite is never empty (ctest treats an empty suite as a failure).
#ifndef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED

BOOST_AUTO_TEST_CASE(native_module_not_available) {
   BOOST_TEST_MESSAGE("native-module runtime not enabled — skipping native overlay tests");
}

#else // SYSIO_NATIVE_MODULE_RUNTIME_ENABLED

#include <sysio/chain/wasm_interface.hpp>
#include <sysio/chain/webassembly/native-module/native_module_overlay.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/variant_object.hpp>
#include <fc/crypto/sha256.hpp>

#include <test_contracts.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace std;
using mvo = fc::mutable_variant_object;

// Path to native contract .so files (set by CMake)
#ifndef NATIVE_CONTRACTS_DIR
#error "NATIVE_CONTRACTS_DIR must be defined"
#endif

// Test that native_module_overlay routes a contract through native .so
// while leaving other contracts on the normal WASM runtime.
BOOST_AUTO_TEST_CASE(overlay_routes_native_contract) { try {
   validating_tester chain;
   chain.produce_block();

   chain.create_accounts({"alice"_n, "bob"_n, "sysio.token"_n});
   chain.produce_block();

   // Deploy sysio.token via WASM
   auto wasm = test_contracts::sysio_token_wasm();
   chain.set_code("sysio.token"_n, wasm);
   chain.set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   chain.set_privileged("sysio.token"_n);
   chain.produce_block();

   // Compute the code_hash the same way the chain does
   auto code_hash = fc::sha256::hash(reinterpret_cast<const char*>(wasm.data()), wasm.size());

   // Load the native .so via overlay
   namespace fs = std::filesystem;
   fs::path so_path = fs::path(NATIVE_CONTRACTS_DIR) / "sysio.token" / "sysio.token_native.so";
   BOOST_REQUIRE_MESSAGE(fs::exists(so_path), "Native .so not found: " + so_path.string());

   webassembly::native_module::native_module_overlay overlay;
   overlay.load(code_hash, so_path);
   BOOST_REQUIRE_EQUAL(overlay.size(), 1u);

   // Install as substitute_apply on both main and validating chain
   auto install_overlay = [&overlay](controller& ctrl) {
      ctrl.get_wasm_interface().substitute_apply =
         [&overlay](const digest_type& hash, uint8_t vm_type,
                    uint8_t vm_version, apply_context& ctx) -> bool {
            return overlay(hash, vm_type, vm_version, ctx);
         };
   };
   install_overlay(*chain.control);
   if (chain.validating_node)
      install_overlay(*chain.validating_node);

   // Execute actions through the native overlay using push_action helper
   chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mvo()
      ("issuer", "alice")
      ("maximum_supply", "1000000000.0000 SYS"));
   chain.produce_block();

   chain.push_action("sysio.token"_n, "issue"_n, "alice"_n, mvo()
      ("to", "alice")
      ("quantity", "100.0000 SYS")
      ("memo", "native overlay test"));
   chain.produce_block();

   chain.push_action("sysio.token"_n, "transfer"_n, "alice"_n, mvo()
      ("from", "alice")
      ("to", "bob")
      ("quantity", "25.0000 SYS")
      ("memo", "via native overlay"));
   chain.produce_block();

   // Verify state is correct (proves native execution wrote to chainbase correctly)
   auto bob_balance = chain.get_currency_balance("sysio.token"_n, symbol(SY(4, SYS)), "bob"_n);
   BOOST_REQUIRE_EQUAL(bob_balance, asset::from_string("25.0000 SYS"));

   auto alice_balance = chain.get_currency_balance("sysio.token"_n, symbol(SY(4, SYS)), "alice"_n);
   BOOST_REQUIRE_EQUAL(alice_balance, asset::from_string("75.0000 SYS"));

} FC_LOG_AND_RETHROW() }

// Test that contracts NOT in the overlay still execute via WASM
BOOST_AUTO_TEST_CASE(overlay_falls_through_to_wasm) { try {
   validating_tester chain;
   chain.produce_block();

   // Install an empty overlay — should fall through for all contracts
   webassembly::native_module::native_module_overlay overlay;
   chain.control->get_wasm_interface().substitute_apply =
      [&overlay](const digest_type& hash, uint8_t vm_type,
                 uint8_t vm_version, apply_context& ctx) -> bool {
         return overlay(hash, vm_type, vm_version, ctx);
      };

   // Normal WASM operations should still work
   chain.create_accounts({"sysio.token"_n, "alice"_n});
   chain.produce_block();
   chain.set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   chain.set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   chain.set_privileged("sysio.token"_n);
   chain.produce_block();

   chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mvo()
      ("issuer", "alice")
      ("maximum_supply", "1000.0000 TST"));
   chain.produce_block();

   // If we get here without throwing, WASM fallback works
   BOOST_REQUIRE(true);

} FC_LOG_AND_RETHROW() }

#endif // SYSIO_NATIVE_MODULE_RUNTIME_ENABLED

BOOST_AUTO_TEST_SUITE_END()
