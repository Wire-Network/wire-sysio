#pragma once

/**
 * Shared AWS SDK glue for the AWS-backed signature-provider plugins
 * (`signature_provider_kms_plugin`, `signature_provider_ssm_plugin`),
 * installed at `sysio/signature_provider_aws/aws_common.hpp`.
 *
 * Everything here is service-agnostic: SDK lifecycle, the per-region client
 * cache, the transient-vs-permanent error split, and the generic pieces of ARN
 * parsing. Service-specific code (the KMS Sign/GetPublicKey plumbing, the SSM
 * GetParameter fetch, their spec grammars) stays in the respective plugin.
 *
 * This library exists for one correctness-critical reason beyond code reuse:
 * `Aws::InitAPI` / `Aws::ShutdownAPI` must be called exactly once per process.
 * When more than one provider plugin is linked into the same binary, each
 * owning its own lifecycle singleton would double-init / double-shutdown the
 * SDK. `ensure_aws_sdk_initialized()` is the single process-wide owner.
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
 * @brief Process-wide, per-region cache of AWS service clients.
 *
 * One instance per service client type, held as a function-local static by the
 * owning sub-library (see `get_kms_client` / `get_ssm_client`). Lock once on
 * lookup; the SDK's HTTP pool inside a client is itself thread-safe, so
 * multiple closures sharing a client may submit requests concurrently.
 *
 * Construction of a client is offline: no credential resolution, no network.
 * Credentials are looked up via the standard AWS provider chain on the first
 * API call, not here. The client configuration carries the region and nothing
 * else -- region comes only from the provider spec, never from `AWS_REGION` or
 * shared-config fallbacks, so a misconfiguration fails as a parse error rather
 * than as a mysterious wrong-region API call.
 *
 * The constructor runs `ensure_aws_sdk_initialized()`: wherever an instance of
 * this cache is created, the SDK lifecycle singleton is thereby the older
 * static and `Aws::ShutdownAPI` runs only after this cache has released its
 * clients.
 */
template<typename Client>
class region_client_cache {
public:
   region_client_cache() { ensure_aws_sdk_initialized(); }

   /**
    * Get (or lazily create) the shared client for `region`.
    *
    * @param region AWS region (e.g. `us-east-1`); must be non-empty
    * @return shared client configured for `region`
    */
   std::shared_ptr<Client> get(const std::string& region) {
      SYS_ASSERT(!region.empty(), chain::plugin_config_exception,
                 "AWS client cache lookup requires a non-empty region");
      std::scoped_lock lock{_mutex};
      auto& slot = _by_region[region];
      if (!slot) {
         Aws::Client::ClientConfiguration cfg;
         cfg.region = Aws::String{region};
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

} // namespace sysio::sigprov::aws
