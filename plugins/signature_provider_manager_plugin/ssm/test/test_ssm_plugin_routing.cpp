/**
 * End-to-end tests that the `ssm` sub-library wires correctly into
 * `signature_provider_manager_plugin` via `register_spec_handler`. These
 * confirm the integration seam without invoking any real SSM API: the
 * registered handler is `create_ssm_provider_with_fetcher` bound to a fake
 * fetcher, so the full `--signature-provider` CSV path -- spec split, pubkey
 * parse, scheme dispatch, provider registration -- runs exactly as in
 * production while the one network call is stubbed.
 *
 * The real GetParameter round-trip is covered by `ssm_live_fetch_round_trip`
 * in `test_ssm_signature_provider.cpp`, which is env-gated.
 */

#include <boost/test/unit_test.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/ssm/ssm_signature_provider.hpp>

#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/key_serdes.hpp>
#include <fc-test/crypto_utils.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace fc::crypto;
namespace chain = sysio::chain;
namespace ssm   = sysio::sigprov::ssm;

namespace {

/// Minimal app fixture for plugin-routing tests: constructs the plugin
/// instance (so we can call methods on it BEFORE initialize), then
/// initializes.
struct ssm_routing_tester {
   appbase::scoped_app app{};

   sysio::signature_provider_manager_plugin& plugin() {
      return app->get_plugin<sysio::signature_provider_manager_plugin>();
   }
};

/// Stand up a tester with an `SSM` handler registered whose fetch is stubbed
/// to return `value` as a SecureString. Initialize is run with no
/// `--signature-provider` options; tests then invoke `create_provider`
/// directly with the spec they want to exercise.
std::unique_ptr<ssm_routing_tester> create_ssm_routing_app(std::string value) {
   auto tester = std::make_unique<ssm_routing_tester>();
   // `register_plugin<>` (static) only enqueues the plugin name in the static
   // registration list; the instance is constructed by `_register_plugin<>`
   // (or later by `initialize<>`). We need the instance now to register the
   // SSM spec handler before any `--signature-provider` parsing happens.
   // `_register_plugin<>` is idempotent, so the subsequent `initialize<>`
   // pass is a no-op for this plugin. This mirrors the registration nodeop's
   // main() performs with the real `create_ssm_provider`.
   auto& plug = tester->app->_register_plugin<sysio::signature_provider_manager_plugin>();
   plug.register_spec_handler(
      "SSM",
      [value = std::move(value)](chain_key_type_t key_type, const public_key& pub, std::string_view spec_data) {
         return ssm::create_ssm_provider_with_fetcher(
            key_type, pub, spec_data, [&value](const ssm::ssm_param_ref&) {
               return ssm::fetched_parameter{value, ssm::parameter_type::SecureString};
            });
      });

   std::vector<const char*> argv{"test_sigprov_ssm"};
   BOOST_CHECK(tester->app->initialize<sysio::signature_provider_manager_plugin>(
      argv.size(), const_cast<char**>(argv.data())));
   return tester;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(ssm_plugin_routing)

BOOST_AUTO_TEST_CASE(create_provider_routes_ssm_spec_through_handler) {
   const auto priv    = private_key::generate();
   const auto pub_str = priv.get_public_key().to_string({});

   auto tester = create_ssm_routing_app(priv.to_string({}));
   const auto spec = to_signature_provider_spec("ssm-wire-01", chain_kind_wire, chain_key_type_wire, pub_str,
                                                "SSM:us-east-1:/wire/test/bp1");
   const auto provider = tester->plugin().create_provider(spec);

   BOOST_REQUIRE(provider != nullptr);
   BOOST_CHECK_EQUAL(provider->key_name, "ssm-wire-01");
   // Full KEY: parity: the raw key is populated for downstream consumers.
   BOOST_REQUIRE(provider->private_key.has_value());
   BOOST_CHECK(provider->private_key->get_public_key() == priv.get_public_key());

   const auto digest = fc::sha256::hash(std::string{"ssm routing wire"});
   BOOST_CHECK(provider->sign(digest) == priv.sign(digest));
}

BOOST_AUTO_TEST_CASE(create_provider_routes_solana_ssm_spec) {
   // Solana is the type that structurally REQUIRES the raw key (its signing
   // path has no remote-closure fallback), so prove the full plugin path
   // populates it.
   const auto fixture = fc::test::load_keygen_fixture(fc::test::keygen_solana_name, 1);

   auto tester = create_ssm_routing_app(fixture.private_key);
   const auto spec = to_signature_provider_spec("ssm-sol-01", chain_kind_solana, chain_key_type_solana,
                                                fixture.public_key, "SSM:us-east-1:/wire/test/sol1");
   const auto provider = tester->plugin().create_provider(spec);

   BOOST_REQUIRE(provider != nullptr);
   BOOST_REQUIRE(provider->private_key.has_value());
   BOOST_CHECK(provider->private_key->get_public_key() ==
               from_native_string_to_public_key<chain_key_type_solana>(fixture.public_key));
}

BOOST_AUTO_TEST_CASE(create_provider_malformed_ssm_spec_throws_through_dispatch) {
   const auto priv    = private_key::generate();
   const auto pub_str = priv.get_public_key().to_string({});

   auto tester = create_ssm_routing_app(priv.to_string({}));
   // Missing region: the handler's parser must reject it through the plugin's
   // dispatch path.
   const auto spec = to_signature_provider_spec("ssm-bad-01", chain_kind_wire, chain_key_type_wire, pub_str,
                                                "SSM:/wire/test/no-region");
   BOOST_CHECK_THROW(tester->plugin().create_provider(spec), chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(unregistered_ssm_scheme_is_unknown_provider_type) {
   // Without a registration (the state of any binary whose main() does not
   // opt in), an SSM: spec fails with the unknown-scheme hint.
   ssm_routing_tester tester;
   tester.app->_register_plugin<sysio::signature_provider_manager_plugin>();
   std::vector<const char*> argv{"test_sigprov_ssm"};
   BOOST_CHECK(tester.app->initialize<sysio::signature_provider_manager_plugin>(
      argv.size(), const_cast<char**>(argv.data())));

   const auto priv    = private_key::generate();
   const auto pub_str = priv.get_public_key().to_string({});
   const auto spec    = to_signature_provider_spec("ssm-unreg-01", chain_kind_wire, chain_key_type_wire, pub_str,
                                                   "SSM:us-east-1:/wire/test/bp1");
   BOOST_CHECK_EXCEPTION(tester.plugin().create_provider(spec), chain::plugin_config_exception,
                         [](const fc::exception& e) {
                            return e.to_detail_string().find("Unknown provider type") != std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(duplicate_ssm_registration_rejected) {
   const auto priv = private_key::generate();
   auto tester = create_ssm_routing_app(priv.to_string({}));
   BOOST_CHECK_THROW(tester->plugin().register_spec_handler(
                        "SSM", &sysio::sigprov::ssm::create_ssm_provider),
                     fc::exception);
}

BOOST_AUTO_TEST_CASE(redaction_passes_ssm_specs_through_unchanged) {
   // `redact_signature_provider_spec` masks inline `KEY:` secrets. An `SSM:`
   // spec carries no secret -- the parameter name / ARN is safe to log -- and
   // must come through verbatim.
   const auto spec = std::string{"ssm-wire-01,wire,wire,PUB_K1_examplekey,SSM:us-east-1:/wire/prod/bp1"};
   BOOST_CHECK_EQUAL(sysio::redact_signature_provider_spec(spec), spec);
}

BOOST_AUTO_TEST_SUITE_END()
