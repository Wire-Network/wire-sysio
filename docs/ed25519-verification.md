# Raw Ed25519 verification

`ed25519_verify` verifies one or more detached Ed25519 signatures over the exact message bytes supplied by a contract. It uses the existing pinned libsodium implementation. A single signature is a batch with one record.

This is independent of `recover_key`: that API retains its existing signature formats and hex-encoded-digest behavior for Ed25519. This intrinsic does not hex-encode, prehash, decode a public-key string, or take a public key from a signature. It verifies plain Ed25519, not Ed25519ph or Ed25519ctx.

## Contract import

```cpp
extern "C" {
__attribute__((sysio_wasm_import))
bool ed25519_verify(const char* entries, uint32_t entries_len, uint32_t count);
}
```

The WASM import is `env.ed25519_verify`, with type `(i32, i32, i32) -> i32`. The result is `1` for true and `0` for false. It is usable from both normal and context-free actions and is present in the genesis intrinsic whitelist.

The intrinsic is intended for newly initialized chains using this release. Updating a node binary does not insert it into an existing chain's persisted intrinsic whitelist. No protocol-feature activation or existing-state migration is included.

## Packed records

`entries_len` is the exact total buffer size in bytes; `count` is the number of records. Records are concatenated without alignment padding, an outer length prefix, or a serialization version.

| Offset within a record | Size | Contents |
| --- | --- | --- |
| 0 | 4 bytes | Unsigned message length, little-endian |
| 4 | 64 bytes | Detached Ed25519 signature |
| 68 | 32 bytes | Raw Ed25519 public key |
| 100 | Message length | Unchanged message bytes |

The next record immediately follows the message. Message lengths are fixed-width little-endian integers, **not** the variable-length sizes produced by packing a C++ vector. Signatures and keys have no type tags or embedded lengths. Unaligned record addresses are supported.

Each message may contain 0 through **65,536 bytes**, inclusive. Repeated messages, public keys, and signatures are accepted when they verify. There is no separate intrinsic batch-count limit: the supplied WASM buffer and transaction resource limits bound the work.

## Results and resource limits

The intrinsic returns true only when every record verifies and all input bytes are consumed. It returns false for:

- A zero count, an incomplete record, a mismatched count, or trailing bytes.
- A message exceeding 65,536 bytes.
- A signature that does not verify, or a signature/key encoding rejected by the pinned libsodium verifier, including noncanonical scalars and small-order inputs.

Verification stops at the first invalid record. The boolean does not identify that record. The intrinsic neither mutates the input nor allocates memory proportional to the batch.

Transaction time is checked on entry and immediately before and after every detached verification, including unsuccessful verification. Libsodium cannot be interrupted within a verification; the fixed per-message limit bounds the amount of uninterrupted work. This is cooperative deadline enforcement, not a guarantee of zero wall-clock overshoot.

Deadline, CPU exhaustion, interruption, and invalid WASM memory errors retain the runtime's normal transaction-abort behavior. They are not converted into false, since doing so could let a contract observe node-local timing or continue after exhausting its budget. Infrastructure failures such as libsodium initialization failure also remain exceptions.

A true result proves the supplied signatures verify. The calling light-client contract must separately validate the source chain's signed-message encoding, validator membership, signer uniqueness, weights, quorum, and replay rules. This API uses libsodium's verification acceptance rules; it does not implement chain-specific alternatives for unusual signature encodings.

## Single-message convenience wrapper

This example requires no new CDT API. It packs one record and calls the same intrinsic used for larger batches.

```cpp
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wire_crypto {
inline constexpr uint32_t max_message_size = 65'536;
inline constexpr uint32_t length_size = 4;
inline constexpr uint32_t signature_size = 64;
inline constexpr uint32_t public_key_size = 32;
inline constexpr uint32_t header_size = length_size + signature_size + public_key_size;
inline constexpr uint32_t bits_per_byte = 8;
inline constexpr uint32_t byte_mask = 0xff;

/** Verify unchanged bytes with a detached signature and a separate public key. */
bool verify_message(const char* message, uint32_t message_size,
                    const std::array<char, signature_size>& signature,
                    const std::array<char, public_key_size>& public_key) {
   if (message_size > max_message_size)
      return false;

   std::vector<char> packed(header_size + message_size);
   for (uint32_t i = 0; i < length_size; ++i)
      packed[i] = static_cast<char>((message_size >> (bits_per_byte * i)) & byte_mask);
   std::memcpy(packed.data() + length_size, signature.data(), signature.size());
   std::memcpy(packed.data() + length_size + signature_size, public_key.data(), public_key.size());
   if (message_size != 0)
      std::memcpy(packed.data() + header_size, message, message_size);

   return ::ed25519_verify(packed.data(), static_cast<uint32_t>(packed.size()), 1);
}
} // namespace wire_crypto
```

