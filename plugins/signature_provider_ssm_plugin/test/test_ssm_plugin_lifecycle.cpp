#include <boost/test/unit_test.hpp>

#include <gsl-lite/gsl-lite.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_ssm_plugin/signature_provider_ssm_plugin.hpp>

// --------------------------------------------------------------------------- Plugin wiring: what constructing
// signature_provider_ssm_plugin does. All offline -- no AWS, no network. The manager-side create/gate mechanics are
// covered with mock handlers in the manager's test binary (test_extension_schemes.cpp); the
// create-runs-with-the-real-handler proof is a nodeop boot smoke, since a real SSM creation performs the Parameter
// Store fetch. ---------------------------------------------------------------------------

using sysio::signature_provider_manager_plugin;
using sysio::signature_provider_ssm_plugin;

BOOST_AUTO_TEST_SUITE(ssm_plugin_lifecycle)

BOOST_AUTO_TEST_CASE(constructor_registers_scheme_handler) {
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   appbase::scoped_app app;
   std::vector<const char*> argv{"test_signature_provider_ssm_plugin"};
   BOOST_REQUIRE((app->initialize<signature_provider_manager_plugin, signature_provider_ssm_plugin>(
      argv.size(), const_cast<char**>(argv.data()))));

   // The plugin's constructor registered the SSM handler in the process-wide sigprov registry, tagged with the plugin's
   // demangled name -- the exact string an operator passes to `plugin =`, and the string the manager gates on.
   // (Construction happens as appbase builds the registered plugins.)
   const auto* entry = sysio::sigprov::find_scheme_handler("SSM");
   BOOST_REQUIRE(entry != nullptr);
   BOOST_CHECK_EQUAL(entry->plugin_name, "sysio::signature_provider_ssm_plugin");
   BOOST_CHECK(static_cast<bool>(entry->handler));
}

BOOST_AUTO_TEST_SUITE_END()
