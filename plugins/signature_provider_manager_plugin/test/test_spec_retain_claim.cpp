#include <boost/test/unit_test.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/filesystem.hpp>
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

// ---------------------------------------------------------------------------
// Retain-and-claim mechanics.
//
// plugin_initialize retains (rather than throws on) any --signature-provider
// spec whose scheme has no registered handler at parse time; the scheme's
// provider plugin later claims those specs with
// create_configured_providers(scheme). These cases drive that machinery with
// mock handlers -- no AWS, no network. NOTE: the scheme->plugin announcement
// registry is deliberately process-static (it describes the binary, not one
// application instance), so every case uses a case-unique scheme name to stay
// independent of announcement state left by other cases.
// ---------------------------------------------------------------------------

using sysio::signature_provider_manager_plugin;
using namespace fc::test;
using sysio::sigprov::test::create_app;
using sysio::sigprov::test::sig_provider_tester;

namespace {

/**
 * A spec handler whose provider signs with a trivial stub. Captures the
 * `spec_data` it was invoked with into @p seen_spec_data (when non-null) so
 * cases can assert the handler actually ran, and against what.
 */
sysio::spec_handler make_stub_handler(std::string* seen_spec_data = nullptr) {
   return [seen_spec_data](fc::crypto::chain_key_type_t, const fc::crypto::public_key&,
                           std::string_view spec_data) -> sysio::provider_spec_result {
      if (seen_spec_data) {
         *seen_spec_data = std::string{spec_data};
      }
      return {.signer      = [](const fc::sha256&) { return fc::crypto::signature{}; },
              .private_key = std::nullopt};
   };
}

/**
 * Build the 5-field spec `<name>,ethereum,ethereum,<pubkey>,<scheme>:<data>`
 * for the given fixture public key.
 */
std::string make_spec(const std::string& name, const std::string& pubkey, const std::string& scheme,
                      const std::string& data) {
   return std::format("{},ethereum,ethereum,{},{}:{}", name, pubkey, scheme, data);
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(signature_provider_spec_retain_claim)

BOOST_AUTO_TEST_CASE(retained_spec_is_created_by_claim_not_by_registration) {
   // A spec whose scheme is unregistered at parse time must survive
   // initialize (retained, not thrown), must NOT be created by
   // register_spec_handler alone (registration is a pure map insert), and
   // must be created by create_configured_providers.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);
   auto tester = create_app(std::string{"--signature-provider"},
                            make_spec("t1key", fixture.public_key, "T1LATE", "t1data"));

   BOOST_CHECK(!tester->plugin().has_provider("t1key"));

   std::string seen_spec_data;
   tester->plugin().register_spec_handler("T1LATE", make_stub_handler(&seen_spec_data));
   BOOST_CHECK(!tester->plugin().has_provider("t1key")); // registration alone creates nothing

   auto created = tester->plugin().create_configured_providers("T1LATE");
   BOOST_REQUIRE_EQUAL(created.size(), 1u);
   BOOST_CHECK_EQUAL(created[0]->key_name, "t1key");
   BOOST_CHECK_EQUAL(seen_spec_data, "t1data");
   BOOST_CHECK(tester->plugin().has_provider("t1key"));

   // The claim consumed the spec: startup's accounting pass has nothing left
   // to complain about, and a second claim finds nothing.
   BOOST_CHECK(tester->plugin().create_configured_providers("T1LATE").empty());
   BOOST_CHECK_NO_THROW(tester->plugin().plugin_startup());
}

BOOST_AUTO_TEST_CASE(claim_consumes_only_its_own_scheme) {
   // Two retained schemes; claiming one must leave the other retained (and
   // startup must still fail over the unclaimed one).
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   keygen_result fixture_a = fc::test::load_keygen_fixture("ethereum", 1);
   keygen_result fixture_b = fc::test::load_keygen_fixture("ethereum", 2);
   auto tester = create_app(std::string{"--signature-provider"},
                            make_spec("t2akey", fixture_a.public_key, "T2A", "a"),
                            std::string{"--signature-provider"},
                            make_spec("t2bkey", fixture_b.public_key, "T2B", "b"));

   tester->plugin().register_spec_handler("T2A", make_stub_handler());
   BOOST_REQUIRE_EQUAL(tester->plugin().create_configured_providers("T2A").size(), 1u);
   BOOST_CHECK(tester->plugin().has_provider("t2akey"));
   BOOST_CHECK(!tester->plugin().has_provider("t2bkey"));

   BOOST_CHECK_EXCEPTION(tester->plugin().plugin_startup(), sysio::chain::plugin_config_exception,
                         [](const auto& e) {
                            auto detail = e.to_detail_string();
                            return detail.find("T2B") != std::string::npos &&
                                   detail.find("t2akey") == std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(claim_creation_error_propagates_from_claim_call) {
   // A retained spec whose creation fails (here: the handler throws, standing
   // in for a bad pubkey / failed remote fetch) must surface the error from
   // create_configured_providers -- i.e. from the claiming plugin's
   // initialize in production -- not from some later pass.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);
   auto tester = create_app(std::string{"--signature-provider"},
                            make_spec("t3key", fixture.public_key, "T3FAIL", "x"));

   tester->plugin().register_spec_handler(
      "T3FAIL", [](fc::crypto::chain_key_type_t, const fc::crypto::public_key&,
                   std::string_view) -> sysio::provider_spec_result {
         FC_THROW_EXCEPTION(sysio::chain::plugin_config_exception, "simulated provider creation failure");
      });

   BOOST_CHECK_THROW(tester->plugin().create_configured_providers("T3FAIL"),
                     sysio::chain::plugin_config_exception);
   BOOST_CHECK(!tester->plugin().has_provider("t3key"));
}

BOOST_AUTO_TEST_CASE(pre_init_registered_scheme_creates_eagerly) {
   // The host-main() path: a handler registered BEFORE app initialization
   // creates its specs eagerly at parse time -- no retained spec, no claim
   // step, exactly the pre-plugin behavior.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);

   auto  tester = std::make_unique<sig_provider_tester>();
   auto& mgr    = tester->app->_register_plugin<signature_provider_manager_plugin>();
   std::string seen_spec_data;
   mgr.register_spec_handler("T4EAGER", make_stub_handler(&seen_spec_data));

   auto spec = make_spec("t4key", fixture.public_key, "T4EAGER", "t4data");
   std::vector<const char*> argv{"test_signature_provider_manager_plugin", "--signature-provider", spec.c_str()};
   BOOST_REQUIRE(tester->app->initialize<signature_provider_manager_plugin>(argv.size(),
                                                                            const_cast<char**>(argv.data())));

   BOOST_CHECK(tester->plugin().has_provider("t4key"));
   BOOST_CHECK_EQUAL(seen_spec_data, "t4data");
   BOOST_CHECK(tester->plugin().create_configured_providers("T4EAGER").empty());
   BOOST_CHECK_NO_THROW(tester->plugin().plugin_startup());
}

BOOST_AUTO_TEST_CASE(unclaimed_spec_startup_error_names_announced_plugin) {
   // The operator-forgot-the-plugin-line case: the scheme was announced by a
   // (not enabled) plugin, so the startup error must name that plugin and the
   // exact `plugin =` remediation.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   sysio::sigprov::announce_scheme_plugin("T5ANN", "sysio::signature_provider_t5_plugin");

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);
   auto tester = create_app(std::string{"--signature-provider"},
                            make_spec("t5key", fixture.public_key, "T5ANN", "x"));

   BOOST_CHECK_EXCEPTION(tester->plugin().plugin_startup(), sysio::chain::plugin_config_exception,
                         [](const auto& e) {
                            auto detail = e.to_detail_string();
                            return detail.find("sysio::signature_provider_t5_plugin") != std::string::npos &&
                                   detail.find("plugin = sysio::signature_provider_t5_plugin") != std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(unclaimed_spec_startup_error_unknown_scheme) {
   // No plugin announced the scheme: the error must say nothing in this
   // binary provides it (and still name the built-ins).
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);
   auto tester = create_app(std::string{"--signature-provider"},
                            make_spec("t6key", fixture.public_key, "T6NOBODY", "x"));

   BOOST_CHECK_EXCEPTION(tester->plugin().plugin_startup(), sysio::chain::plugin_config_exception,
                         [](const auto& e) {
                            auto detail = e.to_detail_string();
                            return detail.find("no plugin in this binary provides this scheme") !=
                                      std::string::npos &&
                                   detail.find("KEY and KIOD") != std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(unclaimed_spec_startup_error_registered_but_never_claimed) {
   // A plugin bug shape: the handler IS registered, but nobody claimed the
   // retained specs. Distinct diagnosis pointing at the missing
   // create_configured_providers call.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);
   auto tester = create_app(std::string{"--signature-provider"},
                            make_spec("t7key", fixture.public_key, "T7HALF", "x"));

   tester->plugin().register_spec_handler("T7HALF", make_stub_handler());

   BOOST_CHECK_EXCEPTION(tester->plugin().plugin_startup(), sysio::chain::plugin_config_exception,
                         [](const auto& e) {
                            return e.to_detail_string().find("create_configured_providers(\"T7HALF\")") !=
                                   std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(mutators_rejected_after_startup) {
   // The provider set is immutable after startup (that is what lets the
   // manager's containers go unsynchronized while runtime threads read
   // them): once the plugin reaches the started state, every public mutator
   // must be rejected at the boundary.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   fc::test::keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);
   auto tester = create_app(std::vector<std::string>{});

   // The state-transitioning wrapper (dependency-ordered, as production
   // startup runs it), not the bare plugin_startup() body.
   tester->plugin().startup();

   auto expect_immutable = [](const fc::exception& e) {
      return e.to_detail_string().find("immutable") != std::string::npos;
   };
   BOOST_CHECK_EXCEPTION(tester->plugin().register_spec_handler("T10LATE", make_stub_handler()),
                         sysio::chain::plugin_config_exception, expect_immutable);
   BOOST_CHECK_EXCEPTION(tester->plugin().create_provider(
                            make_spec("t10key", fixture.public_key, "T10LATE", "x")),
                         sysio::chain::plugin_config_exception, expect_immutable);
   BOOST_CHECK_EXCEPTION(tester->plugin().create_configured_providers("T10LATE"),
                         sysio::chain::plugin_config_exception, expect_immutable);
   BOOST_CHECK_EXCEPTION(tester->plugin().register_default_signature_providers({fc::crypto::chain_key_type_wire}),
                         sysio::chain::plugin_config_exception, expect_immutable);
}

BOOST_AUTO_TEST_CASE(announcement_registry_is_idempotent_and_queryable) {
   sysio::sigprov::announce_scheme_plugin("T8SCHEME", "sysio::first_plugin");
   // Re-announce (as repeated plugin construction across scoped_app instances
   // does): insert-or-assign, the latest wins, no throw.
   sysio::sigprov::announce_scheme_plugin("T8SCHEME", "sysio::second_plugin");

   auto announced = sysio::sigprov::announced_scheme_plugin("T8SCHEME");
   BOOST_REQUIRE(announced.has_value());
   BOOST_CHECK_EQUAL(*announced, "sysio::second_plugin");

   BOOST_CHECK(!sysio::sigprov::announced_scheme_plugin("T8NEVER").has_value());
}

BOOST_AUTO_TEST_CASE(default_generation_guard_trips_before_persisting_keys) {
   // A retained spec dooms the boot; register_default_signature_providers
   // (chain_plugin's initialize path) must fail with the unclaimed-spec error
   // BEFORE generating -- and persisting to default_signature_providers.json
   // -- an anonymous default key that later boots would silently re-load.
   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });

   fc::temp_directory config_dir;
   keygen_result      fixture = fc::test::load_keygen_fixture("ethereum", 1);
   auto tester = create_app(std::string{"--config-dir"}, config_dir.path().string(),
                            std::string{"--signature-provider"},
                            make_spec("t9key", fixture.public_key, "T9GUARD", "x"));

   BOOST_CHECK_THROW(tester->plugin().register_default_signature_providers({fc::crypto::chain_key_type_wire}),
                     sysio::chain::plugin_config_exception);
   BOOST_CHECK(!std::filesystem::exists(config_dir.path() / "default_signature_providers.json"));
   BOOST_CHECK(tester->plugin().query_providers(std::nullopt, std::nullopt, fc::crypto::chain_key_type_wire).empty());
}

BOOST_AUTO_TEST_SUITE_END()
