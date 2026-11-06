# Raw Ed25519 verification

`ed25519_verify` verifies one detached Ed25519 signature over unchanged message
bytes using the pinned libsodium verifier. The companion CDT provides a helper
that verifies multiple signatures independently in a WASM loop, using the caller's
existing buffers. The host has no batch parser, loop, or batch-sized allocation.

## Contract import

```cpp
extern "C" {
__attribute__((sysio_wasm_import))
int32_t ed25519_verify(const char* sig, uint32_t sig_len,
                       const char* pub, uint32_t pub_len,
                       const char* msg, uint32_t msg_len);
}
```

`env.ed25519_verify` has type `(i32, i32, i32, i32, i32, i32) -> i32`.
It returns **0 on success and -1 on failure**; do not interpret it as a boolean.
Pass exactly 64 signature bytes, exactly 32 public-key bytes, and a message of
0 through **65,536 bytes**, inclusive. Inputs may be unaligned. There are no type
tags, embedded public keys, packed records, or encoded message lengths.

The message is neither hex-encoded nor prehashed. This is plain Ed25519, not
Ed25519ph or Ed25519ctx. The existing `recover_key` API retains its signature
format and ASCII-hex digest behavior. Both verification paths use the same raw
helper and libsodium initialization.

The import is available to normal and context-free actions through the genesis whitelist.

## Acceptance rules

The consensus implementation is Wire-Network/libsodium commit
[`abd3612f433b795a6ac31a386a75f8f26a479033`](https://github.com/Wire-Network/libsodium/tree/abd3612f433b795a6ac31a386a75f8f26a479033),
pinned by the custom vcpkg registry. Its version string is `1.0.20`, but it contains
changes after the `1.0.20-RELEASE` tag. The version string alone does not identify
the consensus implementation. **Treat a libsodium port/source bump as a consensus
change**, and review the golden acceptance tests on every supported architecture.

For signature `(R, S)`, public key `A`, and message `M`, the verifier requires:

- `S < L`, where `L` is the prime subgroup order.
- `R` is not a small-order point.
- `A` has a canonical encoding, decodes to a curve point, and is not small-order.
  Mixed-order public keys are permitted; no prime-order subgroup pre-check is added.
- The **cofactorless** equation holds: compute `[S]B - [k]A`, with
  `k = SHA-512(R || A || M) mod L`, encode that point, and compare its encoding
  byte-for-byte with `R` from the signature.

The encoding comparison rejects noncanonical `R`. A mixed-order key can verify
when its torsion contribution cancels. Satisfying only the cofactored equation is
insufficient. A mixed-order `R` with prime-order `A` fails; some inputs with both
mixed-order `A` and `R` satisfy the full equation and are accepted. Frozen vectors
pin these distinctions rather than relying on the broad label “strict Ed25519.”

Invalid sizes, signatures, or keys return -1. Correctly signed empty messages
are supported. No assertion-based signature deserialization occurs.

## Batches and resource limits

The [CDT helper](https://github.com/Wire-Network/wire-cdt/blob/master/docs/ed25519-verification.md)
`sysio::ed25519_verify_batch` takes a span of non-owning
`sysio::ed25519_signature_view` entries. It returns 0 if every entry verifies,
stops at the first failure, and returns -1 for an empty batch. It does not copy
messages, signatures, or keys. Each signature is verified independently; this is
not randomized or cofactored batch verification.

The host performs one size-limited verification per call, with **no explicit
`checktime()` calls**. The WASM runtime's expiration callback enforces the deadline
when WASM execution resumes. Moving the loop into WASM lets that enforcement run
between verifications. A libsodium call already in progress may finish after the
deadline; the message cap bounds uninterrupted host work, without promising zero
wall-clock overshoot. Normal transaction resource limits bound total batch work.

Deadline, CPU exhaustion, interruption, and invalid WASM memory abort the
transaction normally. They are never converted into a verification result.
Infrastructure failures such as libsodium initialization failure also remain
exceptions. All three input spans are validated by the runtime before host entry,
even when an input length would otherwise produce -1.

Repeated messages, keys, and signatures are permitted. A successful result proves
only signature validity under these rules. Light-client contracts remain
responsible for source-chain message encoding, validator membership, signer
uniqueness, weights, quorum, and replay protection.

## Single-message example

The [companion CDT API](https://github.com/Wire-Network/wire-cdt/blob/master/docs/ed25519-verification.md) exposes the
same single-message call and documents construction of borrowed batch views:

```cpp
#include <sysio/crypto_ext.hpp>
#include <array>

constexpr uint32_t signature_size = 64;
constexpr uint32_t public_key_size = 32;
constexpr int32_t verification_success = 0;

/// Verify existing buffers without packing or copying their contents.
bool verify_message(const char* message, uint32_t message_size,
                    const std::array<char, signature_size>& signature,
                    const std::array<char, public_key_size>& public_key) {
   return sysio::ed25519_verify(signature, public_key, {message, message_size}) == verification_success;
}
```

## Validation

`ed25519_intrinsic_tests` deploys the checked-in WASM fixture and covers published
RFC 8032 vectors, frozen acceptance rules, binary/empty messages, independent
WASM loops of 1/24/68 signatures, invalid sizes, context-free actions, memory
bounds, native export dispatch, already-expired execution, and timeout rollback.
The `ed25519_tests` suite in `test_fc` runs all the same acceptance vectors, including invalid lengths,
through the shared verifier and preserves the transaction digest-recovery convention.
See the [golden-vector provenance](../libraries/libfc/test/crypto/ed25519/README.md).

Checked-in WASM/ABI artifacts support `BUILD_TEST_CONTRACTS=OFF`. Rebuild the
fixture with CDT and update both artifacts when changing it. Changes to the genesis
intrinsic whitelist require regeneration of the snapshot compatibility fixtures
and their expected snapshot-info values using the repository's documented workflow.

Run each available runtime from the build directory:

```sh
./libraries/libfc/test/test_fc --run_test=ed25519_tests
./unittests/unit_test --run_test=ed25519_intrinsic_tests --log_level=message -- --sys-vm
./unittests/unit_test --run_test=ed25519_intrinsic_tests --log_level=message -- --sys-vm-jit
./unittests/unit_test --run_test=ed25519_intrinsic_tests --log_level=message -- --sys-vm-oc
```

The contract suite reports representative elapsed times without hardware-dependent
performance assertions. Record machine-specific build and timing results in the
PR description rather than treating them as API guarantees.
