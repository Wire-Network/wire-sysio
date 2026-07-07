#include <sysio/signature_provider_manager_plugin/ssm/ssm_signature_provider.hpp>

#include <sysio/signature_provider_manager_plugin/aws/aws_common.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/types.hpp>

#include <fc/exception/exception.hpp>
#include <fc/string.hpp>

#include <aws/ssm/SSMClient.h>
#include <aws/ssm/SSMErrors.h>
#include <aws/ssm/model/GetParameterRequest.h>
#include <aws/ssm/model/Parameter.h>
#include <aws/ssm/model/ParameterType.h>

#include <magic_enum/magic_enum.hpp>

#include <memory>
#include <string>
#include <string_view>

// SSMErrors codes begin at 129 (SERVICE_EXTENSION_START_RANGE + 1) and -- SSM
// having one of the largest error vocabularies of any AWS service -- run well
// past magic_enum's default ceiling of 127, so enum_name() would return "" for
// them without a wider range. Scope the widening to this one enum rather than
// the global MAGIC_ENUM_RANGE_MAX macro, which would affect every enum in the
// TU (incl. transitively-included chain enums) and risk an ODR mismatch with
// other TUs at the default range. 512 comfortably covers the ~130 service
// codes above the 128 extension floor.
template<>
struct magic_enum::customize::enum_range<Aws::SSM::SSMErrors> {
   static constexpr int min = 0;
   static constexpr int max = 512;
};

