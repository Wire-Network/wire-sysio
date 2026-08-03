#include <boost/test/unit_test.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <gsl-lite/gsl-lite.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <fc-test/crypto_utils.hpp>

#include "sig_provider_tester.hpp"

// --------------------------------------------------------------------------- Extension-scheme resolution.
//
// The manager creates every configured provider eagerly at its own plugin_initialize. A non-built-in scheme resolves
// through a handler from one of two sources:
//   - register_spec_handler() directly on the manager -- the explicit host /
//     test path, ungated.
//   - sigprov::register_scheme_handler() (a provider plugin's constructor),
//     gated on the plugin being enabled via --plugin.
// These cases drive both paths with mock handlers -- no AWS, no network. The sigprov registry is process-static, so
// each case uses a case-unique scheme name to stay independent of other cases' registrations.
// ---------------------------------------------------------------------------

using sysio::signature_provider_manager_plugin;
using namespace fc::test;
using sysio::sigprov::test::create_app;
using sysio::sigprov::test::sig_provider_tester;

namespace {

// A real registered plugin name usable as an --plugin value in this binary, to exercise the "plugin enabled" gate
// without needing a second plugin.
constexpr auto manager_plugin_name = "sysio::signature_provider_manager_plugin";

/// A handler whose provider signs with a trivial stub; records the spec_data it saw into @p seen (when non-null).
sysio::spec_handler make_stub_handler(std::string* seen = nullptr) {
   return [seen](fc::crypto::chain_key_type_t, const fc::crypto::public_key&,
                 std::string_view spec_data) -> sysio::provider_spec_result {
      if (seen)
         *seen = std::string{spec_data};
      return {.signer      = [](const fc::sha256&) { return fc::crypto::signature{}; },
              .private_key = std::nullopt};
   };
}

std::string make_spec(const std::string& name, const std::string& pubkey, const std::string& scheme,
                      const std::string& data) {
   return std::format("{},ethereum,ethereum,{},{}:{}", name, pubkey, scheme, data);
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(signature_provider_extension_schemes)

BOOST_AUTO_TEST_CASE(host_registered_scheme_creates_eagerly) {
   // A scheme registered directly on the manager (host/test path) is ungated: a matching spec is created eagerly at
   // plugin_initialize.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);

   auto  tester = std::make_unique<sig_provider_tester>();
   auto& mgr    = tester->app->_register_plugin<signature_provider_manager_plugin>();
   std::string seen;
   mgr.register_spec_handler("HOSTX", make_stub_handler(&seen));

   auto spec = make_spec("hostkey", fixture.public_key, "HOSTX", "hostdata");
   std::vector<const char*> argv{"test_signature_provider_manager_plugin", "--signature-provider", spec.c_str()};
   BOOST_REQUIRE(tester->app->initialize<signature_provider_manager_plugin>(argv.size(),
                                                                            const_cast<char**>(argv.data())));

   BOOST_CHECK(tester->plugin().has_provider("hostkey"));
   BOOST_CHECK_EQUAL(seen, "hostdata");
}

BOOST_AUTO_TEST_CASE(plugin_scheme_created_when_enabled) {
   // A scheme registered in the sigprov registry (provider-plugin path) is created when its owning plugin is enabled
   // via --plugin.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   std::string seen;
   sysio::sigprov::register_scheme_handler("PLUGON", make_stub_handler(&seen), manager_plugin_name);

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);
   auto tester = create_app(std::string{"--plugin"}, std::string{manager_plugin_name},
                            std::string{"--signature-provider"},
                            make_spec("plugonkey", fixture.public_key, "PLUGON", "pdata"));

   BOOST_CHECK(tester->plugin().has_provider("plugonkey"));
   BOOST_CHECK_EQUAL(seen, "pdata");
}

BOOST_AUTO_TEST_CASE(plugin_scheme_error_when_not_enabled) {
   // Same registry entry, but the owning plugin is NOT enabled: creation must fail at init with an error naming the
   // exact `plugin =` line to add.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   sysio::sigprov::register_scheme_handler("PLUGOFF", make_stub_handler(),
                                           "sysio::signature_provider_plugoff_plugin");

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);

   auto tester = std::make_unique<sig_provider_tester>();
   tester->app->_register_plugin<signature_provider_manager_plugin>();
   auto spec = make_spec("offkey", fixture.public_key, "PLUGOFF", "x");
   std::vector<const char*> argv{"test_signature_provider_manager_plugin", "--signature-provider", spec.c_str()};

   BOOST_CHECK_EXCEPTION(
      tester->app->initialize<signature_provider_manager_plugin>(argv.size(), const_cast<char**>(argv.data())),
      sysio::chain::plugin_config_exception, [](const auto& e) {
         auto detail = e.to_detail_string();
         return detail.find("sysio::signature_provider_plugoff_plugin") != std::string::npos &&
                detail.find("plugin = sysio::signature_provider_plugoff_plugin") != std::string::npos;
      });
}

