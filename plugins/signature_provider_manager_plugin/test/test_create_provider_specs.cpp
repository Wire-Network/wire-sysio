#include <secp256k1.h>
#include <boost/dll.hpp>
#include <boost/process.hpp>
#include <boost/process/io.hpp>
#include <boost/process/spawn.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/ethereum_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/rand.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

using sysio::signature_provider_manager_plugin;
using sysio::chain::private_key_type;
using sysio::chain::public_key_type;

struct keygen_result {
   std::string public_key;
   std::string private_key;
   std::string address;
   std::string signature;
   std::string payload;
};

FC_REFLECT(keygen_result, (public_key)(private_key)(address)(signature)(payload))

namespace {

constexpr auto keygen_ethereum_name = "keygen-ethereum.py";
constexpr auto keygen_solana_name   = "keygen-solana.py";

namespace bp = boost::process;
auto exe_path      = boost::dll::program_location().parent_path();
auto fixtures_path = exe_path / "fixtures";
auto bin_root      = exe_path.parent_path().parent_path().parent_path();

[[maybe_unused]] keygen_result load_keygen_fixture(const std::string& chain_name, std::uint32_t id) {
   auto json_filename = std::format("{}-keygen-{:02}.json", chain_name, id);
   auto json_path     = fixtures_path / json_filename;

   BOOST_CHECK(boost::filesystem::exists(json_path));

   std::string json_data;
   fc::read_file_contents(json_path.string(), json_data);

   keygen_result res;
   auto          res_obj = fc::json::from_string(
      json_data,
      fc::json::parse_type::relaxed_parser
      ).as<fc::variant_object>();

   from_variant(res_obj, res);

   return res;
}


[[maybe_unused]] keygen_result generate_external_test_key_and_sig(const std::string& keygen_name) {
   BOOST_CHECK(keygen_ethereum_name == keygen_name || keygen_solana_name == keygen_name);

   auto script_path = bin_root / "tools" / keygen_name;

   std::vector<std::string> proc_args;

   std::string output;
   // std::string output_err;
   auto python_exe = boost::process::search_path("python");
   std::println(std::cerr, "script_path={}", script_path.string());
   std::println(std::cerr, "python_exe={}", python_exe.string());
   BOOST_CHECK(!python_exe.empty());
   BOOST_CHECK(boost::filesystem::exists(script_path));

   bp::ipstream pipe_stream; // pipe for child's stdout

   bp::child keygen_proc(python_exe,
                         script_path.string(),
                         // proc_args,
                         bp::std_in.close(),
                         bp::std_out > pipe_stream,
                         bp::std_err > bp::null
      );
   std::string line;
   while (std::getline(pipe_stream, line))
      output += line + "\n";

   keygen_proc.wait();
   // std::println(std::cerr, "output={}\n" "output_err={}", output,output_err);
   BOOST_CHECK_EQUAL(keygen_proc.exit_code(), 0);

   keygen_result res;
   auto          res_obj = fc::json::from_string(
      //   keygen_ethereum_output_json,
      output,
      fc::json::parse_type::relaxed_parser
      ).as<fc::variant_object>();
   from_variant(res_obj, res);

   return res;
}

/**
 * Sig provider tester app resources
 */
struct sig_provider_tester {

   appbase::scoped_app app{};

   signature_provider_manager_plugin& plugin() {
      return app->get_plugin<signature_provider_manager_plugin>();
   }
};

/**
 * Creates a tester/app scoped instance
 *
 * @tparam Args additional args to pass to `scoped_app`
 * @param extra_args additional args to pass to `scoped_app`
 * @return `unique_ptr<sig_provider_tester>`
 */
template <typename... Args>
std::unique_ptr<sig_provider_tester> create_app(Args&&... extra_args) {
   auto tester = std::make_unique<sig_provider_tester>();

   std::array args = {
      "test_signature_provider_manager_plugin",
      std::forward<Args>(extra_args)...
   };

   BOOST_CHECK(
      tester->app->initialize<sysio::signature_provider_manager_plugin>(args.size(), const_cast<char**>(args.data())));

   return tester;
}

std::string to_private_key_spec(const std::string& priv) {
   return std::format("KEY:{}", priv);
}

std::string to_provider_spec(
const std::string&         key_name,
fc::crypto::chain_kind     target_chain,
fc::crypto::chain_key_type key_type,
std::string                public_key_text,
std::string                private_key_provider_spec) {
   using namespace fc::crypto;
   return std::format("{},{},{},{},{}",
      key_name,
      chain_kind_reflector::to_fc_string(target_chain),
      chain_key_type_reflector::to_fc_string(key_type),
      public_key_text,
      private_key_provider_spec);
}

struct context_creator {
   context_creator() {
      ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
      char seed[32];
      fc::rand_bytes(seed, sizeof(seed));
      FC_ASSERT(secp256k1_context_randomize(ctx, (const unsigned char*)seed));
   }

