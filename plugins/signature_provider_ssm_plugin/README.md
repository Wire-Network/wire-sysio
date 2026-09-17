# signature_provider_ssm_plugin

`signature_provider_ssm_plugin` adds the `SSM:<param-ref>` signature-provider scheme to
`signature_provider_manager_plugin`. The private key is fetched exactly once from AWS SSM Parameter Store
— a KMS-encrypted `SecureString` — while the node initializes, and every signature after that is produced
locally. Semantically this is `KEY:` with the key material kept out of config files, command lines, process
listings, and shell history, and because signing stays local it is fast enough for every signing path,
including producer block signing. It is opt-in: the plugin is registered by `nodeop` but does nothing until
an operator adds `plugin = sysio::signature_provider_ssm_plugin`.

The plugin carries the AWS SDK dependency, so `programs/nodeop/CMakeLists.txt` links it into `nodeop`
alone rather than into the shared chain-tool link set; `sys-util`, `cranker-example`, and the outpost RPC
tools stay AWS-free unless they opt in themselves.

## How it works

The plugin requires `signature_provider_manager_plugin` and does all of its work through it. The
constructor — not `plugin_initialize` — registers the `SSM` handler in the process-wide scheme registry,
which works because appbase constructs every registered plugin before it initializes any. Registering is
free: no AWS call, no network. `plugin_initialize`, `plugin_startup`, and `plugin_shutdown` are empty, and
the plugin starts no threads of its own.

The manager then creates every configured `SSM:` provider during **its** `plugin_initialize` — before any
consumer plugin initializes — but only when this plugin is enabled. A config carrying an `SSM:` spec
without the `plugin =` line fails the boot with an error naming the exact line to add.

### Spec grammar

```
--signature-provider <name>,<chain-kind>,<key-type>,<public-key>,SSM:<param-ref>
```

`<param-ref>` is one of three forms:

| Form | Example | Region |
|---|---|---|
| Full ARN | `arn:aws:ssm:us-east-1:<your-account-id>:parameter/your/ssm/parameter` | From the ARN's region segment; the whole ARN is handed to `GetParameter` unmodified, so the account id is preserved. |
| `<region>:<parameter-name>` | `us-east-1:/your/ssm/parameter` | The leading token. Everything after the first colon goes to `GetParameter` verbatim. |
| `<parameter-name>` | `/your/ssm/parameter` | Resolved from the environment (see below). |

Everything after the region is passed through untouched, so SSM's own `:version` and `:label` selectors
work (`us-east-1:/your/ssm/parameter:3` selects version 3). The two shorthand forms are told apart by the
leading token: one shaped like an AWS region means the explicit-region form, anything else makes the whole
body a region-less parameter name, selector colons included. Path-style names never collide, because a
region cannot contain `/`. The one ambiguous corner is a region-less *selector* reference to a parameter
whose own name is shaped like a region — address such a parameter with an explicit region or the ARN form.
A bare token that is itself shaped like a region is rejected as the likely "region with no parameter name"
typo, and only the `arn:aws:ssm:` partition and service are accepted: `aws-cn` and `aws-us-gov` are out of
scope, and a spec starting with `arn:` in any casing that does not match fails loudly rather than being
mistaken for the shorthand form.

### Region resolution

When the spec omits its region, the region is resolved when the provider is created, through the same chain
the AWS SDK's own client configuration uses: `AWS_DEFAULT_REGION`, then `AWS_REGION`, then the shared-config
profile's `region`, then the EC2 instance-metadata service (skipped when `AWS_EC2_METADATA_DISABLED=true`).
On AWS compute a region-less spec therefore works with no configuration at all. Two deliberate differences
from stock SDK behavior: an explicit spec region always wins over the environment, and resolution never
silently falls back to a default region — when nothing resolves the boot fails with a message naming every
source that was checked.

