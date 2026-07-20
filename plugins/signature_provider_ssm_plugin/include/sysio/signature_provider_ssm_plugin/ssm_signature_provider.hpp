#pragma once

/**
 * AWS SSM Parameter Store-backed signature provider -- provider machinery of `signature_provider_ssm_plugin`, installed
 * at `sysio/signature_provider_ssm_plugin/ssm_signature_provider.hpp`.
 *
 * This implements the `SSM:<param-ref>` spec grammar -- a sibling of the built-in `KEY:` / `KIOD:` forms and the kms
 * plugin's `KMS:` -- where the private key is fetched from AWS SSM Parameter Store (a KMS-encrypted `SecureString`
 * parameter) exactly once, when the provider is created, and signing is local thereafter. Semantically this is `KEY:`
 * without the key material ever appearing in config files, command lines, process listings, or shell history:
 *
 *   - The parameter's value is exactly the string that would follow `KEY:` (WIF / `PVT_...` for wire, `PVT_BLS_...`,
 *     `0x...` hex for ethereum, base58 for solana), so every chain key type with a `KEY:` form works.
 *   - After the one startup fetch the signer is a local-key closure and `provider_spec_result::private_key` is
 *     populated -- full `KEY:` parity for every downstream consumer, including the Solana path that requires a raw
 *     local key. Signing latency is identical to `KEY:`, which is what makes `SSM:` suitable for producer block signing
 *     where `KMS:` is not.
 *   - Rotation: the spec pins the public key, so rotating the parameter to a new keypair inherently requires a config
 *     edit + restart; there is nothing useful a re-fetch could do. Fetch-once is semantically forced.
 *
 * The fetch happens inside the spec handler, i.e. when the manager's `plugin_initialize` creates the configured `SSM:`
 * providers. This is the first blocking network I/O at plugin-initialize time in the tree (the KMS provider
 * deliberately stays offline until its startup probe) -- a deliberate departure: the provider cannot exist
 * without the key material, and a boot-time failure with a precise error is the intended behavior for a misconfigured
 * signer. The block is bounded by the AWS SDK's connect / request timeouts and its default retry strategy.
 *
 * The manager plugin never links this code; `SSM:` support is enabled per binary+config with
 * `plugin = sysio::signature_provider_ssm_plugin` (nodeop links the plugin; enablement is the operator's choice). A
 * host application that does not use the plugin can instead link this code and call
 *   plugin.register_spec_handler("SSM", &sysio::sigprov::ssm::create_ssm_provider);
 * from `main()` before `app().initialize(...)`.
 *
 * The lower-level helpers (`parse_ssm_spec`, `fetch_ssm_parameter`, `get_ssm_client`, `throw_ssm_error`,
 * `create_ssm_provider_with_fetcher`) are exposed both because they are well-defined operations on SSM-shaped data and
 * so the test suite can exercise them directly -- in particular the fetcher seam lets the entire provider-construction
 * path run offline with a fake fetch.
 */

#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature_provider.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

/**
 * Forward declarations for the AWS SDK types named in this header's function
 * signatures, mirroring the `kms/` header's approach: the full `<aws/ssm/...>`
 * headers pull in a large dependency tree, and only the translation units that
 * actually talk to AWS (`ssm_signature_provider.cpp`, the live test) need the
 * complete types. `SSMClient` is named only as `std::shared_ptr<SSMClient>`
 * and `AWSError` only behind a reference, so incomplete types suffice here.
 */
namespace Aws {
namespace SSM {
   class SSMClient;
   enum class SSMErrors;
} // namespace SSM
namespace Client {
   template<typename ERROR_TYPE> class AWSError;
} // namespace Client
} // namespace Aws

