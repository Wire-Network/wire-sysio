#include <boost/test/unit_test.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sodium.h>
#include <string>
#include <type_traits>
#include <vector>

#include <gsl-lite/gsl-lite.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <fc-test/build_info.hpp>
#include <fc-test/crypto_utils.hpp>

#include "sig_provider_tester.hpp"

using sysio::signature_provider_manager_plugin;
using sysio::chain::private_key_type;
using sysio::chain::public_key_type;
using namespace fc::test;
using sysio::sigprov::test::create_app;
using sysio::sigprov::test::sig_provider_tester;

namespace {

/**
 * Build and initialize a tester with a `PROBE:` spec handler whose returned provider carries `probe_body` as its
 * `startup_probe`. The handler's signer is a trivial stub (never invoked by these tests).
 *
 * The handler is registered via `_register_plugin<>` BEFORE `initialize<>`, so it is in place by the time any provider
 * spec is parsed -- mirroring how a host application registers a real extension handler in `main()`.
 *
 * @param probe_body callback the registered provider's startup probe runs
 * @return an initialized tester ready for `create_provider` / `plugin_startup`
 */
std::unique_ptr<sig_provider_tester> make_probe_tester(std::function<void()> probe_body) {
   auto  tester = std::make_unique<sig_provider_tester>();
   auto& mgr    = tester->app->_register_plugin<signature_provider_manager_plugin>();
   mgr.register_spec_handler(
      "PROBE",
      [body = std::move(probe_body)](fc::crypto::chain_key_type_t, const fc::crypto::public_key&,
                                     std::string_view) -> sysio::provider_spec_result {
         return {.signer        = [](const fc::sha256&) { return fc::crypto::signature{}; },
                 .private_key   = std::nullopt,
                 .startup_probe = body};
      });

   std::vector<const char*> argv{"test_signature_provider_manager_plugin"};
   BOOST_CHECK(tester->app->initialize<signature_provider_manager_plugin>(
      argv.size(), const_cast<char**>(argv.data())));
   return tester;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(signature_provider_manager_create_provider_specs)

BOOST_AUTO_TEST_CASE(create_provider_wire_key_from_example_spec) {
   using namespace fc::crypto;

   auto priv = fc::crypto::private_key::generate();
   auto pub  = priv.get_public_key();

   auto private_key_spec = to_private_key_spec(priv.to_string({}));
   auto provider_spec    =
      fc::crypto::to_signature_provider_spec("wire_key-1", chain_kind_wire, chain_key_type_wire, pub.to_string({}), private_key_spec);
   auto  tester = create_app();
   auto& mgr    = tester->plugin();

   auto provider = mgr.create_provider(provider_spec);

   // A named provider created through the public API was not supplied through
   // --signature-provider and must not satisfy another plugin's config reference.
   BOOST_CHECK(!mgr.is_explicitly_configured_provider(provider->key_name));

   // Public key should match the one provided in spec
   BOOST_CHECK_EQUAL(provider->public_key.to_string({}), pub.to_string({}));
   BOOST_CHECK_EQUAL(provider->public_key, pub);
   BOOST_TEST((provider->key_type == fc::crypto::chain_key_type_wire));

   // Provider should be retrievable via its public key
   BOOST_CHECK(mgr.has_provider(provider->public_key));
   auto found = mgr.get_provider(provider->public_key);
   BOOST_CHECK_EQUAL(found->public_key, pub);
   BOOST_CHECK_EQUAL(found->public_key.to_string({}), pub.to_string({}));

   // Sign function should be set
   BOOST_CHECK(static_cast<bool>(provider->sign));
}

BOOST_AUTO_TEST_CASE(create_provider_ethereum_fixture_pub_priv_sig_interoperable) {

   // Load fixture
   keygen_result fixture = load_keygen_fixture("ethereum", 1);

   // Create private key from fixture
   auto fixture_priv_key_bytes = fc::from_hex(fc::crypto::ethereum::trim(fixture.private_key));

   auto em_priv_key = fc::em::private_key::regenerate(
      fc::sha256(reinterpret_cast<const char*>(fixture_priv_key_bytes.data()), fixture_priv_key_bytes.size()).to_uint64_array());

   auto em_sig_data = em_priv_key.sign_compact(fc::crypto::keccak256::hash(fixture.payload));
   auto em_sig      = fc::to_hex(reinterpret_cast<const char*>(em_sig_data.data()), em_sig_data.size());

   // Compare generated signature against fixture
   auto fixture_sig = fc::crypto::ethereum::trim(fixture.signature);
   BOOST_CHECK_EQUAL(em_sig, fixture_sig);

   // Recover public key data (uncompressed)
   auto em_pub_key_rec_ser =
      fc::em::signature_shim(em_sig_data).recover_eth(fc::crypto::keccak256::hash(fixture.payload)).unwrapped().serialize_uncompressed();

   auto em_pub_key_rec_hex =
      fc::crypto::ethereum::trim_public_key(fc::to_hex(em_pub_key_rec_ser.data(), em_pub_key_rec_ser.size()));

   auto fixture_pub_key_stripped = fc::crypto::ethereum::trim_public_key(fixture.public_key);
   BOOST_CHECK_EQUAL(em_pub_key_rec_hex, fixture_pub_key_stripped);

   // Create pub key from fixture
   auto fixture_pub_key_bytes = fc::from_hex(fixture_pub_key_stripped);

   fc::em::public_key em_pub_key_parsed      = fc::crypto::ethereum::to_em_public_key(fixture_pub_key_stripped);
   auto               em_pub_key_parsed_data = em_pub_key_parsed.serialize();

   auto em_pub_key      = em_priv_key.get_public_key();
   auto em_pub_key_data = em_pub_key.serialize();

   // Compare pub key from parsing fixture to the
   // private key provided public key
   BOOST_CHECK(em_pub_key == em_pub_key_parsed);

   // Redundant, but checks the encoding of pub keys too
   BOOST_CHECK(fc::to_hex(em_pub_key_data.data(), em_pub_key_data.size()) ==
      fc::to_hex(em_pub_key_parsed_data.data(), em_pub_key_parsed_data.size()));
}

BOOST_AUTO_TEST_CASE(create_provider_ethereum_key_spec) {
   using namespace fc::test;
   using namespace fc::crypto;
   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });
   // Load fixture
   keygen_result fixture          = load_keygen_fixture("ethereum", 1);
   auto          fixture_spec     = keygen_fixture_to_spec("ethereum", 1);
   auto          private_key_spec = to_private_key_spec(fixture.private_key);

