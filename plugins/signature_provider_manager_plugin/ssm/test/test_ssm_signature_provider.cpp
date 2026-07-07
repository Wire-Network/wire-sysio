/**
 * Offline unit tests for the `ssm` sub-library: the `SSM:` spec parser and
 * the provider-construction pipeline behind the injectable-fetcher seam
 * (`create_ssm_provider_with_fetcher`). No AWS credentials, network, or SDK
 * calls are involved anywhere except the single env-gated live case at the
 * bottom.
 */

#include <boost/test/unit_test.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_manager_plugin/ssm/ssm_signature_provider.hpp>

#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/key_serdes.hpp>
#include <fc-test/crypto_utils.hpp>

#include <cstdlib>
#include <memory>
#include <string>

using namespace sysio::sigprov::ssm;
using namespace fc::crypto;
namespace chain = sysio::chain;

namespace {

/// Fetcher stub returning a fixed value/type, counting invocations so tests
/// can assert the network round-trip is (or is not) reached.
struct counting_fetcher {
   std::string    value;
   parameter_type type  = parameter_type::SecureString;
   std::shared_ptr<int> calls = std::make_shared<int>(0);

   fetched_parameter operator()(const ssm_param_ref&) const {
      ++*calls;
      return fetched_parameter{value, type};
   }
};

/// True when the exception's rendered detail mentions `needle` -- used to pin
/// operator-facing message content (parameter name, remediation) without
/// asserting the entire string.
auto detail_contains(std::string needle) {
   return [needle = std::move(needle)](const fc::exception& e) {
      return e.to_detail_string().find(needle) != std::string::npos;
   };
}

/// True when the exception's rendered detail does NOT mention `needle` --
/// used to prove secret material is kept out of error messages.
auto detail_omits(std::string needle) {
   return [needle = std::move(needle)](const fc::exception& e) {
      return e.to_detail_string().find(needle) == std::string::npos;
   };
}

} // namespace

BOOST_AUTO_TEST_SUITE(ssm_spec_parser)

BOOST_AUTO_TEST_CASE(parse_full_arn) {
   const auto ref = parse_ssm_spec("arn:aws:ssm:us-east-1:111122223333:parameter/wire/prod/bp1");
   BOOST_CHECK_EQUAL(ref.region, "us-east-1");
   // The full unmodified ARN is handed to GetParameter so the account id is
   // preserved.
   BOOST_CHECK_EQUAL(ref.name, "arn:aws:ssm:us-east-1:111122223333:parameter/wire/prod/bp1");
}

BOOST_AUTO_TEST_CASE(parse_arn_version_selector_stays_glued) {
   // A trailing `:3` (SSM's version-selector syntax) is a seventh colon field;
   // the capped split glues it to the tail and the ARN passes through intact.
   const auto ref = parse_ssm_spec("arn:aws:ssm:us-east-1:111122223333:parameter/wire/bp1:3");
   BOOST_CHECK_EQUAL(ref.region, "us-east-1");
   BOOST_CHECK_EQUAL(ref.name, "arn:aws:ssm:us-east-1:111122223333:parameter/wire/bp1:3");
}

BOOST_AUTO_TEST_CASE(parse_arn_rejects_empty_region) {
   BOOST_CHECK_EXCEPTION(parse_ssm_spec("arn:aws:ssm::111122223333:parameter/wire/bp1"),
                         chain::plugin_config_exception, detail_contains("empty region"));
}

BOOST_AUTO_TEST_CASE(parse_arn_rejects_empty_account) {
   BOOST_CHECK_EXCEPTION(parse_ssm_spec("arn:aws:ssm:us-east-1::parameter/wire/bp1"),
                         chain::plugin_config_exception, detail_contains("empty account-id"));
}

BOOST_AUTO_TEST_CASE(parse_arn_rejects_non_parameter_tail) {
   BOOST_CHECK_EXCEPTION(parse_ssm_spec("arn:aws:ssm:us-east-1:111122223333:document/wire-doc"),
                         chain::plugin_config_exception, detail_contains("parameter/"));
}

BOOST_AUTO_TEST_CASE(parse_arn_rejects_empty_parameter_path) {
   BOOST_CHECK_EXCEPTION(parse_ssm_spec("arn:aws:ssm:us-east-1:111122223333:parameter/"),
                         chain::plugin_config_exception, detail_contains("empty parameter path"));
}

BOOST_AUTO_TEST_CASE(parse_rejects_non_aws_partition) {
   BOOST_CHECK_EXCEPTION(parse_ssm_spec("arn:aws-cn:ssm:cn-north-1:111122223333:parameter/wire/bp1"),
                         chain::plugin_config_exception, detail_contains("aws-cn"));
}

