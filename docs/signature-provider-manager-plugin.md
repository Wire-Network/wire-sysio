# Signature Provider Manager Plugin

This plugin is the result of refactoring the
existing `signature_provider_plugin`.

The goal of the refactoring it to support multiple 
signature providers targeting different chains,
with varied key types.

## Provider Spec

```
<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>

   `<name>`                 name to use when referencing this provider, if empty then auto-assigned
   `<chain-kind>`           chain where the key will be used
   `<key-type>`             key format to parse
   `<public-key>`           is a string form of a valid <key-type>
   `<private-key-provider-spec>`   is a string in the form <key-provider-type>:<data>
       
```
## Key Provider Spec

```
<key-provider-type>:<data>
   `<key-provider-type>`   KEY and KIOD are built in; additional schemes (KMS, SSM) are
                           provided by optional signature-provider plugins, enabled per
                           config with `plugin = ...`
   `<data>`                is provided to the key provider based on the type
        `<private-key>`    string representation of a key in the format of the key type for `KEY` provider type
        `<url>`            is the URL where kiod is available and the appropriate wallet(s) for `KIOD` provider type
        `<key-ref>`        for `KMS`: an ARN or `[<region>:]<key-id-or-alias>` naming the AWS KMS key that
                           signs remotely (see plugins/signature_provider_kms_plugin/test/README.md)
        `<param-ref>`      for `SSM`: an ARN or `[<region>:]<parameter-name>` naming the AWS SSM Parameter
                           Store SecureString that holds the private key (see below)
```

A spec whose scheme's plugin is not enabled fails the boot with an error naming the exact
`plugin =` line to add. Host applications that embed the manager without the plugins can still
register a handler from `main()` before `app().initialize(...)` via `register_spec_handler()`.

## KMS: AWS KMS remote signing (`plugin = sysio::signature_provider_kms_plugin`)

`KMS:<key-ref>` keeps the signing key in AWS KMS and issues a remote `Sign` call per signature —
the key never appears on the host or in process memory. `<key-ref>` is a full key/alias ARN,
`<region>:<key-id-or-alias>`, or a region-less `<key-id-or-alias>` (see "Region resolution"
below). Scope is secp256k1/ethereum keys only — the provider hard-rejects
every other key type at boot, so a `KMS:` key can never back wire producer or BLS finalizer
signing; the 30–100 ms per-signature latency therefore only touches ethereum-side submission
paths, which run at seconds cadence. Every `KMS:` key is probed at startup with a (free)
`GetPublicKey` call, so a credentials / region / IAM / pinned-key misconfiguration fails at boot
instead of on the first sign; a transient AWS error at startup is logged and deferred to the
lazy first-sign check, so an AWS blip never blocks a boot. See
`plugins/signature_provider_kms_plugin/test/README.md` for key setup, IAM requirements, and
operational notes.

## SSM: AWS SSM Parameter Store keys (`plugin = sysio::signature_provider_ssm_plugin`)

`SSM:<param-ref>` fetches the private key from AWS SSM Parameter Store exactly once, when the
provider is created at startup, and signs locally thereafter -- semantically `KEY:` without the
key material ever appearing in config files, command lines, process listings, or shell history.
It works for every key type with a `KEY:` form (the parameter's value is exactly the string that
would follow `KEY:`), and local signing makes it suitable for all signing paths including
producer block signing -- unlike `KMS:`, whose per-signature network round-trip is too slow for
block production (and whose ethereum-only key scope rules that out anyway).

