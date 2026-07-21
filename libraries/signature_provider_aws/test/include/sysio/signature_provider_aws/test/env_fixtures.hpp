#pragma once

/**
 * Test-only RAII environment fixtures for the AWS-backed signature-provider suites, exposed through the
 * `signature_provider_aws_test` INTERFACE target and consumed by this library's own unit binary and the
 * kms/ssm plugin suites.
 *
 * `resolve_default_region` reads live process state -- environment variables, the SDK's cached shared-config
 * file, and (last) IMDS. A test that exercises it must pin exactly the state it means to test and put the
 * host's state back afterwards, or one case's redirect leaks into every later case in the binary (and the
 * host's real `~/.aws/config` leaks into the cases). Everything here is scoped for that reason.
 */

#include <sysio/signature_provider_aws/aws_common.hpp>

#include <aws/core/config/ConfigAndCredentialsCacheManager.h>

#include <cstdlib>
#include <optional>
#include <string>

namespace sysio::sigprov::aws::test {

/**
 * @brief RAII override of one environment variable.
 *
 * Sets (`value` non-null) or clears (`value` null) the variable at construction; restores the prior state --
 * including prior absence -- at destruction.
 */
class scoped_env_var {
public:
   scoped_env_var(const char* name, const char* value)
      : _name(name) {
      if (const char* prior = std::getenv(name))
         _prior = prior;
      apply(value);
   }
   ~scoped_env_var() { apply(_prior ? _prior->c_str() : nullptr); }

   scoped_env_var(const scoped_env_var&)            = delete;
   scoped_env_var(scoped_env_var&&)                 = delete;
   scoped_env_var& operator=(const scoped_env_var&) = delete;
   scoped_env_var& operator=(scoped_env_var&&)      = delete;

private:
   void apply(const char* value) const {
      if (value)
         ::setenv(_name, value, /* overwrite */ 1);
      else
         ::unsetenv(_name);
   }

   const char*                _name;
   std::optional<std::string> _prior;
};

/**
 * @brief RAII redirect of the SDK's shared-config file.
 *
 * Points `AWS_CONFIG_FILE` at `path` and reloads the SDK's config cache so `GetCachedConfigValue` sees the
 * redirect immediately. On destruction the prior value is restored FIRST and the cache reloaded again, so
 * the redirect cannot leak into later cases through the cache.
 */
class scoped_config_file_redirect {
public:
   explicit scoped_config_file_redirect(const char* path) {
      // The config cache is owned by the SDK lifecycle -- make sure it exists before reloading it.
      ensure_aws_sdk_initialized();
      _env.emplace(env_config_file, path);
      Aws::Config::ReloadCachedConfigFile();
   }
   ~scoped_config_file_redirect() {
      _env.reset();
      Aws::Config::ReloadCachedConfigFile();
   }

   scoped_config_file_redirect(const scoped_config_file_redirect&)            = delete;
   scoped_config_file_redirect(scoped_config_file_redirect&&)                 = delete;
   scoped_config_file_redirect& operator=(const scoped_config_file_redirect&) = delete;
   scoped_config_file_redirect& operator=(scoped_config_file_redirect&&)      = delete;

   /// The env var the AWS SDK reads for the shared-config file location.
   static constexpr const char* env_config_file = "AWS_CONFIG_FILE";

private:
   std::optional<scoped_env_var> _env;
};

/**
 * @brief Pins the whole region-resolution chain to "nothing resolves".
 *
 * Both region env vars cleared, the shared-config file redirected to a path that cannot exist (so the host's
 * real `~/.aws/config` cannot satisfy the profile step), and IMDS disabled (so the chain cannot reach the
 * metadata endpoint -- and cannot stall probing for it on non-AWS test hosts). With this in scope,
 * `resolve_default_region` MUST throw; that is exactly what the unresolvable-region cases assert.
 */
struct scoped_unresolvable_region_env {
   /// A path no test host has: keeps the shared-config step empty regardless of the host's AWS setup.
   static constexpr const char* nonexistent_config_path = "/nonexistent/wire-sigprov-test-aws-config";

   scoped_env_var              default_region{env_default_region, nullptr};
   scoped_env_var              region{env_region, nullptr};
   scoped_env_var              imds{env_ec2_metadata_disabled, metadata_disabled_true};
   scoped_config_file_redirect config{nonexistent_config_path};
};

} // namespace sysio::sigprov::aws::test