   // TODO: Now parse and create signature provider
   auto key_type_eth_str = chain_key_type_reflector::to_fc_string(chain_key_type_ethereum);
   BOOST_CHECK_EQUAL(key_type_eth_str, "ethereum");

   auto  tester = create_app();
   auto& mgr    = tester->plugin();

   auto provider =
      mgr.create_provider(fixture.key_name, chain_kind_ethereum, chain_key_type_ethereum, fixture.public_key,
                          private_key_spec);


   // Provider should be retrievable
   BOOST_CHECK(mgr.has_provider(provider->public_key));
   auto found = mgr.get_provider(provider->public_key);
   BOOST_CHECK_EQUAL(found->public_key.to_string({}), provider->public_key.to_string({}));

   // Sign function should be set
   BOOST_CHECK(static_cast<bool>(provider->sign));
}

BOOST_AUTO_TEST_CASE(ethereum_signature_provider_spec_options) {
   using namespace fc::test;
   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });
   using namespace fc::crypto;
   // Load fixture
   keygen_result fixture1      = load_keygen_fixture("ethereum", 1);
   keygen_result fixture2      = load_keygen_fixture("ethereum", 2);
   auto          fixture_spec1 = keygen_fixture_to_spec("ethereum", 1);
   auto          fixture_spec2 = keygen_fixture_to_spec("ethereum", 2);

   std::vector<std::string> args = {
      "--signature-provider", fixture_spec1, "--signature-provider", fixture_spec2};
   auto  tester = create_app(args);
   auto& mgr    = tester->plugin();

   auto all_providers = mgr.query_providers(std::nullopt, fc::crypto::chain_kind_ethereum);
   BOOST_CHECK(all_providers.size() >= 2);

   // Provider 1 should be retrievable
   BOOST_CHECK(!mgr.query_providers(fixture1.key_name).empty());
   BOOST_CHECK(mgr.is_explicitly_configured_provider(fixture1.key_name));
   // Provider 2 should be retrievable
   BOOST_CHECK(!mgr.query_providers(fixture2.key_name).empty());
   BOOST_CHECK(mgr.is_explicitly_configured_provider(fixture2.key_name));
}

