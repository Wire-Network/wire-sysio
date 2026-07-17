#include <boost/test/unit_test.hpp>

#include <fc/crypto/chain_types_reflect.hpp>

#include <gsl-lite/gsl-lite.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_kms_plugin/signature_provider_kms_plugin.hpp>

#include <fc-test/crypto_utils.hpp>

#include <format>

// --------------------------------------------------------------------------- Plugin wiring: what constructing
// signature_provider_kms_plugin does. All offline -- no AWS, no network. The manager-side create/gate mechanics are
// covered with mock handlers in the manager's test binary (test_extension_schemes.cpp); the probe wiring here uses a
// mock PROBE handler so the startup pass runs no real GetPublicKey.
// ---------------------------------------------------------------------------

using sysio::signature_provider_kms_plugin;
using sysio::signature_provider_manager_plugin;

BOOST_AUTO_TEST_SUITE(kms_plugin_lifecycle)

BOOST_AUTO_TEST_CASE(constructor_registers_scheme_handler) {
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   appbase::scoped_app app;
   std::vector<const char*> argv{"test_signature_provider_kms_plugin"};
   BOOST_REQUIRE((app->initialize<signature_provider_manager_plugin, signature_provider_kms_plugin>(
      argv.size(), const_cast<char**>(argv.data()))));

   // The plugin's constructor registered the KMS handler in the process-wide sigprov registry, tagged with the plugin's
   // demangled name -- the exact string an operator passes to `plugin =`, and the string the manager gates on.
   const auto* entry = sysio::sigprov::find_scheme_handler("KMS");
   BOOST_REQUIRE(entry != nullptr);
   BOOST_CHECK_EQUAL(entry->plugin_name, "sysio::signature_provider_kms_plugin");
   BOOST_CHECK(static_cast<bool>(entry->handler));
}

BOOST_AUTO_TEST_CASE(attached_probes_run_at_manager_startup) {
   // Attaching a startup probe IS the opt-in -- there is no enable flag. A provider created with a probe (mock PROBE
   // scheme registered pre-init on the manager; the real KMS handler attaches one per created key) must have that probe
   // run at manager plugin_startup with no probe-related configuration at all.
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
