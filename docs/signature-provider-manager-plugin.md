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
        `<key-ref>`        for `KMS`: an ARN or `<region>:<key-id-or-alias>` naming the AWS KMS key that
                           signs remotely (see plugins/signature_provider_kms_plugin/test/README.md)
        `<param-ref>`      for `SSM`: an ARN or `<region>:<parameter-name>` naming the AWS SSM Parameter
                           Store SecureString that holds the private key (see below)
```

A spec whose scheme's plugin is not enabled fails the boot with an error naming the exact
`plugin =` line to add. Host applications that embed the manager without the plugins can still
register a handler from `main()` before `app().initialize(...)` via `register_spec_handler()`.

## KMS: AWS KMS remote signing (`plugin = sysio::signature_provider_kms_plugin`)

`KMS:<key-ref>` keeps the signing key in AWS KMS and issues a remote `Sign` call per signature —
the key never appears on the host or in process memory. `<key-ref>` is a full key/alias ARN or
`<region>:<key-id-or-alias>`. Scope is secp256k1/ethereum keys only — the provider hard-rejects
every other key type at boot, so a `KMS:` key can never back wire producer or BLS finalizer
signing; the 30–100 ms per-signature latency therefore only touches ethereum-side submission
paths, which run at seconds cadence. The plugin also owns the
`signature-provider-kms-startup-check` option: when set, every `KMS:` key is probed at startup
with a (free) `GetPublicKey` call so a credentials / region / IAM / pinned-key misconfiguration
fails at boot instead of on the first sign. See
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

`<param-ref>` is either the shorthand `<region>:<parameter-name>` (everything after the first
colon passes to GetParameter verbatim, so SSM's native `:version` / `:label` selectors work) or a
full `arn:aws:ssm:<region>:<account>:parameter/<path>` ARN. The region is mandatory -- there is
no `AWS_REGION` fallback.

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

## Config migration

- `SSM:` specs previously worked in stock nodeop with no extra flags; they now require
  `plugin = sysio::signature_provider_ssm_plugin` in the config (or `--plugin` on the command
  line). A config with an `SSM:`/`KMS:` spec and no matching plugin line fails the boot with an
  error naming the exact line to add.
- `signature-provider-kms-startup-check` moved from the manager to
  `signature_provider_kms_plugin`. nodeop still parses it either way (appbase collects options
  from every linked plugin), but it only takes effect when the kms plugin is enabled.


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