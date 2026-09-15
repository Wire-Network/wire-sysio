# signature_provider_kms_plugin

`signature_provider_kms_plugin` adds the `KMS:<key-ref>` signature-provider scheme to
`signature_provider_manager_plugin`. Every signature is a remote AWS KMS `Sign` call, so the private key
never exists on the host or in the node's memory. Scope is secp256k1 / ethereum keys only — the provider
rejects any other key type at construction — and the per-signature round trip (typically 30–100 ms) rules
it out for block production, which the ethereum-only scoping already prevents structurally. It is opt-in:
the plugin is registered by `nodeop` but does nothing until an operator adds
`plugin = sysio::signature_provider_kms_plugin`.

The plugin carries the AWS SDK dependency, so `programs/nodeop/CMakeLists.txt` links it into `nodeop` alone
rather than into the shared chain-tool link set; `sys-util`, `cranker-example`, and the outpost RPC tools
stay AWS-free unless they opt in themselves.

## How it works

The plugin requires `signature_provider_manager_plugin` and does all of its work through it. The
constructor — not `plugin_initialize` — registers the `KMS` handler in the process-wide scheme registry,
which works because appbase constructs every registered plugin before it initializes any. Registering is
free: no AWS call, no network. `plugin_initialize`, `plugin_startup`, and `plugin_shutdown` are empty, and
the plugin starts no threads of its own.

The manager creates every configured `KMS:` provider during **its** `plugin_initialize`, but only when this
plugin is enabled; a config carrying a `KMS:` spec without the `plugin =` line fails the boot with an error
naming the exact line to add. Creation is offline — the spec is parsed, the key type and public-key variant
are checked, and a client is fetched from the shared per-region cache, all without touching AWS.

Each created provider attaches a **startup probe**, which the manager runs from its `plugin_startup`: a
single `GetPublicKey` call that resolves credentials, warms the client, and verifies the KMS key's public
key matches the one pinned in the spec. A permanent misconfiguration — missing credentials, wrong region,
absent IAM grant, mismatched key — aborts the boot loudly instead of surfacing on the first production
signature. A transient failure (throttle, timeout, `KMSInternal`) is logged and deferred to the lazy
first-sign check, so an AWS blip at restart can never block a boot. There is deliberately no flag to skip
the probe.

### Signing

Until a pin has succeeded once, each signature first runs the `GetPublicKey` check; the first success is
cached for the provider's lifetime, and a failed attempt is retried on the next probe or signature. A call
that passes pinning issues one `Sign` request with `SigningAlgorithm=ECDSA_SHA_256` and `MessageType=DIGEST`,
so KMS signs the 32 bytes as-is rather than re-hashing them. The DER signature KMS returns is then decoded to
a 64-byte `r || s` pair, normalised to low-S (EIP-2 / BIP-62 — KMS does not enforce it), and given a recovery
byte derived locally by trying both parities and matching against the pinned public key, producing the 65-byte
`r || s || (27 + recovery_id)` form the ethereum signing paths consume. If neither parity recovers the
expected key the call fails; once pinning has passed, that is a defence-in-depth check that should never fire.

### Spec grammar

```
--signature-provider <name>,ethereum,ethereum,<your-ethereum-public-key>,KMS:<key-ref>
```

`<key-ref>` is one of three forms:

| Form | Example | Region |
|---|---|---|
| Full ARN | `arn:aws:kms:us-east-1:<your-account-id>:alias/your-signing-key` | From the ARN's region segment; the whole ARN is handed to KMS unmodified. |
| `<region>:<key-id-or-alias>` | `us-east-1:alias/your-signing-key` | The leading token. |
| `<key-id-or-alias>` | `alias/your-signing-key` | Resolved from the environment (see below). |