For a batch, append the same record layout for each message, checking the total byte size fits `uint32_t`, and pass the corresponding count. Packing a single record may copy the message; the core exposes one ABI to keep single and batch verification behavior identical.

## Validation

The `ed25519_intrinsic_tests` suite deploys the compiled `ed25519_verify_test` contract and covers RFC 8032 vectors, binary/empty messages, invalid signatures and encodings (including S+L and identity-key forgeries), batches of 1/24/68 records, malformed buffers, the message-size boundary, context-free use, WASM memory boundaries, and timeout rollback. A direct host test ensures deadline expiration is detected inside the intrinsic without relying on a later WASM checkpoint; it also exercises the native export when enabled.

The checked-in WASM and ABI allow `BUILD_TEST_CONTRACTS=OFF`. Rebuild the contract with CDT and copy both artifacts back into its source directory when changing the fixture.

Run from the build directory, using each available runtime:

```sh
./unittests/unit_test --run_test=ed25519_intrinsic_tests --log_level=message -- --sys-vm
./unittests/unit_test --run_test=ed25519_intrinsic_tests --log_level=message -- --sys-vm-jit
./unittests/unit_test --run_test=ed25519_intrinsic_tests --log_level=message -- --sys-vm-oc
```

The suite prints representative host and end-to-end timings without asserting hardware-dependent performance thresholds.

### Validation record (2026-10-22)

Built on Linux x86-64 (AMD EPYC 9654P) with Clang 18, Debug (`-g1`), libsodium 1.0.20, optimized JIT and native exports enabled. The main build uses the checked-in system and test contracts (`BUILD_SYSTEM_CONTRACTS=OFF`, `BUILD_TEST_CONTRACTS=OFF`). The new contract was separately rebuilt using the installed Wire CDT CMake toolchain; its source-tree and build-tree WASM hashes match.

- All eight new cases pass on each of `sys-vm`, `sys-vm-jit`, and `sys-vm-oc`. The single-message documentation example also compiles with CDT.
- The production `nodeop` executable builds and its version command runs successfully.
- The native export is present in the executable's dynamic symbol table and is called by the host deadline test, including exception propagation.
- All 730 libfc cases pass, including existing Ed25519/Solana behavior. Crypto golden/primitives, contract crypto API, intrinsic probes, and checktime regressions pass on all three runtimes.
- Snapshot compatibility, protocol-feature digests, and `sys-util snapshot info` pass after fixture regeneration.

Observed direct host verification times during concurrent regression runs, using repeated RFC 8032 empty-message records:

| Entries | Observed elapsed time |
| --- | --- |
| 1 | 145–359 µs |
| 24 | 2,955–7,686 µs |
| 68 | 8,309–21,684 µs |

These Debug-build measurements include machine load and are not throughput guarantees or deadline-overshoot bounds. The contract tests separately report action-plus-block-production time. Deadline tests exercise expiration, not a minimum verification rate.

The genesis whitelist changes the snapshot integrity hash. Following `AGENTS.md`, the compatibility snapshots and block log were regenerated with `snapshot_part2_tests/* -- --sys-vm --save-snapshot --generate-snapshot-log`, and the expected head block ID in `sysio_util_snapshot_info_test.py` was updated. This workflow deploys the repository's current embedded fixture contracts. The consensus blockchain and deep-mind fixtures passed unchanged.

The broader sweep passed the plugin suite and all 39 system-contract suites. Every interpreter core suite passed except the native-overlay check described below. The long snapshot-attestation suites completed using individual concurrent cases to avoid serializing their 25,000-block fixtures. One unrelated configuration limitation remains: `native_overlay_tests/overlay_routes_native_contract` requires `contracts/sysio.token/sysio.token_native.so`, which is not produced with `BUILD_SYSTEM_CONTRACTS=OFF`. That check cannot pass in this artifact-only configuration; the new Ed25519 native-export checks above do pass.
