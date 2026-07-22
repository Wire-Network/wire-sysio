#pragma once

/**
 * Shared AWS SDK glue for the AWS-backed signature-provider plugins (`signature_provider_kms_plugin`,
 * `signature_provider_ssm_plugin`), installed at `sysio/signature_provider_aws/aws_common.hpp`.
 *
 * Everything here is service-agnostic: SDK lifecycle, the per-region client cache, the transient-vs-permanent error
 * split, and the generic pieces of ARN parsing. Service-specific code (the KMS Sign/GetPublicKey plumbing, the SSM
 * GetParameter fetch, their spec grammars) stays in the respective plugin.
 *
 * This library exists for one correctness-critical reason beyond code reuse: `Aws::InitAPI` / `Aws::ShutdownAPI` must
 * be called exactly once per process. When more than one provider plugin is linked into the same binary, each owning
 * its own lifecycle singleton would double-init / double-shutdown the SDK. `ensure_aws_sdk_initialized()` is the single
 * process-wide owner.
 */

#include <sysio/chain/exceptions.hpp>

#include <fc/exception/exception.hpp>

#include <aws/core/client/AWSError.h>
#include <aws/core/client/ClientConfiguration.h>

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace sysio::sigprov::aws {

/// Environment / shared-config keys consulted by `resolve_default_region`, listed in the order they are
/// consulted -- the same order the AWS SDK's own `ClientConfiguration` constructors use. Exposed so the test
/// fixtures pin the exact same spellings the production chain reads.
inline constexpr const char* env_default_region        = "AWS_DEFAULT_REGION";
inline constexpr const char* env_region                = "AWS_REGION";
inline constexpr const char* config_key_region         = "region";
inline constexpr const char* env_ec2_metadata_disabled = "AWS_EC2_METADATA_DISABLED";

/// The value `AWS_EC2_METADATA_DISABLED` must carry (compared case-insensitively, matching the SDK's own
/// check) for the IMDS step of `resolve_default_region` to be skipped.
inline constexpr const char* metadata_disabled_true    = "true";

/**
 * @brief Construct the process-wide AWS SDK lifecycle if it has not been
 *        constructed yet.
 *
 * The first call runs `Aws::InitAPI`; `Aws::ShutdownAPI` runs at static
 * destruction. Calling this before creating any AWS client (or any static
 * object holding one) pins the lifecycle as the *older* static, so shutdown
 * runs after that object's destructor -- `region_client_cache` relies on this
 * from its constructor.
 *
 * Threadsafe (function-local static). Idempotent.
 */
void ensure_aws_sdk_initialized();

/**
 * @brief Resolve the AWS region to use when a provider spec does not name one.
 *
 * Mirrors the resolution chain the AWS SDK's own `ClientConfiguration` constructors run -- environment
 * (`AWS_DEFAULT_REGION`, then `AWS_REGION`), then the shared-config profile's `region`, then the EC2
 * instance-metadata service (IMDS, skipped when `AWS_EC2_METADATA_DISABLED=true`) -- with one deliberate
 * difference: where the SDK silently falls back to `us-east-1` when nothing resolves, this throws. A signing
 * key must never be looked up in a region the operator didn't choose; "no region anywhere" is a boot-time
 * configuration error, not a default.
 *
 * The IMDS step is network I/O (a link-local HTTP round-trip, bounded by the SDK's short metadata timeouts):
 * on AWS compute it answers in milliseconds; elsewhere it fails fast and resolution falls through to the
 * throw. It runs only when the spec omitted its region AND neither the environment nor the shared config
 * supplied one.
 *
 * The result is deliberately not memoized -- region-less lookups happen a handful of times at
 * plugin-initialize, and `region_client_cache` memoizes the *client* under the resolved region, which is the
 * cache that matters.
 *
 * @throws sysio::chain::plugin_config_exception when no region can be resolved
 * @return the resolved region; never empty
 */
std::string resolve_default_region();

/**
 * @brief Process-wide, per-region cache of AWS service clients.
 *
 * One instance per service client type, held as a function-local static by the owning provider (see `get_kms_client` /
 * `get_ssm_client`). Lookups are serialized by an internal mutex: today every creation happens during the sequential,
 * main-thread initialize phase, but the owning helpers are public and documented threadsafe, so the cache does not
 * lean on that phase discipline. The mutex covers map lookup/insertion and client construction; the region resolution
 * for an empty `region` runs before the lock is taken -- it reads no cache state, and its IMDS step may perform
 * bounded network I/O that has no business holding the lock. At runtime the thread-safety that matters lives inside
 * the AWS client itself: the SDK's HTTP pool is thread-safe, so multiple closures sharing a client may submit
 * requests concurrently.
 *
 * Construction of a client is offline: no credential resolution, no network. Credentials are looked up via the standard
 * AWS provider chain on the first API call, not here. The client configuration carries the region and nothing else. An
 * explicit region comes from the provider spec; a spec that omitted its region resolves one through
 * `resolve_default_region()` -- whose IMDS step is the one exception to "no network" on this path -- and the client is
 * then cached under the *resolved* region, so an explicit `us-east-1` spec and a region-less spec resolving to
 * `us-east-1` share one client. Resolution never silently defaults: an unresolvable region throws here, at provider
 * creation, not as a mysterious wrong-region API call later.
 *
 * The constructor runs `ensure_aws_sdk_initialized()`: wherever an instance of this cache is created, the SDK lifecycle
 * singleton is thereby the older static and `Aws::ShutdownAPI` runs only after this cache has released its clients.
 */
template<typename Client>
class region_client_cache {
public:
   region_client_cache() { ensure_aws_sdk_initialized(); }

   /**
    * Get (or lazily create) the shared client for `region`.
    *
    * @param region AWS region (e.g. `us-east-1`), or empty to use the environment-resolved default region
    *               (see `resolve_default_region()`, which throws when nothing resolves)
    * @return shared client configured for the effective region
    */
   std::shared_ptr<Client> get(const std::string& region) {
      const std::string effective = region.empty() ? resolve_default_region() : region;
      const std::lock_guard<std::mutex> lock{_mutex};
      auto& slot = _by_region[effective];
      if (!slot) {
         Aws::Client::ClientConfiguration cfg;
         cfg.region = Aws::String{effective};
         slot = std::make_shared<Client>(cfg);
      }
      return slot;
   }

private:
   std::mutex                                       _mutex;
   std::map<std::string, std::shared_ptr<Client>>   _by_region;
};

/**
 * @brief Translate a failed AWS API outcome into an fc exception, split by
 *        whether the failure is transient.
 *
 * The AWS SDK classifies every deserialised error as retryable or not -- the
 * same classification its own retry strategy uses. This maps that split onto
 * two distinct exception types so a caller can react correctly:
 *
 *   - Transient (throttling, service-internal errors, dependency / network
 *     timeouts, service-unavailable) -> `sysio::chain::signing_transient_exception`.
 *     The operation may be retried with backoff; the credentials and resource
 *     are fine.
 *   - Permanent (access denied, resource not found, invalid state, bad
 *     parameters) -> `sysio::chain::plugin_config_exception`. Retrying will not
 *     help -- the operator must fix credentials, IAM, region, or the spec.
 *
 * The two types are siblings, not parent and child, so a handler that catches
 * only `plugin_config_exception` will not silently swallow a retryable error.
 * The SDK's own classification is authoritative -- there is no hand-maintained
 * table of error codes here to drift out of date.
 *
 * The caller's translation unit must make a
 * `magic_enum::customize::enum_range<ErrorT>` specialization visible before
 * instantiating this template: AWS service error enums start above
 * magic_enum's default ceiling of 127 (e.g. `KMSErrors` at 129), so
 * `enum_name` would otherwise return an empty string for them.
 *
 * @tparam ErrorT         the service's error enum (e.g. `Aws::KMS::KMSErrors`)
 * @param service         short service label used in the message ("KMS", "SSM")
 * @param op              short label for the failed operation (e.g. "Sign")
 * @param resource_noun   what the id names in the message ("key", "parameter")
 * @param resource_id     the key id / parameter name / ARN the call targeted
 * @param err             the failed outcome's AWS error
 * @param permanent_hint  optional remediation sentence appended to permanent
 *                        failures only (e.g. the IAM actions to grant)
 * @throws sysio::chain::signing_transient_exception if `err` is retryable
 * @throws sysio::chain::plugin_config_exception otherwise
 */
template<typename ErrorT>
[[noreturn]] void throw_aws_error(std::string_view service, std::string_view op, std::string_view resource_noun,
                                  std::string_view resource_id, const Aws::Client::AWSError<ErrorT>& err,
                                  std::string_view permanent_hint = {}) {
   const bool transient = err.ShouldRetry();
   auto message = fmt::format(
      "AWS {} {} for {} \"{}\" failed: {} (status {}, {}) [{}]: {}",
      service, op, resource_noun, resource_id,
      magic_enum::enum_name(err.GetErrorType()),
      magic_enum::enum_integer(err.GetResponseCode()),
      err.GetExceptionName(),
      transient ? "transient, retryable" : "permanent",
      err.GetMessage());

   if (transient) {
      FC_THROW_EXCEPTION(chain::signing_transient_exception, "{}", message);
   }
   if (!permanent_hint.empty()) {
      message += fmt::format(" {}", permanent_hint);
   }
   FC_THROW_EXCEPTION(chain::plugin_config_exception, "{}", message);
}

/// Case-insensitive lead-in shared by every ARN, of any partition or service.
/// A spec that begins with this but does not match the owning sub-library's
/// service prefix (`arn:aws:kms:` / `arn:aws:ssm:`) is a malformed or
/// out-of-scope ARN -- never the shorthand `<region>:<id>` form -- and must
/// fail loudly rather than fall through to a shorthand parser that would
/// silently yield region="arn".
inline constexpr std::string_view arn_lead_in = "arn:";

/// Number of colon-separated segments in a well-formed service-resource ARN:
/// `arn`, `<partition>`, `<service>`, `<region>`, `<account>`, `<resource>`.
inline constexpr std::size_t arn_segment_count = 6;

/// Indices into the split ARN.
inline constexpr std::size_t arn_idx_partition = 1;
inline constexpr std::size_t arn_idx_service   = 2;
inline constexpr std::size_t arn_idx_region    = 3;
inline constexpr std::size_t arn_idx_account   = 4;
inline constexpr std::size_t arn_idx_tail      = 5;

/// Case-insensitive ASCII prefix test. ARN partitions and services are
/// lowercase by convention, but an operator may paste a mis-cased
/// `ARN:AWS:...`; the parsers still want to recognise it as an ARN so it fails
/// loudly rather than being mistaken for the shorthand `<region>:<id>` form.
inline bool starts_with_ci(std::string_view s, std::string_view prefix) {
   if (s.size() < prefix.size())
      return false;
   return std::equal(prefix.begin(), prefix.end(), s.begin(),
                     [](unsigned char a, unsigned char b) {
                        return std::tolower(a) == std::tolower(b);
                     });
}

/// True when `s` is shaped like an AWS region code -- `us-east-1`, `ap-southeast-3`, `us-gov-west-1`,
/// `us-isob-east-1`: three or more hyphen-separated segments, the first all lowercase ASCII alpha (at least two
/// characters), the last all ASCII digits, any between lowercase alphanumeric. Every region of every current AWS
/// partition matches; KMS key ids (uuids, `mrk-...`), alias names (`alias/...`), and path-style SSM parameter names
/// do not. The spec parsers use this to decide whether a shorthand's leading token is an explicit region or part of
/// a region-less key/parameter reference. Character classes are spelled out (not `std::isalpha`/`std::isdigit`) so
/// classification cannot follow the process locale.
inline bool looks_like_aws_region(std::string_view s) {
   constexpr std::size_t minimum_lead_segment_size = 2;
   constexpr std::size_t minimum_segment_count     = 3;
   constexpr auto is_lower_alpha = [](char c) { return c >= 'a' && c <= 'z'; };
   constexpr auto is_digit       = [](char c) { return c >= '0' && c <= '9'; };
   constexpr auto is_lower_alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };

   std::size_t segment_count = 0;
   for (std::size_t start = 0;;) {
      const auto hyphen  = s.find('-', start);
      const bool is_last = hyphen == std::string_view::npos;
      const auto segment = is_last ? s.substr(start) : s.substr(start, hyphen - start);
      ++segment_count;
      if (segment.empty())
         return false;
      if (is_last)
         return segment_count >= minimum_segment_count &&
                std::all_of(segment.begin(), segment.end(), is_digit);
      if (segment_count == 1) {
         if (segment.size() < minimum_lead_segment_size ||
             !std::all_of(segment.begin(), segment.end(), is_lower_alpha))
            return false;
      } else if (!std::all_of(segment.begin(), segment.end(), is_lower_alnum)) {
         return false;
      }
      start = hyphen + 1;
   }
}

} // namespace sysio::sigprov::aws