Clients are cached per resolved region and shared with `signature_provider_kms_plugin` through the common
`signature_provider_aws` library, which also owns the single process-wide `Aws::InitAPI` / `Aws::ShutdownAPI`
pair. Constructing a client is offline; credentials resolve on the first API call through the standard AWS
provider chain (environment, `~/.aws/`, IRSA, EC2 instance role).

### The fetch, step by step

Provider creation runs this pipeline, and every failure throws so a misconfigured signer fails the boot
rather than the first signature:

1. Parse the spec body into `(region, name)`.
2. Reject a chain key type with no `KEY:`-style native form (`sui` is not implemented; an unrecognised value
   is a config parse error).
3. Issue `GetParameter` with `WithDecryption=true` on the shared regional client, bounded by the SDK's connect
   and request timeouts and its default retry strategy. For a spec that names its region this is the only
   network round-trip; a region-less spec may make one more first, because resolving the default region falls
   back to the EC2 instance-metadata service when neither `AWS_DEFAULT_REGION` / `AWS_REGION` nor the
   shared-config profile supplies one (and `AWS_EC2_METADATA_DISABLED` is not `true`).
4. Require the parameter's type to be `SecureString`. A `String` or `StringList` is refused outright, with
   the `put-parameter --type SecureString` remediation in the message.
5. Trim surrounding ASCII whitespace — a trailing newline from `put-parameter --value "$(cat key.txt)"`
   must not brick a producer — and reject an empty value.
6. Parse the value as a private key of the spec's `<key-type>`. A parse failure is re-thrown naming the
   parameter but deliberately **not** carrying the underlying parser message, which could echo the secret.
7. Verify the derived public key equals the `<public-key>` pinned in the spec.

The parameter's value is exactly the string that would follow `KEY:` — WIF / `PVT_...` for `wire`,
`PVT_BLS_...` for `wire_bls`, `0x...` hex for `ethereum`, base58 for `solana`. The result is a local-key
signer with the private key populated, so every downstream consumer sees full `KEY:` parity, including the
Solana path that needs a raw local key. No startup probe is attached — after the eager fetch there is
nothing left to check.

The fetch happens once. A running node never re-reads the parameter, and because the spec pins the public
key, rotating the parameter to a new key pair inherently means editing the spec's `<public-key>` and
restarting.

## Enabling / configuration

### `config.ini`

```ini
plugin = sysio::signature_provider_ssm_plugin

# Explicit region.
signature-provider = bp1,wire,wire,<your-wire-public-key>,SSM:us-east-1:/your/ssm/parameter

# Region-less: region resolves from the environment / IMDS.
signature-provider = bp1,wire,wire,<your-wire-public-key>,SSM:/your/ssm/parameter

# Full ARN.
signature-provider = bp1,wire,wire,<your-wire-public-key>,SSM:arn:aws:ssm:us-east-1:<your-account-id>:parameter/your/ssm/parameter
```

### Command line

```bash
nodeop --plugin sysio::signature_provider_ssm_plugin \
       --signature-provider 'bp1,wire,wire,<your-wire-public-key>,SSM:us-east-1:/your/ssm/parameter'
```

### One-time parameter setup

```bash
# Store the private key as a SecureString; --key-id selects the KMS key that encrypts it.
aws ssm put-parameter \
    --region us-east-1 \
    --name /your/ssm/parameter \
    --type SecureString \
    --key-id alias/your-signing-keys \
    --value '<your-wire-private-key>'
```

Add `--overwrite` to replace an existing parameter. The `<public-key>` field of the spec is the public key
matching the value stored here.

## Options

The plugin registers no program options of its own — `set_program_options` is empty. It is configured
entirely through `plugin = sysio::signature_provider_ssm_plugin` plus the manager's `signature-provider`
option, and through the standard AWS environment (`AWS_DEFAULT_REGION`, `AWS_REGION`,
`AWS_EC2_METADATA_DISABLED`, the shared-config profile, and whatever the credential chain reads).

## Diagnostics