The ARN tail must be `key/<id>` or `alias/<name>`, and the intact ARN is what reaches KMS: AWS accepts a
bare key id, a key ARN, an alias name, or an alias ARN as `KeyId`, but not a bare `key/<uuid>` tail — and
keeping the account id prevents an alias ARN from silently resolving to a same-named alias in the caller's
own account. Only the `arn:aws:kms:` partition and service are accepted; `aws-cn` and `aws-us-gov` are out
of scope, and a spec starting with `arn:` in any casing that does not match fails loudly rather than being
mistaken for the shorthand form. Because a KMS key id (a uuid, or `mrk-...`) and an alias name can never
contain a colon, a colon always means an explicit region: a colon-bearing spec whose leading token is not
shaped like an AWS region is rejected as malformed, and a bare token that *is* shaped like a region is
rejected as the likely "region with no key id" typo.

### Region resolution

When the spec omits its region, the region is resolved when the provider is created, through the same chain
the AWS SDK's own client configuration uses: `AWS_DEFAULT_REGION`, then `AWS_REGION`, then the shared-config
profile's `region`, then the EC2 instance-metadata service (skipped when `AWS_EC2_METADATA_DISABLED=true`).
On AWS compute a region-less spec therefore works with no configuration at all. Two deliberate differences
from stock SDK behavior: an explicit spec region always wins over the environment, and resolution never
silently falls back to a default region — when nothing resolves the boot fails with a message naming every
source that was checked.

Clients are cached per resolved region and shared with `signature_provider_ssm_plugin` through the common
`signature_provider_aws` library, which also owns the single process-wide `Aws::InitAPI` / `Aws::ShutdownAPI`
pair. Multiple specs in one region share one client; the SDK's HTTP pool is thread-safe, so closures may
submit concurrently. Constructing a client is offline; credentials resolve on the first API call through the
standard AWS provider chain.

## Enabling / configuration

### `config.ini`

```ini
plugin = sysio::signature_provider_kms_plugin

# Explicit region, alias form.
signature-provider = eth-01,ethereum,ethereum,<your-ethereum-public-key>,KMS:us-east-1:alias/your-signing-key

# Region-less: region resolves from the environment / IMDS.
signature-provider = eth-01,ethereum,ethereum,<your-ethereum-public-key>,KMS:alias/your-signing-key

# Full ARN.
signature-provider = eth-01,ethereum,ethereum,<your-ethereum-public-key>,KMS:arn:aws:kms:us-east-1:<your-account-id>:alias/your-signing-key
```

### Command line

```bash
nodeop --plugin sysio::signature_provider_kms_plugin \
       --signature-provider 'eth-01,ethereum,ethereum,<your-ethereum-public-key>,KMS:us-east-1:alias/your-signing-key'
```

### One-time key setup

The key must be created with the secp256k1 key spec and signing usage:

```bash
aws kms create-key \
    --region us-east-1 \
    --key-spec ECC_SECG_P256K1 \
    --key-usage SIGN_VERIFY \
    --description "wire signing key"

aws kms create-alias \
    --region us-east-1 \
    --alias-name alias/your-signing-key \
    --target-key-id <the-KeyId-from-the-response>
```

The `<public-key>` field of the spec is the key's uncompressed `04 || X || Y` point (130 hex characters),
read out of the DER `SubjectPublicKeyInfo` that `aws kms get-public-key` returns. The key parser also
accepts the 64-byte raw `X || Y` form and the 33-byte compressed form, with an optional `0x` prefix.

## Options

The plugin registers no program options of its own — `set_program_options` is empty. It is configured
entirely through `plugin = sysio::signature_provider_kms_plugin` plus the manager's `signature-provider`
option, and through the standard AWS environment (`AWS_DEFAULT_REGION`, `AWS_REGION`,
`AWS_EC2_METADATA_DISABLED`, the shared-config profile, and whatever the credential chain reads).

## Diagnostics

Neither the plugin nor the provider emits a log line of its own: the plugin's lifecycle hooks are empty and
the signing path reports through exceptions. What an operator sees at boot comes from the manager — its
info-level `Running signature-provider startup probes for <n> signing key(s)` line, then either
`Signature-provider startup probes passed` or the warning
`Signature-provider startup probe: transient error for one key, deferring its check to the first sign: <detail>`
followed by the deferred-count variant of the passed line.