namespace sysio::sigprov::ssm {

/**
 * @brief Parsed SSM parameter reference.
 *
 * `region` selects the regional `SSMClient`; empty means the spec omitted it, and the client region resolves
 * from the environment (env vars -> shared config -> IMDS on AWS compute; see
 * `sigprov::aws::resolve_default_region`). `name` is handed verbatim to the `Name` field of SSM
 * `GetParameter`, which accepts a parameter name, a full parameter ARN, and the native `name:version` /
 * `name:label` selector forms.
 *
 *   - For an ARN spec, `name` is the full ARN, unmodified. Keeping the ARN
 *     intact preserves the account id (same reasoning as the `kms/` library:
 *     a stripped name would resolve within the *caller's* account and could
 *     silently bind a same-named parameter elsewhere).
 *   - For the shorthand `<region>:<parameter-name>` spec, `name` is
 *     everything after the first `:`, passed through verbatim -- so SSM's
 *     native version / label selectors work with no extra grammar here
 *     (`SSM:us-east-1:/wire/prod/bp1:3` selects version 3).
 *   - For the region-less `<parameter-name>` spec, `name` is the whole body,
 *     again with selector colons passing through
 *     (`SSM:/wire/prod/bp1:3` selects version 3 in the environment-resolved
 *     region).
 */
struct ssm_param_ref {
   std::string region;
   std::string name;
};

/**
 * @brief Parse the body of an `SSM:` provider spec (everything after `SSM:`).
 *
 * Accepted forms:
 *   - Full ARN: `arn:aws:ssm:<region>:<account>:parameter/<path>`
 *     Region is taken from the ARN's region segment; `name` is the full ARN
 *     itself, passed to SSM verbatim so the account id is preserved.
 *   - Shorthand: `<region>:<parameter-name>`
 *     Region is the leading token; everything after the first `:` is `name`
 *     (parameter names cannot contain `:`, so any further colons are SSM's
 *     own `:version` / `:label` selector syntax and pass through unchanged).
 *   - Region-less: `<parameter-name>`
 *     `region` parses as empty, and the client region resolves from the
 *     environment when the provider is created: `AWS_DEFAULT_REGION` /
 *     `AWS_REGION`, then the shared-config profile, then IMDS on AWS compute
 *     (see `sigprov::aws::resolve_default_region`). Resolution never falls
 *     back silently -- when nothing resolves, provider creation throws at
 *     boot rather than fetching from a region the operator didn't choose.
 *
 * The two shorthand forms are told apart by the leading token: one shaped
 * like an AWS region (`sigprov::aws::looks_like_aws_region`) means the
 * explicit-region form; anything else makes the whole body a region-less
 * name, selector colons included. A bare token shaped like a region is
 * rejected as the likely "region without parameter name" typo. The one
 * ambiguous corner is a region-less *selector* reference to a parameter whose
 * own name is shaped like a region (`my-param-2:label` parses as region
 * `my-param-2`); region-shaped-wins is deliberate, and such a parameter stays
 * addressable via the explicit-region or ARN forms. Path-style names
 * (`/wire/...`) never collide -- a region cannot contain `/`.
 *
 * @param spec_data the spec body, e.g. `us-east-1:/wire/prod/bp1` or
 *                  `/wire/prod/bp1`
 * @throws sysio::chain::plugin_config_exception if the form is empty or
 *         malformed
 * @return parsed `ssm_param_ref` ready to hand to the AWS SDK
 */
ssm_param_ref parse_ssm_spec(std::string_view spec_data);

/**
 * @brief Storage type of a fetched parameter.
 *
 * Mirrors `Aws::SSM::Model::ParameterType` -- including its member spellings,
 * so `magic_enum::enum_name` reproduces AWS's own vocabulary ("String",
 * "SecureString") in operator-facing messages -- without leaking AWS model
 * headers into this header or into offline tests. `NOT_SET` covers a response
 * that carried no type.
 */
enum class parameter_type {
   NOT_SET,
   String,
   StringList,
   SecureString,
};

/**
 * @brief A fetched parameter: its decrypted value and its storage type.
 *
 * `value` is secret material (a private key). It must never be logged or
 * embedded in exception messages.
 */
struct fetched_parameter {
   std::string    value;
   parameter_type type = parameter_type::NOT_SET;
};

/**
 * @brief Fetch seam: maps a parsed parameter reference to its fetched value.
 *
 * `create_ssm_provider` binds the real AWS-backed `fetch_ssm_parameter`;
 * offline tests inject fakes (fixed values, wrong types, throwing fetchers)
 * through `create_ssm_provider_with_fetcher` to exercise every construction
 * path without network or credentials.
 */
using parameter_fetcher = std::function<fetched_parameter(const ssm_param_ref&)>;

/**
 * @brief Get (or lazily create) a process-wide `SSMClient` for `region`.
 *
 * Shares the process-wide AWS SDK lifecycle and the per-region cache
 * semantics with the kms plugin (see
 * `sysio::sigprov::aws::region_client_cache`). Construction is offline: no
 * credential resolution, no network. Credentials resolve via the standard AWS
 * provider chain on the first API call. The one exception is an empty
 * `region` (a region-less spec): the effective region then resolves via
 * `sigprov::aws::resolve_default_region`, whose IMDS step may touch the
 * instance-metadata endpoint, and which throws when nothing resolves.
 *
 * @param region AWS region (e.g. `us-east-1`), or empty to use the
 *               environment-resolved default region
 * @return shared `SSMClient` configured for the effective region
 */
std::shared_ptr<Aws::SSM::SSMClient> get_ssm_client(const std::string& region);

/**
 * @brief Translate a failed AWS SSM API outcome into an fc exception, split
 *        by whether the failure is transient.
 *
 * Delegates to `sysio::sigprov::aws::throw_aws_error`: a retryable error
 * (throttling, timeout, service-internal) throws
 * `sysio::chain::signing_transient_exception`; a permanent one
 * (`ParameterNotFound`, access denied, KMS decrypt denied) throws
 * `sysio::chain::plugin_config_exception` with an IAM remediation hint
 * appended (`ssm:GetParameter` on the parameter, `kms:Decrypt` on its key).
 *
 * @param op short label for the failed operation (e.g. "GetParameter")
 * @param parameter_name the parameter name / ARN the call targeted
 * @param err the failed outcome's AWS error
 * @throws sysio::chain::signing_transient_exception if `err` is retryable
 * @throws sysio::chain::plugin_config_exception otherwise
 */
[[noreturn]] void throw_ssm_error(std::string_view op, std::string_view parameter_name,
                                  const Aws::Client::AWSError<Aws::SSM::SSMErrors>& err);

/**
 * @brief Fetch a parameter's decrypted value and type from AWS SSM.
 *
 * Issues `GetParameter` with `WithDecryption=true` on the shared regional
 * client. The AWS SDK's default retry strategy handles transient errors
 * internally; a still-failed outcome lands in `throw_ssm_error`.
 *
 * @param ref parsed `(region, name)` pair
 * @throws sysio::chain::signing_transient_exception on a retryable failure
 *         that outlasted the SDK's own retries
 * @throws sysio::chain::plugin_config_exception on a permanent failure
 * @return the parameter's decrypted value and storage type
 */
fetched_parameter fetch_ssm_parameter(const ssm_param_ref& ref);

/**
 * @brief Core provider construction with an injectable fetch -- the offline
 *        testing seam behind `create_ssm_provider`.
 *
 * Pipeline (all failures throw, so a misconfigured signer fails the boot):
 *   1. `parse_ssm_spec(spec_data)`.
 *   2. Reject chain key types without a `KEY:`-style native form
 *      (`chain_key_type_sui` -> `pending_impl_exception`, unknown values ->
 *      `config_parse_error` -- mirroring the plugin's `KEY:` taxonomy).
 *   3. `fetcher(ref)` -- the one network round-trip in the real path.
 *   4. Require `parameter_type::SecureString`. A key stored as a plaintext
 *      `String` (or a `StringList`) is refused outright with the exact
 *      `put-parameter --type SecureString` remediation in the message: keys
 *      at rest must be KMS-encrypted, and boot time is the cheapest moment to
 *      catch the mistake.
 *   5. Trim ASCII whitespace (operators create parameters from shells; a
 *      trailing newline must not brick a producer) and reject an empty value.
 *   6. Parse the value via the runtime `fc::crypto::from_native_string_to_private_key`.
 *      A parse failure is re-thrown as `plugin_config_exception` naming the
 *      parameter but deliberately NOT the underlying parser message, which
 *      could echo the secret value.
 *   7. Verify the derived public key matches the spec's pinned `<public-key>`
 *      (the plugin core performs this check only for `KEY:`; extension
 *      handlers own it).
 *
 * @param key_type     chain key type from the surrounding spec
 * @param expected_pub public key pinned in the surrounding spec
 * @param spec_data    the spec body after `SSM:`
 * @param fetcher      fetch implementation (real AWS call or test fake)
 * @throws sysio::chain::plugin_config_exception on malformed spec, fetch
 *         failure, non-SecureString parameter, empty / unparseable value, or
 *         pinned-pubkey mismatch
 * @throws sysio::chain::pending_impl_exception for `chain_key_type_sui`
 * @throws sysio::chain::signing_transient_exception if the fetcher classified
 *         its failure as retryable (boot still fails; restarting retries)
 * @return packaged result: local-sign closure, populated `private_key`, no
 *         startup probe (nothing is left to check after the eager fetch)
 */
sysio::provider_spec_result create_ssm_provider_with_fetcher(fc::crypto::chain_key_type_t  key_type,
                                                             const fc::crypto::public_key& expected_pub,
                                                             std::string_view              spec_data,
                                                             const parameter_fetcher&      fetcher);

/**
 * @brief Adapter that fits `signature_provider_manager_plugin`'s
 *        `sysio::spec_handler` signature.
 *
 * `create_ssm_provider_with_fetcher` bound to the real AWS-backed
 * `fetch_ssm_parameter`. Suitable for direct registration:
 *
 *   plugin.register_spec_handler("SSM", &sysio::sigprov::ssm::create_ssm_provider);
 *
 * @param key_type     chain key type from the surrounding spec
 * @param expected_pub public key from the surrounding spec
 * @param spec_data    the spec body after `SSM:`
 * @return packaged result ready to install in the plugin's registry
 */
sysio::provider_spec_result create_ssm_provider(fc::crypto::chain_key_type_t  key_type,
                                                const fc::crypto::public_key& expected_pub,
                                                std::string_view              spec_data);

} // namespace sysio::sigprov::ssm
