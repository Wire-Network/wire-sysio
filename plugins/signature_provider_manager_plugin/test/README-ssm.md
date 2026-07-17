# SSM signature provider — live test & operator runbook

The `SSM:` signature-provider scheme fetches a private key from AWS SSM
Parameter Store (a KMS-encrypted `SecureString` parameter) once at provider
creation, then signs locally. It is semantically `KEY:` without the key
material in config files, command lines, or shell history, and it works for
every chain key type that has a `KEY:` form (`wire`, `wire_bls`, `ethereum`,
`solana`). Because signing is local after the one startup fetch, `SSM:` is
suitable for all signing paths, including producer block signing.

Everything in `test_ssm_signature_provider.cpp` and
`test_ssm_plugin_routing.cpp` runs offline. The single live case
(`ssm_live/ssm_live_fetch_round_trip`) is env-gated and skips cleanly when the
variables below are unset.

## Spec format

```
--signature-provider <name>,<chain>,<key-type>,<public-key>,SSM:<param-ref>
```

`<param-ref>` is either:

- shorthand `<region>:<parameter-name>`, e.g. `us-east-1:/wire/prod/bp1`.
  Everything after the first `:` is passed to `GetParameter` verbatim, so
  SSM's native version / label selectors work: `us-east-1:/wire/prod/bp1:3`.
- a full parameter ARN, e.g.
  `arn:aws:ssm:us-east-1:111122223333:parameter/wire/prod/bp1`.

The region is mandatory (no `AWS_REGION` fallback), matching the `KMS:`
scheme. The parameter's **value** is exactly the string that would follow
`KEY:` — WIF / `PVT_...` for wire, `PVT_BLS_...`, `0x...` hex for ethereum,
base58 for solana. Leading/trailing whitespace (a trailing newline from
`put-parameter --value "$(cat key.txt)"`) is trimmed.

nodeop registers the scheme; nothing AWS runs unless a spec uses `SSM:`.

## One-time parameter setup

```bash
# 1. Generate a key pair (example: a wire K1 key).
clio create key --to-console
#   Private key: 5K...        <- goes into the parameter
#   Public key:  SYS...       <- goes into the spec's <public-key> field

# 2. Store the private key as a SecureString. --key-id selects the KMS key
#    that encrypts it (omit for the account default alias/aws/ssm).
aws ssm put-parameter \
    --region us-east-1 \
    --name /wire/prod/bp1 \
    --type SecureString \
    --key-id alias/wire-signing-keys \
    --value '5K...'

# 3. Reference it from the nodeop config:
#    signature-provider = bp1,wire,wire,SYS...,SSM:us-east-1:/wire/prod/bp1
```

The provider **refuses** parameters of type `String` / `StringList`: private
keys at rest must be KMS-encrypted. If you get that error, re-create the
parameter with `--type SecureString` as above (add `--overwrite` to replace).

## IAM requirements

The node's AWS identity (env credentials, `~/.aws/`, IRSA, or the EC2
instance role) needs exactly two grants:

```json
{
   "Version": "2012-10-17",
   "Statement": [
      {
         "Effect": "Allow",
         "Action": "ssm:GetParameter",
         "Resource": "arn:aws:ssm:us-east-1:111122223333:parameter/wire/prod/bp1"
      },
      {
         "Effect": "Allow",
         "Action": "kms:Decrypt",
         "Resource": "arn:aws:kms:us-east-1:111122223333:key/<key-id-encrypting-the-parameter>"
      }
   ]
}
```

Every decrypt lands in CloudTrail as a KMS event, so key reads are audited.

## Operational notes

- **Fetch-once semantics.** The key is fetched during `plugin_initialize`.
  Any failure — bad region/name, missing IAM grant, non-SecureString type,
  value/pubkey mismatch, or an AWS outage that outlasts the SDK's retries —
  fails the boot with a precise error. A running node never re-fetches.
- **Rotation.** The spec pins the public key, so rotating the parameter to a
  new keypair requires updating the spec's `<public-key>` and restarting.
  Rotate by writing the new key (`put-parameter --overwrite`), updating the
  config, then restarting the node.
- **Threat model.** After the fetch the key lives in process memory, exactly
  like `KEY:`. `SSM:` removes secrets from config files and disk and gates
  reads behind IAM + CloudTrail; it does not provide KMS-style
  never-leaves-AWS isolation. Use `KMS:` where that isolation matters and the
  30–100 ms per-signature latency is acceptable (not block production).

## Live test

```bash
export SSM_LIVE_SPEC='us-east-1:/wire/ci/test-key'   # or the full parameter ARN
export SSM_LIVE_PUBKEY='SYS...'                       # native-form public key
export SSM_LIVE_KEY_TYPE='wire'                       # optional; default wire

$BUILD_DIR/plugins/signature_provider_manager_plugin/test/test_sigprov_ssm \
    --run_test=ssm_live
```

Requires AWS credentials with the IAM grants above. `GetParameter` is free;
the KMS decrypt under it is billed at standard KMS request rates.
