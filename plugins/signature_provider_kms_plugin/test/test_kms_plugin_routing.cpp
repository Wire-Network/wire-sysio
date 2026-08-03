/**
 * End-to-end tests that the `kms` sub-library wires correctly into
 * `signature_provider_manager_plugin` via `register_spec_handler`. These
 * confirm the integration seam without invoking any real KMS API: the sign
 * closure is built and inspected for shape, never called.
 *
 * The real KMS::Sign round-trip is covered by `kms_live_sign_round_trip` in
 * `test_kms_signature_provider.cpp`, which is env-gated.
 */

#include <boost/test/unit_test.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/signature_provider_kms_plugin/kms_signature_provider.hpp>

#include <fc/crypto/chain_types_reflect.hpp>
#include <fc-test/crypto_utils.hpp>

#include <gsl-lite/gsl-lite.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

/// Minimal app fixture for plugin-routing tests: constructs the plugin
/// instance (so we can call methods on it BEFORE initialize), then
/// initializes.
struct kms_routing_tester {
   appbase::scoped_app app{};

   sysio::signature_provider_manager_plugin& plugin() {
      return app->get_plugin<sysio::signature_provider_manager_plugin>();
   }
};

/// Stand up a tester with the KMS handler registered. Initialize is run with
/// no `--signature-provider` options; tests then invoke `create_provider`
/// directly with the spec they want to exercise.
std::unique_ptr<kms_routing_tester> create_kms_routing_app() {
   auto tester = std::make_unique<kms_routing_tester>();
   // `register_plugin<>` (static) only enqueues the plugin name in the static
   // registration list; the instance is constructed by `_register_plugin<>`
   // (or later by `initialize<>`). We need the instance now to register the
   // KMS spec handler before any `--signature-provider` parsing happens.
   // `_register_plugin<>` is idempotent, so the subsequent `initialize<>`
   // pass is a no-op for this plugin.
   auto& plug = tester->app->_register_plugin<sysio::signature_provider_manager_plugin>();
   plug.register_spec_handler(
      "KMS", &sysio::sigprov::kms::create_kms_provider);

   std::vector<const char*> argv{"test_signature_provider_kms_plugin"};
   BOOST_CHECK(tester->app->initialize<sysio::signature_provider_manager_plugin>(
      argv.size(), const_cast<char**>(argv.data())));
   return tester;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(kms_plugin_routing)

BOOST_AUTO_TEST_CASE(create_provider_ethereum_kms_spec_routes_through_parser) {
   // End-to-end check that with the KMS handler registered, the plugin's
   // spec parser routes `KMS:` through the handler and returns a provider
   // whose sign closure is callable. The closure itself is *not* invoked
   // here -- invocation issues a real KMS::Sign request, covered only by
   // the env-gated live test.
   using namespace fc::test;
   using namespace fc::crypto;

   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });

   keygen_result fixture          = load_keygen_fixture("ethereum", 1);
   const std::string kms_provider = "KMS:us-east-1:alias/wire-cranker-eth-01";
   const auto provider_spec       = to_signature_provider_spec(
      "kms-eth-01", chain_kind_ethereum, chain_key_type_ethereum,
      fixture.public_key, kms_provider);

   auto  tester = create_kms_routing_app();
   auto& mgr    = tester->plugin();

   const auto provider = mgr.create_provider(provider_spec);

   BOOST_CHECK_EQUAL(provider->key_name, "kms-eth-01");
   BOOST_TEST((provider->target_chain == chain_kind_ethereum));
   BOOST_TEST((provider->key_type == chain_key_type_ethereum));
   BOOST_CHECK(static_cast<bool>(provider->sign));
   // KMS-backed providers carry no local private key.
   BOOST_CHECK(!provider->private_key.has_value());
}

BOOST_AUTO_TEST_CASE(create_provider_kms_spec_rejects_solana) {
   // The KMS handler must reject a non-secp256k1 chain at parse time, not at
   // first sign -- operators should learn early that KMS can't sign Solana
   // ed25519.
   using namespace fc::test;
   using namespace fc::crypto;

   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });

   keygen_result fixture          = load_keygen_fixture("solana", 1);
   const std::string kms_provider = "KMS:us-east-1:alias/test";
   const auto provider_spec       = to_signature_provider_spec(
      "kms-sol-01", chain_kind_solana, chain_key_type_solana,
      fixture.public_key, kms_provider);

   auto  tester = create_kms_routing_app();
   auto& mgr    = tester->plugin();

   BOOST_CHECK_THROW(mgr.create_provider(provider_spec),
                     sysio::chain::pending_impl_exception);
}

BOOST_AUTO_TEST_SUITE_END()
