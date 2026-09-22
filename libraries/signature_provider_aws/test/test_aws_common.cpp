/**
 * Offline unit tests for the shared AWS glue (`sysio::sigprov::aws`): the region-shape classifier, the
 * environment-driven default-region resolution chain, and the per-region client cache's region-less
 * normalization. No AWS credentials or network anywhere: every case pins the environment it needs (see
 * `env_fixtures.hpp`), and IMDS is explicitly disabled wherever the region resolution chain could otherwise
 * stall probing the metadata endpoint on a non-AWS test host.
 */

#include <boost/test/unit_test.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_aws/aws_common.hpp>
#include <sysio/signature_provider_aws/test/env_fixtures.hpp>

#include <fc/filesystem.hpp>

#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/logging/LogSystemInterface.h>

#include <algorithm>
#include <cstdarg>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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
/// region and IMDS settings it was configured with -- lets the cache's normalization be asserted offline, with
/// no service SDK and no pointer-identity guesswork.
struct fake_client {
   explicit fake_client(const Aws::Client::ClientConfiguration& cfg)
      : region(cfg.region.c_str(), cfg.region.size())
      , credential_region(cfg.credentialProviderConfig.region.c_str(), cfg.credentialProviderConfig.region.size())
      , imds_disabled(cfg.disableIMDS)
      , credential_imds_disabled(cfg.credentialProviderConfig.imdsConfig.disableImds) {}
   std::string region;
   std::string credential_region;
   bool        imds_disabled;
   bool        credential_imds_disabled;
};

/// Records every AWS SDK log message, down to trace level, while in scope; the prior log system is restored after.
class scoped_aws_log_capture {
public:
   scoped_aws_log_capture() { Aws::Utils::Logging::PushLogger(_log); }
   ~scoped_aws_log_capture() { Aws::Utils::Logging::PopLogger(); }

   scoped_aws_log_capture(const scoped_aws_log_capture&)            = delete;
   scoped_aws_log_capture& operator=(const scoped_aws_log_capture&) = delete;

   /// Whether any captured message contains @p text.
   bool contains(std::string_view text) const { return _log->contains(text); }

private:
   struct recording_log_system : Aws::Utils::Logging::LogSystemInterface {
      Aws::Utils::Logging::LogLevel GetLogLevel() const override { return Aws::Utils::Logging::LogLevel::Trace; }
      void Log(Aws::Utils::Logging::LogLevel, const char*, const char* format, ...) override { record(format); }
      void vaLog(Aws::Utils::Logging::LogLevel, const char*, const char* format, va_list) override { record(format); }
      void LogStream(Aws::Utils::Logging::LogLevel, const char*, const Aws::OStringStream& message) override {
         record(message.str());
      }
      void Flush() override {}

      void record(std::string_view message) {
         const std::lock_guard<std::mutex> lock{mutex};
         messages.emplace_back(message);
      }
      bool contains(std::string_view text) const {
         const std::lock_guard<std::mutex> lock{mutex};
         return std::ranges::any_of(messages, [&](const std::string& m) { return m.find(text) != std::string::npos; });
      }

      mutable std::mutex       mutex;
      std::vector<std::string> messages;
   };

   std::shared_ptr<recording_log_system> _log = std::make_shared<recording_log_system>();
};

/// The trace message the SDK's EC2 metadata client logs before it queries IMDS for the instance region.
constexpr std::string_view imds_region_lookup_trace = "Getting current region for ec2 instance";

} // namespace

BOOST_AUTO_TEST_CASE(explicit_region_reaches_the_client_config) {
   region_client_cache<fake_client> cache;
   const auto client = cache.get("eu-west-2");
   BOOST_REQUIRE(client);
   BOOST_CHECK_EQUAL(client->region, "eu-west-2");
   BOOST_CHECK_EQUAL(client->credential_region, "eu-west-2");
   // IMDS is only skipped while the configuration is built; credentials can still resolve through it.
   BOOST_CHECK(!client->imds_disabled);
   BOOST_CHECK(!client->credential_imds_disabled);
   BOOST_CHECK_EQUAL(client.get(), cache.get("eu-west-2").get());
}

BOOST_AUTO_TEST_CASE(client_configuration_skips_the_imds_region_lookup) {
   // No region from the environment or shared config, and IMDS enabled: exactly the state in which the SDK's default
   // ClientConfiguration constructor queries IMDS for a region.
   const scoped_env_var              default_region{env_default_region, nullptr};
   const scoped_env_var              region{env_region, nullptr};
   const scoped_env_var              imds{env_ec2_metadata_disabled, nullptr};
   const scoped_config_file_redirect config{scoped_unresolvable_region_env::nonexistent_config_path};
   BOOST_REQUIRE(Aws::Internal::GetEC2MetadataClient());

   const scoped_aws_log_capture log;
   AWS_LOGSTREAM_TRACE("test_aws_common", "log capture sentinel");
   BOOST_REQUIRE(log.contains("log capture sentinel"));

   const auto cfg = make_client_configuration("eu-west-2");
   BOOST_CHECK(!log.contains(imds_region_lookup_trace));
   BOOST_CHECK_EQUAL(std::string(cfg.region.c_str(), cfg.region.size()), "eu-west-2");
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
