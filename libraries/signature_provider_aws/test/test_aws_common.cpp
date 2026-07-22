/**
 * Offline unit tests for the shared AWS glue (`sysio::sigprov::aws`): the region-shape classifier, the
 * environment-driven default-region resolution chain, and the per-region client cache's region-less
 * normalization. No AWS credentials or network anywhere: every case pins the environment it needs (see
 * `env_fixtures.hpp`), and IMDS is explicitly disabled wherever the chain -- or the SDK's own
 * `ClientConfiguration` default constructor -- could otherwise stall probing the metadata endpoint on a
 * non-AWS test host.
 */

#include <boost/test/unit_test.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_aws/aws_common.hpp>
#include <sysio/signature_provider_aws/test/env_fixtures.hpp>

#include <fc/filesystem.hpp>

#include <fstream>
#include <string>

using namespace sysio::sigprov::aws;
using namespace sysio::sigprov::aws::test;
namespace chain = sysio::chain;

BOOST_AUTO_TEST_SUITE(aws_region_shape)

BOOST_AUTO_TEST_CASE(accepts_regions_of_every_partition) {
   // Standard, extended-geography, GovCloud, ISO, and EU-sovereign spellings all share the shape.
   BOOST_CHECK(looks_like_aws_region("us-east-1"));
   BOOST_CHECK(looks_like_aws_region("eu-west-2"));
   BOOST_CHECK(looks_like_aws_region("ap-southeast-3"));
   BOOST_CHECK(looks_like_aws_region("me-central-1"));
   BOOST_CHECK(looks_like_aws_region("us-gov-west-1"));
   BOOST_CHECK(looks_like_aws_region("us-isob-east-1"));
   BOOST_CHECK(looks_like_aws_region("eusc-de-east-1"));
}

BOOST_AUTO_TEST_CASE(rejects_key_and_parameter_shapes) {
   // The shapes the spec parsers must NOT mistake for a region: KMS key ids (uuid and multi-Region), alias
   // names, and both path-style and bare SSM parameter names.
   BOOST_CHECK(!looks_like_aws_region("1234abcd-12ab-34cd-56ef-1234567890ab"));
   BOOST_CHECK(!looks_like_aws_region("mrk-1234abcd12ab34cd56ef1234567890ab"));
   BOOST_CHECK(!looks_like_aws_region("alias/wire-cranker-eth-01"));
   BOOST_CHECK(!looks_like_aws_region("/wire/prod/bp1"));
   BOOST_CHECK(!looks_like_aws_region("wire-bp1-key"));
   BOOST_CHECK(!looks_like_aws_region("bp1key"));
}

BOOST_AUTO_TEST_CASE(rejects_malformed_region_spellings) {
   BOOST_CHECK(!looks_like_aws_region(""));
   BOOST_CHECK(!looks_like_aws_region("us-east"));       // no trailing digit segment
   BOOST_CHECK(!looks_like_aws_region("us-east-"));      // empty trailing segment
   BOOST_CHECK(!looks_like_aws_region("-us-east-1"));    // empty leading segment
   BOOST_CHECK(!looks_like_aws_region("us--east-1"));    // empty middle segment
   BOOST_CHECK(!looks_like_aws_region("u-east-1"));      // one-char lead
   BOOST_CHECK(!looks_like_aws_region("US-EAST-1"));     // regions are lowercase
   BOOST_CHECK(!looks_like_aws_region("us-east-1x"));    // non-digit in trailing segment
}