Failures split two ways, matching the AWS SDK's own retryability classification:

| Class | Thrown as | Typical cause |
|---|---|---|
| Permanent | `plugin_config_exception` | Access denied, key not found, disabled key, invalid key state, bad parameters, a malformed spec, a non-ethereum key type, a public key that does not match the KMS key, an unresolvable region. |
| Transient | `signing_transient_exception` | Throttling, `KMSInternal`, dependency or network timeouts, service-unavailable. |

The two types are siblings rather than parent and child, so a handler catching only the permanent type
cannot silently swallow a retryable error. Every message names the service, the operation (`Sign` or
`GetPublicKey`), the key, the AWS error type and HTTP status, the exception name, and whether the failure
was classified transient or permanent. At startup a permanent failure aborts the boot; a transient one is
deferred. At runtime a transient failure is safe to retry with backoff, and a permanent one means the
configuration must be fixed.

### Credentials and IAM

Credentials resolve through the standard AWS provider chain — environment variables, `~/.aws/`, IRSA, IMDS,
or SSO — on the first API call. The node's identity needs two grants on the key: `kms:Sign` and
`kms:GetPublicKey`. `kms:GetPublicKey` is a runtime requirement, not just a setup convenience: it backs the
startup probe and the pre-sign pinning check, and without it both fail with `AccessDeniedException`. The
AWS SDK's internal logger is left disabled, so a credential-chain or retry problem shows up as the thrown
error rather than as SDK log output.

## Tests

```bash
ninja -C build/debug test_signature_provider_kms_plugin
./build/debug/plugins/signature_provider_kms_plugin/test/test_signature_provider_kms_plugin
```

Everything in the default run is offline. The suite covers the spec parser (all three forms, multi-region
keys, and every rejected shape — bare region, non-region lead before a colon, missing or bad ARN tail,
empty region or account, wrong or mis-cased partition and service, too few segments), DER-to-compact
conversion against known-answer vectors, low-S normalisation, v recovery, the end-to-end DER-to-ethereum
signature path checked against a local sign, `SubjectPublicKeyInfo` decoding (including wrong-curve,
truncated, off-curve, wrong-algorithm, trailing-byte, and nonzero-unused-bit rejections), pinning against a
mismatched key, and the plugin's construction-time handler registration plus the probe actually running at
the manager's startup.

One case reaches AWS and is skipped cleanly when its environment variables are unset:

```bash
export KMS_LIVE_SPEC='us-east-1:alias/your-signing-key'   # or the full ARN form
export KMS_LIVE_PUBKEY='<your-ethereum-public-key-hex>'

./build/debug/plugins/signature_provider_kms_plugin/test/test_signature_provider_kms_plugin \
    --run_test=kms_signature_provider_tests/kms_live_sign_round_trip \
    --log_level=test_suite
```

It signs one fixed deterministic digest, converts the result to the 65-byte ethereum form, and recovers the
public key locally (no `kms:Verify` call) to check it matches. A run is a single `Sign` call. `test/README.md`
in this directory carries the full runbook, including the cost note and a table of common failure modes.

## Related plugins

- [`signature_provider_manager_plugin`](../signature_provider_manager_plugin/README.md) — required by this
  plugin; owns the spec grammar, creates the providers, and runs the startup probes.
- [`signature_provider_ssm_plugin`](../signature_provider_ssm_plugin/README.md) — the sibling AWS scheme;
  fetches the key once and signs locally, so it works for every key type including producer block signing.
- `libraries/signature_provider_aws` — the shared AWS glue: SDK lifecycle, per-region client cache, region
  resolution, and the transient-versus-permanent error split.
- `outpost_ethereum_client_plugin` — the ethereum-side submission path a `KMS:` key typically signs for.
- `docs/signature-provider-manager-plugin.md` — the spec reference shared by every scheme.