namespace sysio::sigprov::ssm {

namespace {

/// Anchor for ARN detection. Parameter ARNs always start with `arn:aws:ssm:`
/// (non-`aws` partitions such as `aws-cn` / `aws-us-gov` are out of scope,
/// same boundary as the `kms/` sibling). The service-agnostic ARN pieces
/// (`arn:` lead-in, segment count / indices, the case-insensitive prefix
/// test) come from `sysio::sigprov::aws` (aws_common.hpp).
constexpr std::string_view ssm_arn_prefix = "arn:aws:ssm:";

/// Resource-tail prefix of a parameter ARN:
/// `arn:aws:ssm:<region>:<account>:parameter/<path>`.
constexpr std::string_view tail_prefix_parameter = "parameter/";

/// Remediation appended (by the shared error split) to permanent GetParameter
/// failures only -- the two IAM actions an SSM signing key needs.
constexpr std::string_view ssm_permanent_hint =
   "Verify the caller's IAM identity is granted ssm:GetParameter on the parameter and "
   "kms:Decrypt on the KMS key that encrypts it, and that the region and parameter name "
   "in the spec are correct.";

/// Strip leading and trailing ASCII whitespace. Operators create parameters
/// from shells (`aws ssm put-parameter --value "$(cat key.txt)"`), so a
/// trailing newline is a likely artifact that must not brick a producer key.
/// The whitespace set is spelled out explicitly rather than delegated to
/// `std::isspace`, whose classification follows the process locale -- what
/// gets trimmed from a secret must not depend on ambient locale state.
std::string_view trim_ascii_whitespace(std::string_view s) {
   const auto is_space = [](char c) {
      return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
   };
   while (!s.empty() && is_space(s.front()))
      s.remove_prefix(1);
   while (!s.empty() && is_space(s.back()))
      s.remove_suffix(1);
   return s;
}

/// Map the SDK's parameter-type enum onto the header's AWS-free mirror. The
/// SDK's generated `ParameterTypeMapper` emits the canonical spelling
/// ("String", "StringList", "SecureString"), which `parameter_type` mirrors
/// member-for-member, so a name-based cast converts without a hand-rolled
/// switch and without `static_cast` across unrelated enum types.
parameter_type to_parameter_type(Aws::SSM::Model::ParameterType t) {
   const auto name = Aws::SSM::Model::ParameterTypeMapper::GetNameForParameterType(t);
   return magic_enum::enum_cast<parameter_type>(std::string_view{name.c_str(), name.size()})
      .value_or(parameter_type::NOT_SET);
}

} // namespace

ssm_param_ref parse_ssm_spec(std::string_view spec_data) {
   SYS_ASSERT(!spec_data.empty(), chain::plugin_config_exception,
              "SSM spec body is empty; expected an ARN or '<region>:<parameter-name>'");

   if (spec_data.starts_with(ssm_arn_prefix)) {
      // Full ARN form. Split into exactly `aws::arn_segment_count` parts so
      // any further colons in the trailing segment (SSM's own `:version` /
      // `:label` selector syntax) stay glued to it. The split is only for
      // *validation* below -- the value handed to SSM is the unmodified ARN.
      auto parts = fc::split(spec_data, ':', sigprov::aws::arn_segment_count);
      SYS_ASSERT(parts.size() == sigprov::aws::arn_segment_count, chain::plugin_config_exception,
                 "Malformed SSM parameter ARN \"{}\": expected {} colon-separated segments, got {}",
                 spec_data, sigprov::aws::arn_segment_count, parts.size());

      const auto& region  = parts[sigprov::aws::arn_idx_region];
      const auto& account = parts[sigprov::aws::arn_idx_account];
      const auto& tail    = parts[sigprov::aws::arn_idx_tail];

      // `arn`, `aws`, and `ssm` are guaranteed non-empty and correct by the
      // `ssm_arn_prefix` match above. The region, account, and tail segments
      // are operator-supplied; an empty one means a stray colon collapsed two
      // segments, producing a malformed ARN. Reject that here with a precise
      // message rather than at the API call against a bad endpoint.
      SYS_ASSERT(!region.empty(), chain::plugin_config_exception,
                 "SSM parameter ARN \"{}\" has empty region segment", spec_data);
      SYS_ASSERT(!account.empty(), chain::plugin_config_exception,
                 "SSM parameter ARN \"{}\" has empty account-id segment", spec_data);
      SYS_ASSERT(tail.starts_with(tail_prefix_parameter), chain::plugin_config_exception,
                 "SSM parameter ARN tail must start with 'parameter/', got \"{}\" in \"{}\"",
                 tail, spec_data);
      SYS_ASSERT(tail.size() > tail_prefix_parameter.size(), chain::plugin_config_exception,
                 "SSM parameter ARN tail \"{}\" has empty parameter path", tail);

      // Hand SSM the full ARN, not a stripped path: GetParameter accepts a
      // parameter ARN as `Name`, and the intact ARN preserves the account id
      // (same account-identity reasoning as the kms/ sibling's key ARNs).
      // `region` is still taken from the ARN to build the regional client; it
      // matches the region embedded in the ARN we pass through.
      return ssm_param_ref{region, std::string{spec_data}};
   }

   // A spec that begins with `arn:` (any casing) but did not match the
   // supported `arn:aws:ssm:` form above is a malformed or out-of-scope ARN,
   // never shorthand. Falling through to the `<region>:<parameter-name>`
   // parser below would split on the first colon and silently yield
   // region="arn"; AWS then rejects that only at the API call, with an opaque
   // endpoint error. Fail loudly here instead, naming the offending partition
   // and service -- mis-cased `ARN:AWS:SSM:...` and typo'd services land here
   // too.
   if (sigprov::aws::starts_with_ci(spec_data, sigprov::aws::arn_lead_in)) {
      const auto parts = fc::split(spec_data, ':', sigprov::aws::arn_segment_count);
      std::string partition, service;
      if (parts.size() > sigprov::aws::arn_idx_partition)
         partition = parts[sigprov::aws::arn_idx_partition];
      if (parts.size() > sigprov::aws::arn_idx_service)
         service = parts[sigprov::aws::arn_idx_service];
      FC_THROW_EXCEPTION(chain::plugin_config_exception,
                         "Unsupported SSM ARN \"{}\": only the 'arn:aws:ssm:' partition/service "
                         "is supported (got partition \"{}\", service \"{}\"). Non-'aws' "
                         "partitions such as 'aws-cn' and 'aws-us-gov' are out of scope.",
                         spec_data, partition, service);
   }

   // Shorthand `<region>:<parameter-name>`. Split on the first colon only:
   // parameter names cannot contain `:`, so anything after a further colon is
   // SSM's own `name:version` / `name:label` selector syntax and passes
   // through to GetParameter unchanged.
   const auto colon = spec_data.find(':');
   SYS_ASSERT(colon != std::string_view::npos, chain::plugin_config_exception,
              "SSM spec \"{}\" must include a region: expected '<region>:<parameter-name>' "
              "or a full 'arn:aws:ssm:...' ARN", spec_data);
   SYS_ASSERT(colon > 0, chain::plugin_config_exception,
              "SSM spec \"{}\" has empty region", spec_data);
   SYS_ASSERT(colon + 1 < spec_data.size(), chain::plugin_config_exception,
              "SSM spec \"{}\" has empty parameter name", spec_data);

   return ssm_param_ref{
      std::string{spec_data.substr(0, colon)},
      std::string{spec_data.substr(colon + 1)},
   };
}

std::shared_ptr<Aws::SSM::SSMClient> get_ssm_client(const std::string& region) {
   SYS_ASSERT(!region.empty(), chain::plugin_config_exception,
              "get_ssm_client: region must not be empty");

   // Function-local static, constructed on first use. Its constructor runs
   // `ensure_aws_sdk_initialized()`, pinning the SDK lifecycle singleton as
   // the older static so Aws::ShutdownAPI runs only after this cache has
   // released its SSMClient shared_ptrs.
   static sigprov::aws::region_client_cache<Aws::SSM::SSMClient> cache;
   return cache.get(region);
}

[[noreturn]] void throw_ssm_error(std::string_view op, std::string_view parameter_name,
                                  const Aws::Client::AWSError<Aws::SSM::SSMErrors>& err) {
   // Classification (ShouldRetry -> transient vs permanent) and message shape
   // live in the shared `sysio::sigprov::aws::throw_aws_error`; see its doc
   // for the full rationale. This wrapper contributes the SSM service label,
   // the "parameter" resource noun, and the IAM remediation hint appended to
   // permanent failures.
   sigprov::aws::throw_aws_error("SSM", op, "parameter", parameter_name, err, ssm_permanent_hint);
}

fetched_parameter fetch_ssm_parameter(const ssm_param_ref& ref) {
   auto client = get_ssm_client(ref.region);

   // WithDecryption asks SSM to return a SecureString's plaintext (requiring
   // kms:Decrypt on its key); it is a documented no-op for plain String
   // parameters, which `create_ssm_provider_with_fetcher` rejects by type.
   Aws::SSM::Model::GetParameterRequest req;
   req.SetName(Aws::String{ref.name});
   req.SetWithDecryption(true);

   auto outcome = client->GetParameter(req);
   if (!outcome.IsSuccess()) {
      throw_ssm_error("GetParameter", ref.name, outcome.GetError());
   }

   const auto& param = outcome.GetResult().GetParameter();
   const auto& value = param.GetValue();
   return fetched_parameter{
      std::string{value.c_str(), value.size()},
      to_parameter_type(param.GetType()),
   };
}

sysio::provider_spec_result create_ssm_provider_with_fetcher(fc::crypto::chain_key_type_t  key_type,
                                                             const fc::crypto::public_key& expected_pub,
                                                             std::string_view              spec_data,
                                                             const parameter_fetcher&      fetcher) {
   using namespace fc::crypto;

   const auto ref = parse_ssm_spec(spec_data);

   // Reject key types without a `KEY:`-style native form before spending the
   // network round-trip. Mirrors the plugin's `KEY:` taxonomy: sui is a known
   // type pending implementation; anything else is an invalid enum value.
   switch (key_type) {
   case chain_key_type_wire:
   case chain_key_type_wire_bls:
   case chain_key_type_ethereum:
   case chain_key_type_solana:
      break;
   case chain_key_type_sui:
      // to_fc_string throughout (here and the plugin's KEY: arms): unlike to_string, it is total --
      // an out-of-range value formats as its number instead of throwing bad_enum_cast mid-throw.
      FC_THROW_EXCEPTION(chain::pending_impl_exception, "Key type needs to be implemented: {}",
                         chain_key_type_reflector::to_fc_string(key_type));
   default:
      FC_THROW_EXCEPTION(chain::config_parse_error, "Unknown or Unsupported chain kind: {}",
                         chain_key_type_reflector::to_fc_string(key_type));
   }

   const auto fetched = fetcher(ref);

   // Decision (settled at design review): a private key stored as a plaintext
   // `String` (or a `StringList`) is refused outright, not warned about. Keys
   // at rest must be KMS-encrypted, and boot is the cheapest moment to catch
   // the mistake. The message carries the exact remediation.
   SYS_ASSERT(fetched.type == parameter_type::SecureString, chain::plugin_config_exception,
              "SSM parameter \"{}\" has type {}; private keys must be stored as type SecureString "
              "(encrypted at rest with a KMS key). Re-create it with: aws ssm put-parameter "
              "--name '{}' --type SecureString --key-id <kms-key-id-or-alias> "
              "--value '<private-key>' --overwrite",
              ref.name, magic_enum::enum_name(fetched.type), ref.name);

   const auto value = trim_ascii_whitespace(fetched.value);
   SYS_ASSERT(!value.empty(), chain::plugin_config_exception,
              "SSM parameter \"{}\" is empty (after trimming whitespace)", ref.name);

   chain::private_key_type privkey;
   try {
      privkey = from_native_string_to_private_key(key_type, std::string{value});
   } catch (const fc::exception& e) {
      // Deliberately do NOT embed the parser's message or detail: low-level
      // parse errors can echo their input, and the input here is the secret
      // parameter value. The exception name alone is safe and still tells the
      // operator which parser stage rejected it.
      FC_THROW_EXCEPTION(chain::plugin_config_exception,
                         "SSM parameter \"{}\" does not contain a valid {} private key "
                         "(parse failed with {}; the value is not shown because it is a secret)",
                         ref.name, chain_key_type_reflector::to_fc_string(key_type), e.name());
   }

   // The plugin core verifies pinned-pubkey-vs-derived only for `KEY:`;
   // extension handlers own the check. Mismatch here means the parameter and
   // the spec's <public-key> disagree -- fail the boot with the fix, naming
   // no secret.
   SYS_ASSERT(expected_pub == privkey.get_public_key(), chain::plugin_config_exception,
              "SSM parameter \"{}\" holds a private key whose public key does not match the "
              "public key pinned in the signature-provider spec. Correct the spec's "
              "<public-key> to the key this parameter actually holds, or point the spec at "
              "the intended parameter.",
              ref.name);

   // Full `KEY:` parity from here on: local sign closure + populated
   // private_key (the Solana signing path requires the raw key; the Ethereum
   // path prefers it over the closure). No startup probe -- the eager fetch
   // above already proved credentials, IAM, the parameter, and the key.
   return {.signer = make_local_sign_fn(privkey), .private_key = privkey, .startup_probe = {}};
}

sysio::provider_spec_result create_ssm_provider(fc::crypto::chain_key_type_t  key_type,
                                                const fc::crypto::public_key& expected_pub,
                                                std::string_view              spec_data) {
   return create_ssm_provider_with_fetcher(key_type, expected_pub, spec_data, &fetch_ssm_parameter);
}

} // namespace sysio::sigprov::ssm