BOOST_AUTO_TEST_CASE(wire_signature_provider_spec_options) {
   using namespace fc::test;
   using namespace fc::crypto;

   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });

   // Load fixture
   keygen_result fixture1      = load_keygen_fixture("wire", 1);
   auto          fixture_spec1 = keygen_fixture_to_spec("wire", 1);

   std::vector<std::string> args = {
      "--signature-provider", fixture_spec1};
   auto  tester = create_app(args);
   auto& mgr    = tester->plugin();

   auto all_providers = mgr.query_providers(std::nullopt, fc::crypto::chain_kind_wire);
   BOOST_CHECK(all_providers.size() >= 1);

   // Provider 1 should be retrievable
   BOOST_CHECK(!mgr.query_providers(fixture1.key_name).empty());

}

BOOST_AUTO_TEST_CASE(create_provider_solana_fixture_pub_priv_sig_interoperable) {
   using namespace fc::crypto;

   // Load fixture - all keys are base58 encoded
   keygen_result fixture = load_keygen_fixture("solana", 1);

   // The fixture private key is 64-byte (seed + pubkey) in base58
   // Parse the private key from base58
   ed::private_key_shim ed_priv_key = ed::private_key_shim::from_base58_string(fixture.private_key);

   // Parse the public key from base58
   ed::public_key_shim ed_pub_key = ed::public_key_shim::from_base58_string(fixture.public_key);

   // Verify the derived public key matches the fixture public key
   auto derived_pub = ed_priv_key.get_public_key();
   BOOST_CHECK(derived_pub.serialize() == ed_pub_key.serialize());

   // Verify the base58 address encoding matches
   auto ed_pub_key_base58 = ed_pub_key.to_string({});
   BOOST_CHECK_EQUAL(ed_pub_key_base58, fixture.address);
   BOOST_CHECK_EQUAL(ed_pub_key_base58, fixture.public_key);

   // Decode the raw 64-byte fixture signature from base58
   auto raw_sig_bytes = fc::from_base58(fixture.signature);
   BOOST_REQUIRE_EQUAL(raw_sig_bytes.size(), crypto_sign_BYTES);

   // Build the full signature blob: [pubkey 32B][sig 64B]
   ed::signature_shim fixture_sig;
   memcpy(fixture_sig._data.data(), ed_pub_key._data.data(), crypto_sign_PUBLICKEYBYTES);
   memcpy(fixture_sig._data.data() + crypto_sign_PUBLICKEYBYTES, raw_sig_bytes.data(), crypto_sign_BYTES);

   // Verify the fixture signature against the raw payload using libsodium directly
   // (Python keygen signs raw bytes, our C++ signs SHA256 hash)
   int verify_result = crypto_sign_verify_detached(
      fixture_sig._data.data() + crypto_sign_PUBLICKEYBYTES,
      reinterpret_cast<const unsigned char*>(fixture.payload.data()),
      fixture.payload.size(),
      ed_pub_key._data.data());
   BOOST_CHECK_EQUAL(verify_result, 0);  // 0 means valid signature
}

