# Design Note: AWS KMS signing for `signature_provider_manager_plugin`

Status: In progress — registry overlays landed (file:// verified, GitHub-URL verification gated on master merge); CMake wiring underway
Author: brian.johnson@wire.network
Date: 2026-05-08 (initial draft 2026-05-06)
Plugin: `plugins/signature_provider_manager_plugin/`
External dep: [`aws-sdk-cpp`](https://vcpkg.io/en/package/aws-sdk-cpp) feature `kms`

## 1. Motivation

Today the only ways to attach signing material to a `signature_provider_manager_plugin` spec are:

| Provider | Where the secret lives                  | Spec form                       |
|----------|------------------------------------------|---------------------------------|
| `KEY`    | In the `nodeop`/cranker config / env var | `KEY:<hex-or-wif-private-key>`  |
| `KIOD`   | Local `kiod` daemon over HTTP/Unix       | `KIOD:<url>`                    |

Both require the operator either to put a raw private key on the host or to run a local key daemon. For production cranker / outpost-client deployments we want the signing key to live in **AWS KMS** so:

- the private key material never appears on the host or in process memory,
- access is gated by IAM / IMDS / IRSA (no key files to rotate),
- audit of every `Sign` call is centralised in CloudTrail,
- key rotation and disablement do not require redeploying the binary.

The first concrete user is the Ethereum cranker (`bin/cranker`), where the signer address (`0x8950…0800`) currently comes from a `KEY:0x…` literal in `~/scripts/cranker.sh`. Replacing it with a KMS-backed provider closes the only place where a hot Ethereum private key lives on the operator's box.

## 2. Goals / Non-goals

**Goals**

1. Add a new `KMS:` provider type to the existing spec grammar, with no breaking change to `KEY:` / `KIOD:`.
2. Cover the immediate need: **secp256k1** signing for `chain_key_type_ethereum` and `chain_key_type_wire` (sysio K1).
3. Use the standard AWS credential chain — no new config knobs unless we discover we need them.
4. Keep the `signature_provider_t` model intact: `private_key` stays `std::optional` and is `std::nullopt` for KMS-backed providers (same shape as `KIOD`).
5. Ship with unit tests that don't require a live AWS account (mocked `KMSClient`).

**Non-goals**

1. **R1 / NIST P-256 (`chain_key_type_*` for R1)**: KMS supports `ECC_NIST_P256`, but no current Wire plugin needs it. Add later behind the same plumbing.
2. **Ed25519 / Solana keys**: KMS does not support Ed25519 asymmetric signing. Out of scope.
3. **BLS keys**: KMS does not support BLS12-381. Out of scope.
4. **Producer-block signing on the hot path**: KMS Sign is ~30-100 ms / call and ~$0.03 / 10k calls — fine for crank/maintenance traffic, **not** intended for `producer_plugin` block signing. Documented in §8 as a deliberate restriction.
5. **Multi-region failover.**

## 3. Spec grammar

Extend the existing `<provider-spec>` grammar. The full signature-provider spec is unchanged:

```
<name>,<chain-kind>,<key-type>,<public-key>,<provider-spec>
```

`<provider-spec>` adds one form:

```
KMS:<key-ref>
```

where `<key-ref>` is **either** a full ARN

```
arn:aws:kms:<region>:<account>:key/<uuid>
arn:aws:kms:<region>:<account>:alias/<alias-name>
```

**or** the shorthand `<region>:<key-id-or-alias>` for environments that prefer not to embed an ARN:

```
KMS:us-east-1:alias/wire-cranker-eth-01
KMS:us-east-1:1234abcd-12ab-34cd-56ef-1234567890ab
```

Region is mandatory because `Aws::Client::ClientConfiguration::region` must be set explicitly; we do not silently inherit `AWS_REGION` from the environment so that misconfiguration is loud.

Cranker example, drop-in replacement for the current `KEY:` form:

```
--signature-provider eth-01,ethereum,ethereum,0x045a87…eef3,KMS:us-east-1:alias/wire-cranker-eth-01
```

## 4. End-to-end flow

```
caller (e.g. cranker) ──► signature_provider_t::sign(digest)
                                │
                                ▼
                  KMS provider closure (per-spec)
                                │
                  ┌─────────────┴─────────────┐
                  │                           │
                  ▼                           ▼
        Aws::KMS::KMSClient::Sign     cached recovered_pubkey
        MessageType=DIGEST            (from GetPublicKey)
        SigningAlgorithm=ECDSA_SHA_256
                  │
                  ▼
            DER (r, s)
                  │
                  ▼
        DER → raw (r, s)
                  │
                  ▼
        normalise s to low-S    (EIP-2 / canonical secp256k1)
                  │
                  ▼
        recover v ∈ {0, 1} by trial-recovery against pubkey
                  │
                  ▼
        chain::signature_type
        (em::compact_signature for ethereum,
         ecc::signature_shim    for wire k1)
```

Notes:

- The `sign_fn` signature is `fc::crypto::signature(const sha256&)` — the digest is already 32 bytes, so we set `MessageType=DIGEST` and **never** ask KMS to do its own hashing.
- KMS does not return a recovery id. We must derive it locally by trying both parities and comparing the recovered public key to the known one (which we fetch once from KMS via `GetPublicKey` on first use and pin against the spec's `<public-key>` field — see §6).
- DER signatures from KMS occasionally have leading zero padding on `r`/`s`; the parser must strip and zero-extend to 32 bytes.
- Low-S normalisation is required: Ethereum rejects high-S (EIP-2), and `fc::crypto::em::compact_signature` round-trips assume canonical form.

## 5. C++ surface

### 5.1 New translation unit

```
plugins/signature_provider_manager_plugin/
   src/
      kms_signature_provider.cpp        (new)
      kms_signature_provider.hpp        (new, plugin-private)
      signature_provider_manager_plugin.cpp  (modified — one new branch)
```

Anonymous-namespace constants live at the top of `kms_signature_provider.cpp`:

```cpp
namespace {
   constexpr auto kms_spec_prefix          = "KMS";
   constexpr auto kms_signing_algorithm    = "ECDSA_SHA_256";
   constexpr auto kms_message_type_digest  = "DIGEST";
   constexpr auto kms_arn_prefix           = "arn:aws:kms:";
}
```

### 5.2 Public entry point (plugin-private header)

```cpp
namespace sysio::sigprov::kms {

/// Opaque target — either an ARN or a parsed (region, key-id) pair.
struct kms_key_ref {
   std::string region;
   std::string key_id;     ///< raw KMS KeyId, alias name, or ARN tail
};

/// Parses `KMS:<key-ref>` body (everything after `KMS:`).
kms_key_ref parse_kms_spec(std::string_view spec_data);

/// Builds a `sign_fn` closure backed by AWS KMS for the given key reference,
/// validating that the recovered public key matches `expected_pubkey`.
fc::crypto::sign_fn make_kms_signature_provider(
   const kms_key_ref&                ref,
   fc::crypto::chain_key_type_t      key_type,
   const fc::crypto::public_key&     expected_pubkey);

} // namespace sysio::sigprov::kms
```

### 5.3 Hook into `create_provider_from_spec`

In `signature_provider_manager_plugin.cpp::signature_provider_manager_plugin_impl::create_provider_from_spec`, add a third branch alongside `KEY` / `KIOD`:

```cpp
if (spec_type_str == kms_spec_prefix) {
   auto ref = sysio::sigprov::kms::parse_kms_spec(spec_data);
   return { sysio::sigprov::kms::make_kms_signature_provider(ref, key_type, public_key),
            std::nullopt };
}
```

Same shape as `KIOD`: returns `std::nullopt` for the optional `private_key` because nothing leaves AWS.

### 5.4 SDK lifecycle

`Aws::InitAPI` / `Aws::ShutdownAPI` are process-global and not safe to call concurrently. Wrap in a Meyers-singleton guard owned by the plugin:

```cpp
class aws_sdk_lifecycle {
public:
   static aws_sdk_lifecycle& instance() {
      static aws_sdk_lifecycle s;
      return s;
   }
private:
   aws_sdk_lifecycle()  { Aws::InitAPI(_options); }
   ~aws_sdk_lifecycle() { Aws::ShutdownAPI(_options); }
   Aws::SDKOptions _options{};
};
```

`make_kms_signature_provider` calls `aws_sdk_lifecycle::instance()` once before constructing its first `KMSClient`. Static destruction order guarantees `ShutdownAPI` runs after the last `KMSClient` is destroyed (closures hold the client by `shared_ptr`).

### 5.5 `KMSClient` reuse

One `std::shared_ptr<Aws::KMS::KMSClient>` per `(region, credentials)` pair, kept in a process-wide map keyed by region. Construction is cheap but not free; multiple cranker specs in the same region must not each spin up a fresh client.

### 5.6 Threading

`Aws::KMS::KMSClient::Sign` is thread-safe (per AWS SDK docs and source). The closure captures `std::shared_ptr<KMSClient>` by value and is itself thread-safe.

### 5.7 Public-key pinning

On first sign for a given closure, call `KMSClient::GetPublicKey` once, parse the DER X.509 SubjectPublicKeyInfo to raw secp256k1 (uncompressed `04 || X || Y`), compare to `expected_pubkey`. Throw `chain::plugin_config_exception` on mismatch. Cache the parsed pubkey for subsequent v-recovery.

This catches the common operator error of putting a different `<public-key>` in the spec than the KMS key actually holds.

## 6. Build wiring

### 6.1 `vcpkg.json`

Add to `dependencies`:

```json
{
   "name": "aws-sdk-cpp",
   "default-features": false,
   "features": ["kms"]
}
```

`default-features: false` is important — the upstream port defaults to `dynamodb`, `kinesis`, `s3` which we do not need (the upstream `vcpkg.in.json` confirms this).

The aws-sdk-cpp port already pulls in `curl`, `openssl`, and `zlib`, all of which we have. `aws-crt-cpp` is a transitive dep and uses `s2n` on Linux — verify it doesn't conflict with our `boringssl-custom` curl feature (see §9 risks).

### 6.2 Plugin `CMakeLists.txt`

```cmake
find_package(aws-cpp-sdk-kms CONFIG REQUIRED)

plugin_target(
   ${TARGET_NAME}
   SOURCE_GLOBS
   src/*.cpp
   src/*.hpp
   include/*.hpp

   LIBRARIES
   custom_appbase
   http_client_plugin
   wallet_plugin_headers
   aws-cpp-sdk-kms
)
```

The `aws-cpp-sdk-kms` config (vcpkg-installed at `share/aws-cpp-sdk-kms/`) imports a STATIC `aws-cpp-sdk-kms` target and `find_dependency(aws-cpp-sdk-core)`. Transitive deps (`aws-crt-cpp` → `aws-c-cal` → `boringssl-custom`, `s2n`, etc.) propagate via `INTERFACE_LINK_LIBRARIES`. The umbrella `find_package(AWSSDK REQUIRED COMPONENTS kms)` is also viable but pulls in extra setup files (`sdksCommon.cmake`, `platformDeps.cmake`, `compiler_settings.cmake`) we do not need.

## 7. Tests

New test source: `plugins/signature_provider_manager_plugin/test/test_kms_signature_provider.cpp`, registered as a `boost_test`-style suite (`kms_signature_provider_tests`) and run from `tests/plugin_test`.

| Suite case                                  | What it pins                                           |
|---------------------------------------------|---------------------------------------------------------|
| `parse_kms_spec_arn`                        | full-ARN form parses to (region, key-id)                |
| `parse_kms_spec_region_keyid`               | `<region>:<key-id-or-alias>` form parses               |
| `parse_kms_spec_rejects_no_region`          | bare key-id throws `plugin_config_exception`           |
| `der_to_raw_strips_leading_zero`            | DER (r,s) round-trips to 32+32 raw                     |
| `low_s_normalisation`                       | high-S input flips to low-S, untouched if already low  |
| `recover_v_zero_and_one`                    | both parities recovered correctly via mocked pubkey    |
| `pubkey_mismatch_throws`                    | mocked `GetPublicKey` returning wrong key throws        |
| `sign_round_trip_against_secp256k1_fixture` | mocked Sign with a known DER blob → expected `signature`|
| `sdk_init_idempotent`                       | constructing two providers in the same region init's once|

The mock implements `Aws::KMS::KMSClient` by overriding `Sign(...)` and `GetPublicKey(...)`. Fixtures are precomputed offline with a fresh secp256k1 key so the test does not need network or AWS creds.

A separate live-account test (`KMS_LIVE_TEST=1` env-gated) hits a sandbox alias `alias/wire-ci-test-secp256k1` for end-to-end verification — opt-in only, never run in default CI.

## 8. Operational guidance (will land in `signature_provider_help_text()`)

```
KMS:<key-ref>     <key-ref> is either an ARN
                  (arn:aws:kms:<region>:<account>:(key|alias)/<id>)
                  or <region>:<key-id-or-alias-name>.

                  The KMS key must be an asymmetric ECC_SECG_P256K1 key
                  with usage SIGN_VERIFY. AWS credentials are taken from
                  the standard chain (env, shared config, IRSA, IMDS).

                  Recommended for outpost cranker / maintenance signers.
                  NOT recommended for producer block signing — adds
                  ~30-100ms per signature and is rate-limited per AWS
                  account / region.
```

## 9. Risks & open questions

1. **`aws-sdk-cpp` / `aws-c-cal` / `s2n` ↔ `boringssl-custom` collision (RESOLVED).** The original observation: `boringssl-custom` claims OpenSSL's install footprint (same header paths, same `libssl.a`), so any port that declares an upstream `openssl` dep collides at port-install time. Three layers in the AWS dep graph declare `openssl`:

   ```
   aws-sdk-cpp ──► aws-crt-cpp
                     ├── aws-c-cal ──► (declares openssl)
                     └── aws-c-io
                           └── s2n  ──► (declares openssl)
   aws-sdk-cpp itself ─────────────────► (declares openssl)
   ```

   **Resolution (landed):** overlay ports in `wire-network/wire-vcpkg-registry` on branch `feature/aws-sdk-cpp-boringssl` (commits `641bb7f`, `d1e4cc9`, `088646f`, `c739f08`):

   - `ports/aws-sdk-cpp/` — manifest only; drops the `openssl` dep (the SDK's per-OS `OpenSSLImpl` was removed before 1.11.591, crypto delegates to `aws-crt-cpp`).
   - `ports/aws-c-cal/` — drops the `openssl` dep, replaces it with `boringssl-custom`; patches `CMakeLists.txt` and the installed `aws-c-cal-config.cmake` to use `boringssl::crypto`.
   - `ports/s2n/` — drops the `openssl` dep, replaces it with `boringssl-custom`; patches `CMakeLists.txt` and the installed `s2n-config.cmake` to use `boringssl::crypto`; relaxes `-Werror` on the build (upstream tracks bleeding-edge compilers).

   `vcpkg-configuration.json` claims all three port names in the wire registry's `packages` array and pins `baseline: c739f089...` (`aws-sdk-cpp 1.11.665`, `aws-c-cal 0.9.3`, `s2n 1.5.27`). End-to-end verified against a `file:///home/swamp/dev/wire-vcpkg-registry` URL: all 50 ports install cleanly, `libaws-cpp-sdk-kms.a` + `libs2n.a` + `libbscrypto.a` materialise in `vcpkg_installed/x64-linux/lib/`, no port-install collision, `cmake -B build -S .` exits 0 in 105.9 s.

   **Pending durability step:** `feature/aws-sdk-cpp-boringssl` must merge to `master` in the registry. vcpkg's git-registry uses the registry's HEAD branch for port discovery (independent of the configured baseline SHA), so consumers fetching from `https://github.com/wire-network/wire-vcpkg-registry` will get `error: aws-sdk-cpp does not exist` until the branch lands on master. PR or fast-forward push to master closes this.
2. **Determinism.** secp256k1 ECDSA in KMS is **not** RFC-6979 deterministic. Each call returns a fresh `(r, s)`. Fine for transactions (nonce + chainId pin replay), but a hard "no" for any signing path that consensus assumes is deterministic across nodes. Documented in §8.
3. **Cold-start latency.** First sign on a fresh process can take 200-800 ms while the credential provider chain resolves IMDS / IRSA. **Mitigation:** call `GetPublicKey` once at plugin startup to warm the client, fail fast if creds are missing.
4. **Cost.** $0.03 per 10k Sign calls plus per-key monthly storage. Trivial for cranker (≪ 1 sign/min); flag in §8.
5. **Throttling.** KMS default is 10k req/s shared per region. Far above what any single signer needs, but AWS SDK's default retry strategy already handles `ThrottlingException` — confirmed acceptable, no custom retry policy needed.
6. **Static-destruction order of `aws_sdk_lifecycle`.** Closures captured into `signature_provider_t::sign` hold the `KMSClient` by `shared_ptr`. If a `signature_provider_t` outlives the plugin (e.g. captured into a long-lived appbase context) static destruction order matters. **Mitigation:** plugin clears its provider map in `plugin_shutdown` before SDK shutdown runs.

## 10. Implementation plan

Each step is independently reviewable. Order matters — earlier steps unblock later ones.

| # | Step | Files touched | Verification |
|---|------|---------------|--------------|
| 1a | **DONE (locally verified, pending master merge).** Overlay ports `aws-sdk-cpp`, `aws-c-cal`, `s2n` in `wire-network/wire-vcpkg-registry`, swapping `openssl` → `boringssl-custom`; baseline bumped; port names registered in `vcpkg-configuration.json`'s `packages` array. See §9 risk #1. | `wire-vcpkg-registry/ports/{aws-sdk-cpp,aws-c-cal,s2n}/`, `wire-vcpkg-registry/versions/`, `vcpkg-configuration.json` | All three ports install via `cmake -B build -S .` against a `file://` clone (verified). GitHub-URL access pending merge of `feature/aws-sdk-cpp-boringssl` → `master`. |
| 1b | **DONE.** `aws-sdk-cpp` (`kms` feature, no defaults) added to `vcpkg.json`. | `vcpkg.json` | vcpkg installs `libaws-cpp-sdk-kms.a` + transitive deps with `--default-features=false`; configure exits 0 in 105.9 s. |
| 2 | Smoke-link AWS SDK into the plugin's `CMakeLists.txt` (no code yet). | `plugins/signature_provider_manager_plugin/CMakeLists.txt` | `ninja -C $BUILD_DIR signature_provider_manager_plugin nodeop` succeeds; build graph references `aws-cpp-sdk-kms`. |
| 3 | Spec parser: `parse_kms_spec` + unit tests for ARN / shorthand / error paths. | `kms_signature_provider.{hpp,cpp}`, `test_kms_signature_provider.cpp` | New `parse_kms_spec_*` tests pass under `plugin_test`. |
| 4 | DER→raw + low-S normalisation + v-recovery helpers, with secp256k1 fixtures (no AWS calls). | `kms_signature_provider.cpp`, test file | `der_to_raw_*`, `low_s_*`, `recover_v_*` cases pass. |
| 5 | `aws_sdk_lifecycle` singleton + per-region `KMSClient` cache. | `kms_signature_provider.cpp` | `sdk_init_idempotent` test passes. |
| 6 | `make_kms_signature_provider` end-to-end with mocked `KMSClient` (Sign + GetPublicKey). | `kms_signature_provider.cpp`, test file | `sign_round_trip_*`, `pubkey_mismatch_throws` pass. |
| 7 | Wire `KMS` branch into `signature_provider_manager_plugin_impl::create_provider_from_spec`; update help text. | `signature_provider_manager_plugin.cpp` | `--signature-provider …,KMS:…` parses; existing `KEY` / `KIOD` paths untouched. |
| 8 | Verify on Hoodi against a real KMS key — point cranker at `KMS:us-east-1:alias/wire-cranker-eth-01`, confirm signer address matches the existing `0x8950…0800` (or whatever the new authorised address is, depending on how the role-grant from yesterday's diagnosis is resolved). | `~/scripts/cranker.sh` (operator config, not committed) | Cranker submits a successful tx (no `AccessManagedUnauthorized` revert from §1 of yesterday's diagnosis, no `KMSException`). |
| 9 | **DONE.** Documentation pass: `signature_provider_help_text()` extended in step 7; `BUILD.md` now notes the `aws-sdk-cpp[kms]` + overlay-port chain pulled by vcpkg on first configure; `OVERVIEW.md` Batch Operators section mentions the new `KMS:` option alongside `KEY:`/`KIOD:`. | `signature_provider_manager_plugin.cpp`, `BUILD.md`, `OVERVIEW.md` | Docs reflect the landed implementation. |

Steps 1-7 are pure-local, no AWS account needed. Step 8 is the only one that touches a real KMS key and is gated on having a provisioned secp256k1 KMS key + an IAM grant for the runner.