`<param-ref>` is the shorthand `<region>:<parameter-name>` (everything after the first colon
passes to GetParameter verbatim, so SSM's native `:version` / `:label` selectors work), a
region-less `<parameter-name>` (see "Region resolution" below), or a full
`arn:aws:ssm:<region>:<account>:parameter/<path>` ARN. The two shorthand forms are told apart by
the leading token: one shaped like an AWS region (`us-east-1`, `eu-west-2`, ...) means the
explicit-region form; anything else makes the whole body the parameter name, selector colons
included. The one ambiguous corner is a region-less *selector* reference to a parameter whose own
name is shaped like a region (`SSM:my-param-2:label` parses as region `my-param-2`, name
`label`) -- address such a parameter with an explicit region or the ARN form.

Requirements and failure behavior:

- The parameter must be of type `SecureString` (KMS-encrypted at rest); plain `String` /
  `StringList` parameters are refused at boot with the `put-parameter --type SecureString`
  remediation in the error.
- The caller's IAM identity needs `ssm:GetParameter` on the parameter and `kms:Decrypt` on the
  KMS key that encrypts it. Credentials resolve through the standard AWS provider chain (env,
  `~/.aws/`, IRSA, EC2 instance role).
- The derived public key must match the spec's `<public-key>`; any mismatch, fetch failure, or
  misconfiguration fails the boot with a precise error. A running node never re-fetches --
  rotating the parameter to a new keypair requires updating the spec's `<public-key>` and
  restarting.

```
# Store the key (one-time):
#   aws ssm put-parameter --region us-east-1 --name /wire/prod/bp1 \
#       --type SecureString --key-id alias/wire-signing-keys --value '5J5Lz...'
# config.ini:
plugin = sysio::signature_provider_ssm_plugin
signature-provider = bp1,wire,wire,SYS7AzqPxqfoEigXBefEo6efsCZszLzwv4vCdWqTt6s6zSnDELSmm,SSM:us-east-1:/wire/prod/bp1
```

See `plugins/signature_provider_ssm_plugin/test/README.md` for the full operator
runbook (IAM policy, rotation, live-test setup).

## Region resolution (`KMS:` and `SSM:`)

A spec may name its region explicitly (`<region>:` prefix, or embedded in an ARN) or omit it.
When omitted, the region resolves at provider creation through the same chain the AWS SDK itself
uses: `AWS_DEFAULT_REGION`, then `AWS_REGION`, then the shared-config profile's `region`
(`~/.aws/config`), then the EC2 instance-metadata service — any process on AWS compute can learn
its own instance's region from IMDS, so on EC2/ECS a region-less spec works with zero
configuration. Two deliberate differences from stock SDK behavior: resolution never silently
falls back to `us-east-1` (an unresolvable region fails the boot with a precise error instead of
signing against a region the operator didn't choose), and an explicit spec region always wins
over the environment. IMDS is skipped when `AWS_EC2_METADATA_DISABLED=true`.

## Config migration

- `SSM:` specs previously worked in stock nodeop with no extra flags; they now require
  `plugin = sysio::signature_provider_ssm_plugin` in the config (or `--plugin` on the command
  line). A config with an `SSM:`/`KMS:` spec and no matching plugin line fails the boot with an
  error naming the exact line to add.
- `signature-provider-kms-startup-check` was removed: every `KMS:` key is now probed at startup
  unconditionally (transient AWS errors defer to the first sign, so boots never block on a
  blip). A config still setting the option fails option parsing — delete the line.


## Examples

### WIRE

```
# Private key: 5J5LzjfChtY3LGhkxaRoAaSjKHtgNZqKyaJaw5boxuY9LNv4e1U
# Public key: SYS7AzqPxqfoEigXBefEo6efsCZszLzwv4vCdWqTt6s6zSnDELSmm
wire-01,wire,wire,SYS7AzqPxqfoEigXBefEo6efsCZszLzwv4vCdWqTt6s6zSnDELSmm,KEY:5J5LzjfChtY3LGhkxaRoAaSjKHtgNZqKyaJaw5boxuY9LNv4e1U
```

### Ethereum

```
# PRIVATE_KEY: 0x8f2cdaeb8e036865421c79d4cc42c7704af5cef0f592b2e5c993e2ba7d328248
# PUBLIC_KEY: 0xfc5422471c9e31a6cd6632a2858eeaab39f9a7eec5f48eedecf53b8398521af1c86c9fce17312900cbb11e2e2ec1fb706598065f855c2f8f2067e1fbc1ba54c8
eth-01,ethereum,ethereum,0xfc5422471c9e31a6cd6632a2858eeaab39f9a7eec5f48eedecf53b8398521af1c86c9fce17312900cbb11e2e2ec1fb706598065f855c2f8f2067e1fbc1ba54c8,KEY:0x8f2cdaeb8e036865421c79d4cc42c7704af5cef0f592b2e5c993e2ba7d328248
```