BOOST_AUTO_TEST_CASE(create_provider_solana_key_spec) {
   using namespace fc::test;
   using namespace fc::crypto;
   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });

   // Load fixture
   keygen_result fixture          = load_keygen_fixture("solana", 1);
   auto          fixture_spec     = keygen_fixture_to_spec("solana", 1);
   auto          private_key_spec = to_private_key_spec(fixture.private_key);

   // Verify chain type string conversion
   auto key_type_sol_str = chain_key_type_reflector::to_fc_string(chain_key_type_solana);
   BOOST_CHECK_EQUAL(key_type_sol_str, "solana");

   auto  tester = create_app();
   auto& mgr    = tester->plugin();

   auto provider =
      mgr.create_provider(fixture.key_name, chain_kind_solana, chain_key_type_solana, fixture.public_key,
                          private_key_spec);

   // Provider should be retrievable
   BOOST_CHECK(mgr.has_provider(provider->public_key));
   auto found = mgr.get_provider(provider->public_key);
   BOOST_CHECK_EQUAL(found->public_key.to_string({}), provider->public_key.to_string({}));
   BOOST_TEST((found->key_type == chain_key_type_solana));

   // Sign function should be set
   BOOST_CHECK(static_cast<bool>(provider->sign));
}

BOOST_AUTO_TEST_CASE(solana_signature_provider_spec_options) {
   using namespace fc::test;
   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });
   using namespace fc::crypto;

   // Load fixture
   keygen_result fixture1      = load_keygen_fixture("solana", 1);
   auto          fixture_spec1 = keygen_fixture_to_spec("solana", 1);

   std::vector<std::string> args = {
      "--signature-provider", fixture_spec1};
   auto  tester = create_app(args);
   auto& mgr    = tester->plugin();

   auto all_providers = mgr.query_providers(std::nullopt, fc::crypto::chain_kind_solana);
   BOOST_CHECK(all_providers.size() >= 1);

   // Provider 1 should be retrievable
   BOOST_CHECK(!mgr.query_providers(fixture1.key_name).empty());

   // Verify the provider has correct key type
   auto providers = mgr.query_providers(fixture1.key_name);
   BOOST_REQUIRE(!providers.empty());
   BOOST_TEST((providers[0]->key_type == chain_key_type_solana));
}