BOOST_AUTO_TEST_CASE(parse_rejects_wrong_service_arn) {
   // A KMS ARN handed to the SSM scheme is an out-of-scope ARN, not shorthand.
   BOOST_CHECK_EXCEPTION(parse_ssm_spec("arn:aws:kms:us-east-1:111122223333:key/abc"),
                         chain::plugin_config_exception, detail_contains("arn:aws:ssm:"));
}

BOOST_AUTO_TEST_CASE(parse_rejects_miscased_arn_instead_of_shorthand) {
   // Mis-cased ARNs must be recognised as ARNs (and rejected loudly), never
   // parsed as shorthand with region="ARN".
   BOOST_CHECK_EXCEPTION(parse_ssm_spec("ARN:AWS:SSM:us-east-1:111122223333:parameter/wire/bp1"),
                         chain::plugin_config_exception, detail_contains("arn:aws:ssm:"));
}

BOOST_AUTO_TEST_CASE(parse_shorthand) {
   const auto ref = parse_ssm_spec("us-east-1:/wire/prod/bp1");
   BOOST_CHECK_EQUAL(ref.region, "us-east-1");
   BOOST_CHECK_EQUAL(ref.name, "/wire/prod/bp1");
}

BOOST_AUTO_TEST_CASE(parse_shorthand_bare_name) {
   // Non-hierarchical parameter names (no leading '/') are valid in SSM.
   const auto ref = parse_ssm_spec("eu-west-2:wire-bp1-key");
   BOOST_CHECK_EQUAL(ref.region, "eu-west-2");
   BOOST_CHECK_EQUAL(ref.name, "wire-bp1-key");
}

BOOST_AUTO_TEST_CASE(parse_shorthand_version_selector_passthrough) {
   // Everything after the first colon is the Name, so SSM's native
   // `name:version` / `name:label` selectors need no extra grammar here.
   const auto ref = parse_ssm_spec("us-east-1:/wire/prod/bp1:3");
   BOOST_CHECK_EQUAL(ref.region, "us-east-1");
   BOOST_CHECK_EQUAL(ref.name, "/wire/prod/bp1:3");
}

BOOST_AUTO_TEST_CASE(parse_rejects_missing_region) {
   BOOST_CHECK_EXCEPTION(parse_ssm_spec("/wire/prod/bp1"),
                         chain::plugin_config_exception, detail_contains("must include a region"));
}

BOOST_AUTO_TEST_CASE(parse_rejects_empty_region_shorthand) {
   BOOST_CHECK_EXCEPTION(parse_ssm_spec(":/wire/prod/bp1"),
                         chain::plugin_config_exception, detail_contains("empty region"));
}

BOOST_AUTO_TEST_CASE(parse_rejects_empty_name_shorthand) {
   BOOST_CHECK_EXCEPTION(parse_ssm_spec("us-east-1:"),
                         chain::plugin_config_exception, detail_contains("empty parameter name"));
}