BOOST_AUTO_TEST_CASE(names_ending_in_a_digit_segment_are_region_shaped) {
   // The deliberate corner: a name whose trailing hyphen-separated segment is all digits is
   // indistinguishable from a region by shape alone. The SSM spec grammar documents that such a parameter,
   // when combined with a region-less selector reference, must be addressed with an explicit region or an
   // ARN (region-shaped-wins precedence).
   BOOST_CHECK(looks_like_aws_region("my-param-2"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(aws_default_region_resolution)

BOOST_AUTO_TEST_CASE(resolves_from_aws_region_env) {
   const scoped_env_var default_region{env_default_region, nullptr};
   const scoped_env_var region{env_region, "eu-central-1"};
   BOOST_CHECK_EQUAL(resolve_default_region(), "eu-central-1");
}

BOOST_AUTO_TEST_CASE(aws_default_region_outranks_aws_region) {
   // Same precedence as the SDK's own ClientConfiguration chain.
   const scoped_env_var default_region{env_default_region, "us-west-2"};
   const scoped_env_var region{env_region, "eu-central-1"};
   BOOST_CHECK_EQUAL(resolve_default_region(), "us-west-2");
}

BOOST_AUTO_TEST_CASE(resolves_from_shared_config_when_env_is_empty) {
   const scoped_env_var default_region{env_default_region, nullptr};
   const scoped_env_var region{env_region, nullptr};
   // Pin the profile selection too, so the [default] section below is the one consulted.
   const scoped_env_var profile{"AWS_PROFILE", nullptr};
   const scoped_env_var default_profile{"AWS_DEFAULT_PROFILE", nullptr};
   // The config step must win before IMDS is even consulted.
   const scoped_env_var imds{env_ec2_metadata_disabled, metadata_disabled_true};

   const fc::temp_directory dir;
   const auto config_path = (dir.path() / "aws-config").string();
   {
      std::ofstream out{config_path};
      out << "[default]\nregion = mx-central-1\n";
   }
   const scoped_config_file_redirect config{config_path.c_str()};
   BOOST_CHECK_EQUAL(resolve_default_region(), "mx-central-1");
}

BOOST_AUTO_TEST_CASE(unresolvable_region_throws_instead_of_defaulting) {
   // The SDK's own chain silently falls back to us-east-1 here; the whole point of resolve_default_region
   // is that it must NOT -- a signing key never gets looked up in a region the operator didn't choose. The
   // message must carry the remediation env var.
   const scoped_unresolvable_region_env pinned;
   BOOST_CHECK_EXCEPTION(resolve_default_region(), chain::plugin_config_exception,
                         [](const fc::exception& e) {
                            return e.to_detail_string().find(env_region) != std::string::npos;
                         });
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(aws_region_client_cache)

namespace {

/// Minimal stand-in for an AWS service client: constructible from a ClientConfiguration, recording the
/// region it was configured with -- lets the cache's normalization be asserted offline, with no service SDK
/// and no pointer-identity guesswork.
struct fake_client {
   explicit fake_client(const Aws::Client::ClientConfiguration& cfg)
      : region(cfg.region.c_str(), cfg.region.size()) {}
   std::string region;
};

} // namespace

BOOST_AUTO_TEST_CASE(explicit_region_reaches_the_client_config) {
   // IMDS disabled so the ClientConfiguration default constructor cannot stall probing the metadata
   // endpoint on non-AWS test hosts (the spec region overwrites whatever it resolves anyway).
   const scoped_env_var imds{env_ec2_metadata_disabled, metadata_disabled_true};
   region_client_cache<fake_client> cache;
   const auto client = cache.get("eu-west-2");
   BOOST_REQUIRE(client);
   BOOST_CHECK_EQUAL(client->region, "eu-west-2");
   BOOST_CHECK_EQUAL(client.get(), cache.get("eu-west-2").get());
}

BOOST_AUTO_TEST_CASE(empty_region_resolves_and_shares_the_resolved_slot) {
   const scoped_env_var default_region{env_default_region, nullptr};
   const scoped_env_var region{env_region, "eu-north-1"};
   region_client_cache<fake_client> cache;
   const auto resolved = cache.get("");
   BOOST_REQUIRE(resolved);
   BOOST_CHECK_EQUAL(resolved->region, "eu-north-1");
   // Cached under the RESOLVED region: the equivalent explicit lookup shares the client.
   BOOST_CHECK_EQUAL(resolved.get(), cache.get("eu-north-1").get());
}

BOOST_AUTO_TEST_CASE(empty_region_unresolvable_throws) {
   const scoped_unresolvable_region_env pinned;
   region_client_cache<fake_client> cache;
   BOOST_CHECK_THROW(cache.get(""), chain::plugin_config_exception);
}

BOOST_AUTO_TEST_SUITE_END()
