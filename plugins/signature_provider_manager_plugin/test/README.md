# `test_signature_provider_manager_plugin` — running the live AWS KMS round-trip

This directory hosts the Boost.Test suite for the signature-provider plugin,
including the AWS KMS provider tests. Most of those cases are offline — they
exercise the spec parser, DER → raw conversion, low-S normalisation, and
v-recovery using locally generated secp256k1 keys, so they need no AWS
credentials and no network.

The one case that does touch AWS is
`kms_signature_provider_tests/kms_live_sign_round_trip`. This README is the
minimum runbook to make it pass.

## What the live case actually verifies

1. Construct a real `make_kms_signature_provider` closure for the supplied
   `KMS:<spec>`.
2. Issue a real `KMS::Sign` call against a fixed deterministic digest
   (`keccak256("wire-sysio kms live test 2026")`), `MessageType=DIGEST`,
   `SigningAlgorithm=ECDSA_SHA_256`.
3. Convert the returned DER signature to the 65-byte Ethereum compact form,
   recovering `v` locally by trial-recovery against the pubkey you supplied.
4. Recover the public key from the resulting signature **locally** (no
   `kms:Verify` call) and `BOOST_CHECK` that it matches `KMS_LIVE_PUBKEY`
   byte-for-byte.

So "KMS is working" here means: KMS returned a valid ECDSA signature over
your digest, and the key it signed with matches the public key you pinned.

If `KMS_LIVE_SPEC` or `KMS_LIVE_PUBKEY` is unset or empty, the case logs
`skipping live KMS test` and exits success — that is how default CI stays
free of network calls and KMS charges.

## Prerequisites

### 1. KMS key

A KMS key in your AWS account with:

- **Key spec:** `ECC_SECG_P256K1` (secp256k1 — the curve Ethereum uses)
- **Key usage:** `SIGN_VERIFY`

```bash
aws kms create-key \
   --region us-east-1 \
   --key-spec ECC_SECG_P256K1 \
   --key-usage SIGN_VERIFY \
   --description "wire-sysio KMS live signing test"
# capture the KeyId from the response, then optionally alias it:
aws kms create-alias \
   --region us-east-1 \
   --alias-name alias/wire-ci-test-secp256k1 \
   --target-key-id <KeyId>
```

### 2. IAM permission

The principal whose credentials the test runs under needs `kms:Sign` on the
key. `kms:GetPublicKey` is also granted so the same principal can extract
the matching pubkey hex (Prereq #4) without juggling a second role; the
plugin itself does not call `GetPublicKey` (the expected pubkey is supplied
by the caller). Minimum inline policy:

```json
{
   "Version": "2012-10-17",
   "Statement": [{
      "Effect": "Allow",
      "Action": [
         "kms:Sign",
         "kms:GetPublicKey"
      ],
      "Resource": "arn:aws:kms:us-east-1:<account-id>:key/<KeyId>"
   }]
}
```

### 3. AWS credentials in the runner's environment

Anything the standard AWS credential chain accepts works: `AWS_ACCESS_KEY_ID`
+ `AWS_SECRET_ACCESS_KEY` (+ `AWS_SESSION_TOKEN` if you assumed a role),
`~/.aws/credentials`, IRSA, IMDS, SSO. Confirm with:

```bash
aws sts get-caller-identity
```

### 4. The matching public key in hex

Pull the SubjectPublicKeyInfo from KMS and extract the uncompressed
`04 || X || Y` (65 bytes, 130 hex chars):

```bash
aws kms get-public-key \
   --region us-east-1 \
   --key-id alias/wire-ci-test-secp256k1 \
   --output text \
   --query PublicKey \
| base64 -d \
| openssl ec -pubin -inform DER -text -noout 2>/dev/null \
| awk '/^pub:/{flag=1;next}/ASN1 OID/{flag=0}flag' \
| tr -d ' :\n'
```

That prints e.g. `045a87…eef3`. The fc parser also accepts the 64-byte raw
`X || Y` form (no `04` prefix) and the 33-byte compressed form (`02`/`03`
prefix); leading `0x` is optional.

## Build

The live case lives in the same binary as the parser tests. Build only what
you need (substitute your own `$BUILD_DIR`, e.g. `build/claude`):

```bash
ninja -C $BUILD_DIR test_signature_provider_manager_plugin
```

## Run

Export the two env vars and the AWS credentials, then invoke the binary with
a `--run_test` filter so you only pay for the one Sign call:

```bash
export KMS_LIVE_SPEC='us-east-1:alias/wire-ci-test-secp256k1'
# or the full ARN form:
# export KMS_LIVE_SPEC='arn:aws:kms:us-east-1:111122223333:alias/wire-ci-test-secp256k1'
export KMS_LIVE_PUBKEY='045a87...eef3'

$BUILD_DIR/plugins/signature_provider_manager_plugin/test/test_signature_provider_manager_plugin \
   --run_test=kms_signature_provider_tests/kms_live_sign_round_trip \
   --log_level=test_suite
```

Expected output on success:

```
Entering test case "kms_live_sign_round_trip"
Leaving test case "kms_live_sign_round_trip"
*** No errors detected
```

If you want to confirm the skip path works without spending money, unset the
env vars and run the same command — Boost prints
`skipping live KMS test` and the case still reports `No errors detected`.

## Cost & rate-limit notes

`KMS::Sign` is billed at roughly $0.03 per 10 000 calls, plus the per-key
monthly storage fee for the asymmetric key. A single live run is one Sign
call. KMS throttles at 10 000 req/s per region, far above what a single
test needs.

## Common failure modes

| Symptom                                            | Likely cause                                              |
|----------------------------------------------------|-----------------------------------------------------------|
| `Unable to locate credentials`                     | AWS credential chain came up empty — see Prereq #3       |
| `AccessDeniedException: not authorized to perform: kms:Sign` | IAM policy missing — see Prereq #2                |
| `NotFoundException: Invalid keyId`                 | Wrong region in `KMS_LIVE_SPEC`, or alias not created    |
| `BOOST_CHECK(recovered == em_expected) failed`     | `KMS_LIVE_PUBKEY` does not match the key KMS holds       |
| `parse_kms_spec` exception at startup              | Spec body malformed — see `KMS_SIGNING_DESIGN.md` §3     |

See `../KMS_SIGNING_DESIGN.md` for the full design context.