Neither the plugin nor the provider emits a log line: the plugin's lifecycle hooks are empty and the fetch
path reports through exceptions. Everything an operator sees comes from the manager — its debug-level
`Registered signature provider (<name>): <public-key>`, the only per-spec line it writes — or from a
boot-time failure.

Failures split two ways, matching the AWS SDK's own retryability classification:

| Class | Thrown as | Typical cause |
|---|---|---|
| Permanent | `plugin_config_exception` | `ParameterNotFound`, access denied, KMS decrypt denied, a non-`SecureString` parameter, an empty or unparseable value, a pinned-public-key mismatch, a malformed spec, an unresolvable region. |
| Transient | `signing_transient_exception` | Throttling, service-internal errors, timeouts that outlasted the SDK's own retries. |

Either class fails the boot — there is no partially-configured signer. A permanent failure additionally
carries the remediation: verify the caller's IAM identity is granted `ssm:GetParameter` on the parameter and
`kms:Decrypt` on the KMS key that encrypts it, and that the region and parameter name are right. Every
message names the service, the operation, the parameter, the AWS error type and HTTP status, and whether the
failure was classified transient or permanent.

### Credentials and IAM

Credentials resolve through the standard AWS provider chain — environment variables, `~/.aws/`, IRSA, or the
EC2 instance role — on the first API call. The node's identity needs exactly two grants: `ssm:GetParameter`
on the parameter, and `kms:Decrypt` on the KMS key that encrypts it. Every decrypt is recorded in CloudTrail
as a KMS event, so key reads are audited.

Threat model: after the fetch the key lives in process memory, exactly as a `KEY:` provider's would. `SSM:`
removes secrets from config files and disk and puts reads behind IAM and CloudTrail; it does not give the
never-leaves-AWS isolation of `KMS:`.

## Tests

```bash
ninja -C build/debug test_signature_provider_ssm_plugin
./build/debug/plugins/signature_provider_ssm_plugin/test/test_signature_provider_ssm_plugin
```

Everything in the default run is offline. The suite covers the spec parser (all three forms, selector
pass-through, region-shaped-lead tiebreak, ARN partition/service rejection, every malformed shape), the
construction pipeline against an injected fetcher (`wire`, `wire_bls`, `ethereum`, and `solana` round
trips, whitespace trimming, the `String` / `StringList` / untyped / empty-value rejections, the guarantee
that a parse failure does not echo the value, pinned-public-key mismatch, and transient-failure
propagation), the error classification, the per-region client cache, and the plugin's construction-time
handler registration and routing through the manager — including the error that names this plugin when the
scheme is used without enabling it.

One case reaches AWS and is skipped cleanly when its environment variables are unset:

```bash
export SSM_LIVE_SPEC='us-east-1:/your/ssm/parameter'   # or the full parameter ARN
export SSM_LIVE_PUBKEY='<your-public-key>'             # native-form public key
export SSM_LIVE_KEY_TYPE='wire'                        # optional; default wire

./build/debug/plugins/signature_provider_ssm_plugin/test/test_signature_provider_ssm_plugin \
    --run_test=ssm_live
```

It needs AWS credentials with the two grants above. `GetParameter` is free; the KMS decrypt under it is
billed at standard KMS request rates. `test/README.md` in this directory carries the full runbook.

## Related plugins

- [`signature_provider_manager_plugin`](../signature_provider_manager_plugin/README.md) — required by this
  plugin; owns the spec grammar and creates the providers.
- [`signature_provider_kms_plugin`](../signature_provider_kms_plugin/README.md) — the sibling AWS scheme;
  keeps the key inside AWS at the cost of a network round-trip per signature, and covers ethereum keys only.
- `libraries/signature_provider_aws` — the shared AWS glue: SDK lifecycle, per-region client cache, region
  resolution, and the transient-versus-permanent error split.
- `docs/signature-provider-manager-plugin.md` — the spec reference shared by every scheme.
