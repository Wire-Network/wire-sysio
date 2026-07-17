#include <boost/test/unit_test.hpp>

#include <gsl-lite/gsl-lite.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_ssm_plugin/signature_provider_ssm_plugin.hpp>

// ---------------------------------------------------------------------------
// Plugin lifecycle: what enabling signature_provider_ssm_plugin does. All
// offline -- no AWS, no network. The retain/claim mechanics themselves are
// covered with mock handlers in the manager's test binary
// (test_spec_retain_claim.cpp); the claim-runs-with-the-real-handler proof is
// a nodeop boot smoke, since a real claim performs the SSM fetch.
// ---------------------------------------------------------------------------

using sysio::signature_provider_manager_plugin;
using sysio::signature_provider_ssm_plugin;

BOOST_AUTO_TEST_SUITE(ssm_plugin_lifecycle)

BOOST_AUTO_TEST_CASE(initialize_registers_scheme_and_announces_plugin) {
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   appbase::scoped_app app;
   std::vector<const char*> argv{"test_signature_provider_ssm_plugin"};
   BOOST_REQUIRE((app->initialize<signature_provider_manager_plugin, signature_provider_ssm_plugin>(
      argv.size(), const_cast<char**>(argv.data()))));

   // The plugin's initialize registered the SSM handler: a second
   // registration must be rejected as a duplicate.
   auto& manager = app->get_plugin<signature_provider_manager_plugin>();
   BOOST_CHECK_EXCEPTION(
      manager.register_spec_handler("SSM",
                                    [](fc::crypto::chain_key_type_t, const fc::crypto::public_key&,
                                       std::string_view) -> sysio::provider_spec_result { return {}; }),
      fc::exception,
      [](const fc::exception& e) { return e.to_detail_string().find("already registered") != std::string::npos; });

   // The constructor announced the scheme with the plugin's demangled name --
   // the exact string an operator passes to `plugin =`.
   auto announced = sysio::sigprov::announced_scheme_plugin("SSM");
   BOOST_REQUIRE(announced.has_value());
   BOOST_CHECK_EQUAL(*announced, "sysio::signature_provider_ssm_plugin");
}

BOOST_AUTO_TEST_SUITE_END()