   secp256k1_context* ctx = nullptr;
};

const secp256k1_context* get_ec_context() {
   static context_creator cc;
   return cc.ctx;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(signature_provider_manager_create_provider_specs)

BOOST_AUTO_TEST_CASE(create_provider_wire_key_from_example_spec) {
   using namespace fc::crypto;
   auto priv = fc::crypto::private_key::generate();
   auto pub  = priv.get_public_key();

   auto private_key_spec = to_private_key_spec(priv.to_string({}));
   auto provider_spec = to_provider_spec(
      "wire_key-1",
      chain_kind_wire,
      chain_key_type_wire,
      pub.to_string({}),
      private_key_spec
      );
   auto  tester = create_app();
   auto& mgr    = tester->plugin();

   auto& provider = mgr.create_provider(provider_spec);

   // Public key should match the one provided in spec
   BOOST_CHECK_EQUAL(provider.public_key.to_string({}), pub.to_string({}));
   BOOST_CHECK_EQUAL(provider.public_key, pub);
   BOOST_CHECK_EQUAL(provider.key_type, fc::crypto::chain_key_type::chain_key_type_wire);

   // Provider should be retrievable via its public key
   BOOST_CHECK(mgr.has_provider(provider.public_key));
   auto& found = mgr.get_provider(provider.public_key);
   BOOST_CHECK_EQUAL(found.public_key, pub);
   BOOST_CHECK_EQUAL(found.public_key.to_string({}), pub.to_string({}));

   // Sign function should be set
   BOOST_CHECK(static_cast<bool>(provider.sign));
}

BOOST_AUTO_TEST_CASE(create_provider_ethereum_fixture_pub_priv_sig_interoperable) {

   // Load fixture
   keygen_result fixture = load_keygen_fixture("ethereum", 1);

   // Create private key from fixture
   auto fixture_priv_key_bytes = fc::from_hex(fc::crypto::ethereum::trim(fixture.private_key));

   auto em_priv_key = fc::em::private_key::regenerate(
      fc::sha256(reinterpret_cast<const char*>(fixture_priv_key_bytes.data()), fixture_priv_key_bytes.size()));

   auto em_sig_data = em_priv_key.sign_compact_ex(fixture.payload, false);
   auto em_sig      = fc::to_hex(reinterpret_cast<const char*>(em_sig_data.data), em_sig_data.size());

   // Compare generated signature against fixture
   auto fixture_sig = fc::crypto::ethereum::trim(fixture.signature);
   BOOST_CHECK_EQUAL(em_sig, fixture_sig);

   // Recover public key data (uncompressed)
   auto em_pub_key_rec_ser = fc::em::signature_shim(em_sig_data).recover_ex(fixture.payload, false)
                                                                .unwrapped().serialize_uncompressed();

   auto em_pub_key_rec_hex = fc::crypto::ethereum::trim_public_key(fc::to_hex(
      em_pub_key_rec_ser.data,
      em_pub_key_rec_ser.size()));

   auto fixture_pub_key_stripped = fc::crypto::ethereum::trim_public_key(fixture.public_key);
   BOOST_CHECK_EQUAL(em_pub_key_rec_hex, fixture_pub_key_stripped);

   // Create pub key from fixture
   auto fixture_pub_key_bytes = fc::from_hex(fixture_pub_key_stripped);

   fc::em::public_key em_pub_key_parsed      = fc::crypto::ethereum::parse_public_key(fixture_pub_key_stripped);
   auto               em_pub_key_parsed_data = em_pub_key_parsed.serialize();

   auto em_pub_key      = em_priv_key.get_public_key();
   auto em_pub_key_data = em_pub_key.serialize();

   // Compare pub key from parsing fixture to the
   // private key provided public key
   BOOST_CHECK(em_pub_key == em_pub_key_parsed);

   // Redundant, but checks the encoding of pub keys too
   BOOST_CHECK(fc::to_hex(em_pub_key_data.data, em_pub_key_data.size()) ==
      fc::to_hex(em_pub_key_parsed_data.data, em_pub_key_parsed_data.size()));


}

BOOST_AUTO_TEST_CASE(create_provider_ethereum_key_spec) {
   using namespace fc::crypto;
   // Load fixture
   keygen_result fixture = load_keygen_fixture("ethereum", 1);

   // TODO: Now parse and create signature provider
   auto key_type_eth_str = chain_key_type_reflector::to_fc_string(chain_key_type_ethereum);
   BOOST_CHECK_EQUAL(key_type_eth_str, "ethereum");

   const std::string private_key_spec = to_private_key_spec(fixture.private_key);
   // auto provider_spec = to_provider_spec(
   //    "eth_key-1",
   //    chain_kind_ethereum,
   //    chain_key_type_ethereum,
   //    fixture.public_key,
   //    private_key_spec
   //    );
   auto  tester = create_app();
   auto& mgr    = tester->plugin();

   auto& provider = mgr.create_provider(
      "",
      chain_kind_ethereum,
      chain_key_type_ethereum,
      fixture.public_key,
      private_key_spec);


   // Provider should be retrievable
   BOOST_CHECK(mgr.has_provider(provider.public_key));
   auto& found = mgr.get_provider(provider.public_key);
   BOOST_CHECK_EQUAL(found.public_key.to_string({}), provider.public_key.to_string({}));

   // Sign function should be set
   BOOST_CHECK(static_cast<bool>(provider.sign));
}

// BOOST_AUTO_TEST_CASE(create_provider_solana_key_spec) {
//    // Generate a Solana keypair (ED25519)
//    private_key_type priv = private_key_type::generate<fc::crypto::ed::private_key_shim>();
//    const std::string spec = make_spec_from_keypair(priv);
//
//    auto tester = create_app();
//    auto& mgr = tester.plugin.get();
//
//    auto& provider = mgr.create_provider(spec);
//
//    // Public key should match
//    BOOST_CHECK_EQUAL(provider.public_key.to_string({}), priv.get_public_key().to_string({}));
//
//    // Provider should be retrievable
//    BOOST_CHECK(mgr.has_provider(provider.public_key));
//    auto& found = mgr.get_provider(provider.public_key);
//    BOOST_CHECK_EQUAL(found.public_key.to_string({}), provider.public_key.to_string({}));
//
//    // Sign function should be set
//    BOOST_CHECK(static_cast<bool>(provider.sign));
// }

BOOST_AUTO_TEST_SUITE_END()