BOOST_AUTO_TEST_CASE(unknown_scheme_error) {
   // A scheme no linked plugin provides fails at init with a "no plugin provides this" error that still names the
   // built-ins.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);

   auto tester = std::make_unique<sig_provider_tester>();
   tester->app->_register_plugin<signature_provider_manager_plugin>();
   auto spec = make_spec("nokey", fixture.public_key, "NOPEZ", "x");
   std::vector<const char*> argv{"test_signature_provider_manager_plugin", "--signature-provider", spec.c_str()};

   BOOST_CHECK_EXCEPTION(
      tester->app->initialize<signature_provider_manager_plugin>(argv.size(), const_cast<char**>(argv.data())),
      sysio::chain::plugin_config_exception, [](const auto& e) {
         auto detail = e.to_detail_string();
         return detail.find("no plugin in this binary provides") != std::string::npos &&
                detail.find("KEY and KIOD") != std::string::npos;
      });
}

BOOST_AUTO_TEST_CASE(plugin_scheme_creation_error_propagates) {
   // When the handler itself throws (bad pubkey / failed remote fetch stand-in), the error surfaces from the manager's
   // init and aborts the boot.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   sysio::sigprov::register_scheme_handler(
      "PLUGFAIL",
      [](fc::crypto::chain_key_type_t, const fc::crypto::public_key&,
         std::string_view) -> sysio::provider_spec_result {
         FC_THROW_EXCEPTION(sysio::chain::plugin_config_exception, "simulated provider creation failure");
      },
      manager_plugin_name);

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);

   auto tester = std::make_unique<sig_provider_tester>();
   tester->app->_register_plugin<signature_provider_manager_plugin>();
   auto spec = make_spec("failkey", fixture.public_key, "PLUGFAIL", "x");
   std::vector<const char*> argv{"test_signature_provider_manager_plugin", "--plugin", manager_plugin_name,
                                 "--signature-provider", spec.c_str()};

   BOOST_CHECK_THROW(
      tester->app->initialize<signature_provider_manager_plugin>(argv.size(), const_cast<char**>(argv.data())),
      sysio::chain::plugin_config_exception);
   BOOST_CHECK(!tester->plugin().has_provider("failkey"));
}

BOOST_AUTO_TEST_CASE(scheme_handler_registry_is_queryable_and_idempotent) {
   sysio::sigprov::register_scheme_handler("REGX", make_stub_handler(), "sysio::first_plugin");
   // Re-registration by the SAME plugin is idempotent (repeated plugin construction across scoped_app instances does
   // exactly this): no throw, the entry remains.
   sysio::sigprov::register_scheme_handler("REGX", make_stub_handler(), "sysio::first_plugin");

   const auto* entry = sysio::sigprov::find_scheme_handler("REGX");
   BOOST_REQUIRE(entry != nullptr);
   BOOST_CHECK_EQUAL(entry->plugin_name, "sysio::first_plugin");

   // A DIFFERENT plugin claiming an already-registered scheme is a wiring bug and fails loudly rather than shadowing.
   BOOST_CHECK_EXCEPTION(
      sysio::sigprov::register_scheme_handler("REGX", make_stub_handler(), "sysio::second_plugin"), fc::exception,
      [](const auto& e) { return e.to_detail_string().find("already registered by plugin") != std::string::npos; });

   BOOST_CHECK(sysio::sigprov::find_scheme_handler("REGNEVER") == nullptr);
}

BOOST_AUTO_TEST_CASE(register_spec_handler_rejects_after_startup) {
   // The immutable-after-startup contract still holds for the public mutator.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   auto tester = create_app(std::vector<std::string>{});
   tester->plugin().startup();

   BOOST_CHECK_EXCEPTION(tester->plugin().register_spec_handler("LATE", make_stub_handler()),
                         sysio::chain::plugin_config_exception,
                         [](const auto& e) { return e.to_detail_string().find("immutable") != std::string::npos; });
}

BOOST_AUTO_TEST_SUITE_END()