BOOST_AUTO_TEST_CASE(create_provider_unknown_scheme_throws_with_hint) {
   // Any `<provider-type>:` not built in (KEY, KIOD) and not registered by
   // the host application must throw with a clear hint about how to enable
   // it -- this is the operator-facing surface for a binary that does not
   // link the relevant extension library.
   using namespace fc::crypto;

   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });

   keygen_result fixture          = fc::test::load_keygen_fixture("ethereum", 1);
   const std::string kms_spec     = "KMS:us-east-1:alias/none";
   const auto provider_spec       = to_signature_provider_spec(
      "kms-eth-01", chain_kind_ethereum, chain_key_type_ethereum,
      fixture.public_key, kms_spec);

   auto  tester = create_app();
   auto& mgr    = tester->plugin();

   BOOST_CHECK_THROW(mgr.create_provider(provider_spec),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(register_spec_handler_dispatches_custom_scheme) {
   // The extension API: a host application registers a handler for a custom
   // scheme via `register_spec_handler` before app().initialize(); the
   // plugin's spec parser then routes that scheme through the handler. This
   // test uses a mock handler -- no AWS, no network -- to exercise the
   // wiring end-to-end and verify the returned provider is in the registry.
   using namespace fc::crypto;

   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });

   keygen_result fixture     = fc::test::load_keygen_fixture("ethereum", 1);
   const auto provider_spec  = to_signature_provider_spec(
      "mock-eth-01", chain_kind_ethereum, chain_key_type_ethereum,
      fixture.public_key, "MOCK:anything");

   // Build a tester *without* initializing; we need to register a handler in
   // the gap between plugin construction and initialize (which parses
   // --signature-provider options). `register_plugin<>` (static) only
   // enqueues a name in the static registration list, so call
   // `_register_plugin<>` directly to construct the instance now --
   // `_register_plugin<>` is idempotent, so the later `initialize<>` pass is
   // a no-op for this plugin.
   auto tester = std::make_unique<sig_provider_tester>();
   auto& mgr   = tester->app->_register_plugin<signature_provider_manager_plugin>();

   bool handler_called = false;
   mgr.register_spec_handler(
      "MOCK",
      [&handler_called](chain_key_type_t /*key_type*/,
                        const public_key& /*expected*/,
                        std::string_view spec_data) -> sysio::provider_spec_result {
         handler_called = true;
         BOOST_CHECK_EQUAL(spec_data, "anything");
         // Trivial signer: returns a default-constructed signature. The
         // plugin never invokes it in this test -- we only verify routing.
         return {
            .signer        = [](const fc::sha256&) { return fc::crypto::signature{}; },
            .private_key   = std::nullopt,
            .startup_probe = {}
         };
      });

   std::vector<const char*> argv{"test_signature_provider_manager_plugin"};
   BOOST_CHECK(tester->app->initialize<signature_provider_manager_plugin>(
      argv.size(), const_cast<char**>(argv.data())));

   const auto provider = tester->plugin().create_provider(provider_spec);
   BOOST_CHECK(handler_called);
   BOOST_CHECK_EQUAL(provider->key_name, "mock-eth-01");
   BOOST_CHECK(static_cast<bool>(provider->sign));
   BOOST_CHECK(!provider->private_key.has_value());
}

BOOST_AUTO_TEST_CASE(register_spec_handler_rejects_builtin_and_duplicates) {
   // The extension API must refuse to override built-ins and refuse to
   // re-register the same scheme twice -- both are operator-facing bugs that
   // would otherwise yield baffling runtime behaviour.
   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });

   // `register_plugin<>` only enqueues the plugin name; the instance is not
   // constructed until `initialize<>` runs. Use `_register_plugin<>` to
   // construct the instance now so we can call `register_spec_handler` on
   // it.
   auto tester = std::make_unique<sig_provider_tester>();
   auto& mgr   = tester->app->_register_plugin<signature_provider_manager_plugin>();

   sysio::spec_handler noop_handler =
      [](fc::crypto::chain_key_type_t, const fc::crypto::public_key&, std::string_view) {
         return sysio::provider_spec_result{};
      };

   BOOST_CHECK_THROW(mgr.register_spec_handler("KEY", noop_handler),
                     fc::exception);
   BOOST_CHECK_THROW(mgr.register_spec_handler("KIOD", noop_handler),
                     fc::exception);

   BOOST_CHECK_NO_THROW(mgr.register_spec_handler("TEST", noop_handler));
   BOOST_CHECK_THROW(mgr.register_spec_handler("TEST", noop_handler),
                     fc::exception);
}

// ---------------------------------------------------------------------------
// Startup-probe pass (plugin_startup -> run_startup_probes)
//
// A spec handler may attach a `startup_probe` to its result; the plugin runs every attached probe from
// plugin_startup() unconditionally -- attaching a probe IS the opt-in, there is no enable flag. These cases drive that
// machinery with a mock handler -- no AWS, no network -- to pin its control flow: transient failures are deferred,
// permanent failures abort startup, and the probe list is one-shot.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(startup_probe_transient_failure_is_deferred_not_fatal) {
   // A transient probe failure (e.g. KMS throttle / KMSInternal / timeout) is
   // not a misconfiguration: plugin_startup() must log it and return normally,
   // leaving the lazy first-sign check to retry. The probe still runs once.
   using namespace fc::crypto;
   auto clean_app = gsl_lite::finally([]() { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);

   int  probe_calls = 0;
   auto tester      = make_probe_tester(
      [&probe_calls] {
         ++probe_calls;
         FC_THROW_EXCEPTION(sysio::chain::signing_transient_exception,
                            "simulated transient signing-provider failure");
      });

   tester->plugin().create_provider("probe-transient", chain_kind_ethereum, chain_key_type_ethereum,
                                    fixture.public_key, "PROBE:x");

   BOOST_CHECK_NO_THROW(tester->plugin().plugin_startup());
   BOOST_CHECK_EQUAL(probe_calls, 1); // probe ran (threw transient, was swallowed)
}

