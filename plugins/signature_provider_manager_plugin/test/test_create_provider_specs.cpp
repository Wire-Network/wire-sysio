#include <boost/dll.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/spawn.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>
#include <format>
#include <memory>
#include <sodium.h>
#include <string>
#include <type_traits>
#include <vector>

#include <gsl-lite/gsl-lite.hpp>

#include <sysio/chain/types.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <fc-test/build_info.hpp>
#include <fc-test/crypto_utils.hpp>

using sysio::signature_provider_manager_plugin;
using sysio::chain::private_key_type;
using sysio::chain::public_key_type;
using namespace fc::test;

namespace {

namespace bp = boost::process;
namespace bfs = boost::filesystem;

/**
 * Sig provider tester app resources
 */
struct sig_provider_tester {

   appbase::scoped_app app{};

   signature_provider_manager_plugin& plugin() { return app->get_plugin<signature_provider_manager_plugin>(); }
};

/**
 * Creates a tester/app scoped instance
 *
 * @tparam args additional args to pass to `scoped_app`
 * @return `unique_ptr<sig_provider_tester>`
 */

// Overload that accepts a vector of strings for arguments
std::unique_ptr<sig_provider_tester> create_app(const std::vector<std::string>& args) {
   auto tester = std::make_unique<sig_provider_tester>();

   // Build argv as vector<char*> pointing to the underlying string buffers
   std::vector<const char*> argv;
   argv.reserve(args.size() + 1);
   argv.push_back("test_signature_provider_manager_plugin"); // program name
   for (auto& s : args) {
      argv.push_back(s.c_str());
   }

   BOOST_CHECK(tester->app->initialize<sysio::signature_provider_manager_plugin>(argv.size(), const_cast<char**>(argv.data())));

   return tester;
}

template <typename... Args>
   requires((std::same_as<std::decay_t<Args>, std::string>) && ...)
std::unique_ptr<sig_provider_tester> create_app(Args&&... extra_args) {
   std::vector<std::string> args_vec = {std::forward<Args>(extra_args)...};
   return create_app(args_vec);
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

   // Public key should match the one provided in spec
   BOOST_CHECK_EQUAL(provider->public_key.to_string({}), pub.to_string({}));
   BOOST_CHECK_EQUAL(provider->public_key, pub);
   BOOST_CHECK_EQUAL(provider->key_type, fc::crypto::chain_key_type_t::chain_key_type_wire);

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

   auto em_sig_data = em_priv_key.sign_compact_ex(fixture.payload, false);
   auto em_sig      = fc::to_hex(reinterpret_cast<const char*>(em_sig_data.data()), em_sig_data.size());

   // Compare generated signature against fixture
   auto fixture_sig = fc::crypto::ethereum::trim(fixture.signature);
   BOOST_CHECK_EQUAL(em_sig, fixture_sig);

   // Recover public key data (uncompressed)
   auto em_pub_key_rec_ser =
      fc::em::signature_shim(em_sig_data).recover_ex(fixture.payload, false).unwrapped().serialize_uncompressed();

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
   // Provider 2 should be retrievable
   BOOST_CHECK(!mgr.query_providers(fixture2.key_name).empty());
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

   // Parse the fixture signature from base58
   ed::signature_shim fixture_sig = ed::signature_shim::from_base58_string(fixture.signature);

   // The Python keygen signs the raw payload bytes (not a hash)
   // But our C++ implementation signs a SHA256 hash, so we verify
   // that the fixture signature verifies against the raw payload
   // by using libsodium directly
   int verify_result = crypto_sign_verify_detached(
      fixture_sig._data.data(),
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
   BOOST_CHECK_EQUAL(found->key_type, chain_key_type_solana);

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
   BOOST_CHECK_EQUAL(providers[0]->key_type, chain_key_type_solana);
}

BOOST_AUTO_TEST_SUITE_END()