BOOST_AUTO_TEST_CASE(parse_rejects_empty_spec) {
   BOOST_CHECK_EXCEPTION(parse_ssm_spec(""),
                         chain::plugin_config_exception, detail_contains("empty"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ssm_provider_construction)

namespace {
constexpr std::string_view test_spec_body = "us-east-1:/wire/test/key";
}

BOOST_AUTO_TEST_CASE(wire_key_round_trip) {
   const auto priv     = private_key::generate();
   const auto priv_str = priv.to_string({});
   // Round-trip the public key through its native string form, exactly as the
   // plugin does when it parses the spec's <public-key> field.
   const auto pub = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{priv_str};
   const auto result = create_ssm_provider_with_fetcher(chain_key_type_wire, pub, test_spec_body, fetcher);

   BOOST_CHECK_EQUAL(*fetcher.calls, 1);
   BOOST_REQUIRE(result.private_key.has_value());
   BOOST_CHECK(result.private_key->get_public_key() == pub);
   BOOST_CHECK(!result.startup_probe); // nothing left to probe after the eager fetch

   // The signer closure captured the fetched key: its output matches signing
   // with the key directly (fc signing is deterministic for every key type).
   const auto digest = fc::sha256::hash(std::string{"ssm sigprov wire round trip"});
   BOOST_CHECK(result.signer(digest) == priv.sign(digest));
}

BOOST_AUTO_TEST_CASE(wire_bls_key_round_trip) {
   const auto priv     = private_key::generate(private_key::key_type::bls);
   const auto priv_str = priv.to_string({});
   const auto pub = from_native_string_to_public_key<chain_key_type_wire_bls>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{priv_str};
   const auto result = create_ssm_provider_with_fetcher(chain_key_type_wire_bls, pub, test_spec_body, fetcher);

   BOOST_REQUIRE(result.private_key.has_value());
   BOOST_CHECK(result.private_key->get_public_key() == pub);

   const auto digest = fc::sha256::hash(std::string{"ssm sigprov bls round trip"});
   BOOST_CHECK(result.signer(digest) == priv.sign(digest));
}

BOOST_AUTO_TEST_CASE(ethereum_key_round_trip) {
   const auto fixture = fc::test::load_keygen_fixture(fc::test::keygen_ethereum_name, 1);
   const auto pub     = from_native_string_to_public_key<chain_key_type_ethereum>(fixture.public_key);

   counting_fetcher fetcher{fixture.private_key};
   const auto result = create_ssm_provider_with_fetcher(chain_key_type_ethereum, pub, test_spec_body, fetcher);

   BOOST_REQUIRE(result.private_key.has_value());
   BOOST_CHECK(result.private_key->get_public_key() == pub);

   const auto digest = fc::sha256::hash(std::string{"ssm sigprov ethereum round trip"});
   BOOST_CHECK(result.signer(digest) == result.private_key->sign(digest));
}

BOOST_AUTO_TEST_CASE(solana_key_round_trip) {
   const auto fixture = fc::test::load_keygen_fixture(fc::test::keygen_solana_name, 1);
   const auto pub     = from_native_string_to_public_key<chain_key_type_solana>(fixture.public_key);

   counting_fetcher fetcher{fixture.private_key};
   const auto result = create_ssm_provider_with_fetcher(chain_key_type_solana, pub, test_spec_body, fetcher);

   // The Solana signing path in signer.hpp requires a raw local key -- the
   // populated private_key is what makes an SSM: provider usable there.
   BOOST_REQUIRE(result.private_key.has_value());
   BOOST_CHECK(result.private_key->get_public_key() == pub);

   const auto digest = fc::sha256::hash(std::string{"ssm sigprov solana round trip"});
   BOOST_CHECK(result.signer(digest) == result.private_key->sign(digest));
}

BOOST_AUTO_TEST_CASE(value_whitespace_is_trimmed) {
   // `aws ssm put-parameter --value "$(cat key.txt)"` style workflows leave
   // trailing newlines; they must not brick the key.
   const auto priv = private_key::generate();
   const auto pub  = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{"\n  " + priv.to_string({}) + " \t\r\n"};
   const auto result = create_ssm_provider_with_fetcher(chain_key_type_wire, pub, test_spec_body, fetcher);
   BOOST_REQUIRE(result.private_key.has_value());
   BOOST_CHECK(result.private_key->get_public_key() == pub);
}

BOOST_AUTO_TEST_CASE(rejects_plain_string_parameter) {
   const auto priv = private_key::generate();
   const auto pub  = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{priv.to_string({}), parameter_type::String};
   BOOST_CHECK_EXCEPTION(
      create_ssm_provider_with_fetcher(chain_key_type_wire, pub, test_spec_body, fetcher),
      chain::plugin_config_exception, detail_contains("--type SecureString"));
}

BOOST_AUTO_TEST_CASE(rejects_string_list_parameter) {
   const auto priv = private_key::generate();
   const auto pub  = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{priv.to_string({}), parameter_type::StringList};
   BOOST_CHECK_EXCEPTION(
      create_ssm_provider_with_fetcher(chain_key_type_wire, pub, test_spec_body, fetcher),
      chain::plugin_config_exception, detail_contains("StringList"));
}

BOOST_AUTO_TEST_CASE(rejects_untyped_parameter) {
   const auto priv = private_key::generate();
   const auto pub  = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{priv.to_string({}), parameter_type::NOT_SET};
   BOOST_CHECK_THROW(create_ssm_provider_with_fetcher(chain_key_type_wire, pub, test_spec_body, fetcher),
                     chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(rejects_empty_value) {
   const auto priv = private_key::generate();
   const auto pub  = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{"  \n\t  "};
   BOOST_CHECK_EXCEPTION(
      create_ssm_provider_with_fetcher(chain_key_type_wire, pub, test_spec_body, fetcher),
      chain::plugin_config_exception, detail_contains("is empty"));
}

BOOST_AUTO_TEST_CASE(parse_failure_does_not_echo_the_value) {
   // The fetched value is a secret. When it fails to parse as a key, the
   // error must name the parameter and the reason class -- and must NOT
   // contain the value itself.
   constexpr auto sentinel = "not-a-key-SENTINEL-0xDEADBEEF";
   const auto priv = private_key::generate();
   const auto pub  = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{sentinel};
   BOOST_CHECK_EXCEPTION(
      create_ssm_provider_with_fetcher(chain_key_type_wire, pub, test_spec_body, fetcher),
      chain::plugin_config_exception, detail_omits("SENTINEL"));
   BOOST_CHECK_EXCEPTION(
      create_ssm_provider_with_fetcher(chain_key_type_wire, pub, test_spec_body, fetcher),
      chain::plugin_config_exception, detail_contains("the value is not shown"));
}

BOOST_AUTO_TEST_CASE(rejects_pinned_pubkey_mismatch) {
   const auto priv       = private_key::generate();
   const auto other_priv = private_key::generate();
   const auto other_pub =
      from_native_string_to_public_key<chain_key_type_wire>(other_priv.get_public_key().to_string({}));

   counting_fetcher fetcher{priv.to_string({})};
   BOOST_CHECK_EXCEPTION(
      create_ssm_provider_with_fetcher(chain_key_type_wire, other_pub, test_spec_body, fetcher),
      chain::plugin_config_exception, detail_contains("does not match"));
}

BOOST_AUTO_TEST_CASE(rejects_sui_before_fetching) {
   const auto priv = private_key::generate();
   const auto pub  = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{priv.to_string({})};
   BOOST_CHECK_THROW(create_ssm_provider_with_fetcher(chain_key_type_sui, pub, test_spec_body, fetcher),
                     chain::pending_impl_exception);
   // The key-type pre-check runs before the network round-trip.
   BOOST_CHECK_EQUAL(*fetcher.calls, 0);
}

BOOST_AUTO_TEST_CASE(malformed_spec_fails_before_fetching) {
   const auto priv = private_key::generate();
   const auto pub  = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   counting_fetcher fetcher{priv.to_string({})};
   BOOST_CHECK_THROW(create_ssm_provider_with_fetcher(chain_key_type_wire, pub, "no-region-here", fetcher),
                     chain::plugin_config_exception);
   BOOST_CHECK_EQUAL(*fetcher.calls, 0);
}

BOOST_AUTO_TEST_CASE(transient_fetch_failure_propagates_as_transient) {
   // The fetcher classifies its own failures (the real one via
   // throw_ssm_error). A transient classification must reach the caller as
   // signing_transient_exception, not be laundered into a config error.
   const auto priv = private_key::generate();
   const auto pub  = from_native_string_to_public_key<chain_key_type_wire>(priv.get_public_key().to_string({}));

   const parameter_fetcher throttled = [](const ssm_param_ref&) -> fetched_parameter {
      FC_THROW_EXCEPTION(chain::signing_transient_exception, "simulated SSM throttle");
   };
   BOOST_CHECK_THROW(create_ssm_provider_with_fetcher(chain_key_type_wire, pub, test_spec_body, throttled),
                     chain::signing_transient_exception);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ssm_live)

// ---------------------------------------------------------------------------
// Env-gated live round-trip against real AWS SSM Parameter Store. Skipped
// (cleanly, with a message) unless BOTH env vars are set:
//
//   SSM_LIVE_SPEC      body of an `SSM:` spec, e.g. `us-east-1:/wire/ci/test-key`
//                      or `arn:aws:ssm:us-east-1:111122223333:parameter/wire/ci/test-key`
//   SSM_LIVE_PUBKEY    native-form public key matching the private key stored
//                      in the parameter (e.g. `PUB_K1_...` for a wire key)
//   SSM_LIVE_KEY_TYPE  optional chain key type of the stored key
//                      ("wire" | "wire_bls" | "ethereum" | "solana"),
//                      default "wire"
//
// Requires AWS credentials in the runner's environment (env, ~/.aws/, IRSA,
// or IMDS) with ssm:GetParameter on the parameter and kms:Decrypt on its key.
// See README.md in this directory for the one-time parameter setup.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ssm_live_fetch_round_trip) {
   const auto* spec_env = std::getenv("SSM_LIVE_SPEC");
   const auto* pub_env  = std::getenv("SSM_LIVE_PUBKEY");
   if (!spec_env || !pub_env || *spec_env == '\0' || *pub_env == '\0') {
      BOOST_TEST_MESSAGE("SSM_LIVE_SPEC / SSM_LIVE_PUBKEY not set -- skipping live SSM test");
      return;
   }
   const auto* type_env = std::getenv("SSM_LIVE_KEY_TYPE");
   const auto  key_type = (type_env && *type_env != '\0')
                             ? chain_key_type_reflector::from_string(type_env)
                             : chain_key_type_wire;

   const auto pub    = fc::crypto::from_native_string_to_public_key(key_type, pub_env);
   const auto result = create_ssm_provider(key_type, pub, spec_env);

   BOOST_REQUIRE(result.private_key.has_value());
   BOOST_CHECK(result.private_key->get_public_key() == pub);

   const auto digest = fc::sha256::hash(std::string{"wire-sysio ssm live test 2026"});
   BOOST_CHECK(result.signer(digest) == result.private_key->sign(digest));
}

BOOST_AUTO_TEST_SUITE_END()