BOOST_AUTO_TEST_CASE(startup_probe_permanent_failure_aborts_startup) {
   // A permanent probe failure (bad credentials / region / IAM / pinned key)
   // must propagate out of plugin_startup() to abort node startup loudly,
   // rather than waiting to fail on the first production sign.
   using namespace fc::crypto;
   auto clean_app = gsl_lite::finally([]() { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);

   int  probe_calls = 0;
   auto tester      = make_probe_tester(
      [&probe_calls] {
         ++probe_calls;
         FC_THROW_EXCEPTION(sysio::chain::plugin_config_exception,
                            "simulated permanent signing-provider misconfiguration");
      });

   tester->plugin().create_provider("probe-permanent", chain_kind_ethereum, chain_key_type_ethereum,
                                    fixture.public_key, "PROBE:x");

   BOOST_CHECK_THROW(tester->plugin().plugin_startup(), sysio::chain::plugin_config_exception);
   BOOST_CHECK_EQUAL(probe_calls, 1); // probe ran (threw permanent, propagated)
}

BOOST_AUTO_TEST_CASE(startup_probes_are_one_shot) {
   // The probe list is drained on the first plugin_startup(); a second call
   // re-runs nothing, so the startup check cannot fire twice for one provider.
   using namespace fc::crypto;
   auto clean_app = gsl_lite::finally([]() { appbase::application::reset_app_singleton(); });

   keygen_result fixture = fc::test::load_keygen_fixture("ethereum", 1);

   int  probe_calls = 0;
   auto tester      = make_probe_tester([&probe_calls] { ++probe_calls; });

   tester->plugin().create_provider("probe-once", chain_kind_ethereum, chain_key_type_ethereum,
                                    fixture.public_key, "PROBE:x");

   BOOST_CHECK_NO_THROW(tester->plugin().plugin_startup());
   BOOST_CHECK_EQUAL(probe_calls, 1);
   BOOST_CHECK_NO_THROW(tester->plugin().plugin_startup()); // list already drained
   BOOST_CHECK_EQUAL(probe_calls, 1);                       // not re-run
}

BOOST_AUTO_TEST_CASE(startup_probe_not_retained_for_rejected_duplicate_provider) {
   // create_provider() appends the startup_probe ONLY after set_provider()
   // succeeds. A provider rejected as a duplicate (same key_name) must leave
   // no orphan probe behind, so plugin_startup() runs exactly one probe -- the
   // surviving provider's -- not two.
   using namespace fc::crypto;
   auto clean_app = gsl_lite::finally([]() { appbase::application::reset_app_singleton(); });

   keygen_result fixture1 = fc::test::load_keygen_fixture("ethereum", 1);
   keygen_result fixture2 = fc::test::load_keygen_fixture("ethereum", 2);

   int  probe_calls = 0;
   auto tester      = make_probe_tester([&probe_calls] { ++probe_calls; });
   auto& plug = tester->plugin();

   // Provider #1 succeeds -> its probe is retained.
   plug.create_provider("dup-name", chain_kind_ethereum, chain_key_type_ethereum,
                        fixture1.public_key, "PROBE:a");

   // Provider #2 reuses the same key_name (different public key) -> set_provider
   // throws a duplicate; the handler already built a probe for it, which must
   // be dropped rather than retained.
   BOOST_CHECK_THROW(plug.create_provider("dup-name", chain_kind_ethereum, chain_key_type_ethereum,
                                          fixture2.public_key, "PROBE:b"),
                     sysio::chain::plugin_config_exception);

   BOOST_CHECK_NO_THROW(plug.plugin_startup());
   BOOST_CHECK_EQUAL(probe_calls, 1); // only the surviving provider's probe ran
}

// A signature-provider spec must never be logged with its inline private key intact; `redact_signature_provider_spec`
// masks only the final `KEY:<private-key>` field while leaving name/chain/type/public-key and non-KEY providers
// (which reference external key material) untouched.
BOOST_AUTO_TEST_CASE(redact_signature_provider_spec_masks_inline_private_key) {
   using sysio::redact_signature_provider_spec;

   // Full CSV spec with an inline KEY: private key -> only the private key is masked.
   BOOST_CHECK_EQUAL(
      redact_signature_provider_spec("wire-1,wire,wire,PUB_WA_pub,KEY:PVT_WA_secretkey"),
      "wire-1,wire,wire,PUB_WA_pub,KEY:<redacted>");

   // Ethereum-style hex key (no ':') is still fully masked.
   BOOST_CHECK_EQUAL(
      redact_signature_provider_spec("eth-1,ethereum,ethereum,0xabc,KEY:0xdeadbeef"),
      "eth-1,ethereum,ethereum,0xabc,KEY:<redacted>");

   // Bare provider spec (no CSV prefix) is masked too.
   BOOST_CHECK_EQUAL(redact_signature_provider_spec("KEY:PVT_WA_secretkey"), "KEY:<redacted>");

   // KIOD (and other non-KEY) providers reference external material -> returned unchanged.
   BOOST_CHECK_EQUAL(
      redact_signature_provider_spec("wire-1,wire,wire,PUB_WA_pub,KIOD:http://127.0.0.1:8888"),
      "wire-1,wire,wire,PUB_WA_pub,KIOD:http://127.0.0.1:8888");

   // Only the final field is inspected: a KEY:-prefixed *name* must not trigger false redaction.
   BOOST_CHECK_EQUAL(
      redact_signature_provider_spec("KEY:weird-name,wire,wire,PUB_WA_pub,KIOD:url"),
      "KEY:weird-name,wire,wire,PUB_WA_pub,KIOD:url");
}

// The auto-generated default signature-provider file holds private keys and must be written owner-only (0600),
// not left group/world readable under the process umask.
BOOST_AUTO_TEST_CASE(default_signature_provider_file_is_owner_only) {
   using namespace fc::crypto;
   auto clean_app = gsl_lite::finally([]() { appbase::application::reset_app_singleton(); });

   // Isolated, writable config-dir so the persisted key file is under our control.
   auto config_dir = std::filesystem::temp_directory_path() / "sigprov_perms_test";
   std::error_code ec;
   std::filesystem::remove_all(config_dir, ec);
   std::filesystem::create_directories(config_dir);
   auto cleanup = gsl_lite::finally([&]() { std::error_code e; std::filesystem::remove_all(config_dir, e); });

   std::vector<std::string> args = {"--config-dir", config_dir.string()};
   auto tester = create_app(args);

   // Generating + persisting a default provider must write the key file with owner-only permissions.
   tester->plugin().register_default_signature_providers({chain_key_type_wire});

   auto key_file = config_dir / "default_signature_providers.json";
   BOOST_REQUIRE(std::filesystem::exists(key_file));

   const auto perms = std::filesystem::status(key_file).permissions();
   BOOST_CHECK((perms & std::filesystem::perms::owner_read)  != std::filesystem::perms::none);
   BOOST_CHECK((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none);
   BOOST_CHECK((perms & std::filesystem::perms::group_all)   == std::filesystem::perms::none);
   BOOST_CHECK((perms & std::filesystem::perms::others_all)  == std::filesystem::perms::none);
}

// A default key file that predates the owner-only hardening was written under the process umask and may be
// group/world readable. Loading such a file must bring it down to owner-only even when no new default key is
// generated (`changed` stays false on that path, so nothing rewrites the file -- the load itself restricts it).
BOOST_AUTO_TEST_CASE(pre_existing_default_key_file_restricted_on_load) {
   using namespace fc::crypto;
   auto clean_app = gsl_lite::finally([]() { appbase::application::reset_app_singleton(); });

   // Isolated, writable config-dir so the persisted key file is under our control.
   auto config_dir = std::filesystem::temp_directory_path() / "sigprov_legacy_perms_test";
   std::error_code ec;
   std::filesystem::remove_all(config_dir, ec);
   std::filesystem::create_directories(config_dir);
   auto cleanup = gsl_lite::finally([&]() { std::error_code e; std::filesystem::remove_all(config_dir, e); });

   // Persist a valid default-provider key file the way the old, unhardened writer left it: world readable.
   auto priv = fc::crypto::private_key::generate();
   auto spec = fc::crypto::to_signature_provider_spec("wire-default", chain_kind_wire, chain_key_type_wire,
                                                      priv.get_public_key().to_string({}),
                                                      to_private_key_spec(priv.to_string({})));
   fc::mutable_variant_object vo;
   auto key_type_str = chain_key_type_reflector::to_string(chain_key_type_wire);
   vo(key_type_str, spec);
   const auto file_content = fc::json::to_string(vo, fc::time_point::maximum());
   auto key_file = config_dir / "default_signature_providers.json";
   {
      std::ofstream out(key_file);
      out << file_content;
   }
   std::filesystem::permissions(key_file,
                                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                   std::filesystem::perms::group_read | std::filesystem::perms::others_read,
                                std::filesystem::perm_options::replace);

   std::vector<std::string> args = {"--config-dir", config_dir.string()};
   auto tester = create_app(args);

   // The pre-seeded spec satisfies the requested key type, so nothing new is generated and nothing is saved;
   // the permission restriction must come from the load path.
   tester->plugin().register_default_signature_providers({chain_key_type_wire});

   const auto perms = std::filesystem::status(key_file).permissions();
   BOOST_CHECK((perms & std::filesystem::perms::owner_read)  != std::filesystem::perms::none);
   BOOST_CHECK((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none);
   BOOST_CHECK((perms & std::filesystem::perms::group_all)   == std::filesystem::perms::none);
   BOOST_CHECK((perms & std::filesystem::perms::others_all)  == std::filesystem::perms::none);

   // Restricting must not rewrite or corrupt the key material, and the loaded key must be registered.
   std::string after;
   fc::read_file_contents(key_file.string(), after);
   BOOST_CHECK_EQUAL(after, file_content);
   BOOST_CHECK(tester->plugin().has_provider(priv.get_public_key()));
}

// A malformed spec that still carries an inline KEY:<private-key> must not leak the key through the
// invalid-comma-count error message; the spec is redacted before it is formatted into the assertion.
BOOST_AUTO_TEST_CASE(invalid_spec_error_redacts_inline_private_key) {
   using namespace fc::crypto;

   auto priv = fc::crypto::private_key::generate();
   const auto priv_str = priv.to_string({});
   // Only 3 of the expected 4-5 comma-separated fields, so create_provider rejects it on comma count alone.
   const auto bad_spec = std::format("wire,{},{}", priv.get_public_key().to_string({}),
                                     to_private_key_spec(priv_str));

   auto tester = create_app();
   BOOST_CHECK_EXCEPTION(tester->plugin().create_provider(bad_spec), sysio::chain::plugin_config_exception,
                         [&](const sysio::chain::plugin_config_exception& e) {
                            const auto detail = e.to_detail_string();
                            return detail.find(priv_str) == std::string::npos &&
                                   detail.find("KEY:<redacted>") != std::string::npos;
                         });
}

BOOST_AUTO_TEST_SUITE_END()
