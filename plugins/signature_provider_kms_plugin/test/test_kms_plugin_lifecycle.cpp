#include <boost/test/unit_test.hpp>

#include <fc/crypto/chain_types_reflect.hpp>

#include <gsl-lite/gsl-lite.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_kms_plugin/signature_provider_kms_plugin.hpp>

#include <fc-test/crypto_utils.hpp>

#include <format>

// ---------------------------------------------------------------------------
// Plugin lifecycle: what enabling signature_provider_kms_plugin does. All
// offline -- no AWS, no network. The retain/claim mechanics themselves are
// covered with mock handlers in the manager's test binary
// (test_spec_retain_claim.cpp); KMS provider construction/claiming is offline
// by design but the probe wiring here still uses a mock PROBE handler so the
// startup pass runs no real GetPublicKey.
// ---------------------------------------------------------------------------

using sysio::signature_provider_kms_plugin;
using sysio::signature_provider_manager_plugin;

BOOST_AUTO_TEST_SUITE(kms_plugin_lifecycle)

BOOST_AUTO_TEST_CASE(initialize_registers_scheme_and_announces_plugin) {
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   appbase::scoped_app app;
   std::vector<const char*> argv{"test_signature_provider_kms_plugin"};
   BOOST_REQUIRE((app->initialize<signature_provider_manager_plugin, signature_provider_kms_plugin>(
      argv.size(), const_cast<char**>(argv.data()))));

   // The plugin's initialize registered the KMS handler: a second
   // registration must be rejected as a duplicate.
   auto& manager = app->get_plugin<signature_provider_manager_plugin>();
   BOOST_CHECK_EXCEPTION(
      manager.register_spec_handler("KMS",
                                    [](fc::crypto::chain_key_type_t, const fc::crypto::public_key&,
                                       std::string_view) -> sysio::provider_spec_result { return {}; }),
      fc::exception,
      [](const fc::exception& e) { return e.to_detail_string().find("already registered") != std::string::npos; });

   // The constructor announced the scheme with the plugin's demangled name --
   // the exact string an operator passes to `plugin =`.
   auto announced = sysio::sigprov::announced_scheme_plugin("KMS");
   BOOST_REQUIRE(announced.has_value());
   BOOST_CHECK_EQUAL(*announced, "sysio::signature_provider_kms_plugin");
}

BOOST_AUTO_TEST_CASE(attached_probes_run_at_manager_startup) {
   // Attaching a startup probe IS the opt-in -- there is no enable flag. A
   // provider created with a probe (mock PROBE scheme registered pre-init,
   // eager path; the real KMS handler attaches one per claimed key) must have
   // that probe run at manager plugin_startup with the kms plugin enabled and
   // no probe-related configuration at all.
   using namespace fc::crypto;
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   fc::test::keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);

   appbase::scoped_app app;
   auto& manager = app->_register_plugin<signature_provider_manager_plugin>();
   int   probe_calls = 0;
   manager.register_spec_handler(
      "PROBE", [&probe_calls](chain_key_type_t, const fc::crypto::public_key&,
                              std::string_view) -> sysio::provider_spec_result {
         return {.signer        = [](const fc::sha256&) { return fc::crypto::signature{}; },
                 .private_key   = std::nullopt,
                 .startup_probe = [&probe_calls] { ++probe_calls; }};
      });

   auto spec = std::format("probe-key,ethereum,ethereum,{},PROBE:x", fixture.public_key);
   std::vector<const char*> argv{"test_signature_provider_kms_plugin", "--signature-provider", spec.c_str()};
   BOOST_REQUIRE((app->initialize<signature_provider_manager_plugin, signature_provider_kms_plugin>(
      argv.size(), const_cast<char**>(argv.data()))));

   BOOST_CHECK_EQUAL(probe_calls, 0);              // probes run at startup, not initialize
   BOOST_CHECK_NO_THROW(manager.plugin_startup());
   BOOST_CHECK_EQUAL(probe_calls, 1);              // no flag needed: attached probe ran
}

BOOST_AUTO_TEST_SUITE_END()
