# Wire Key Formats

Reference for the public-key formats Wire supports, how to generate them, and how
they appear (a) as command-line / string forms and (b) inside OPP protobuf messages.

> Private keys are the user's responsibility. There is **no recovery mechanism** --
> a lost private key means a lost account.

## Key types

Wire's `public_key` is a tagged union over six curve types. The string form is
`PUB_<TYPE>_<encoding>` (private keys: `PVT_<TYPE>_<encoding>`); the `<TYPE>` tag
selects the curve and, for signing, the signature scheme.

| Type  | Curve / scheme                          | String form              | Encoding        | Serialized bytes                         | Typical use |
|-------|-----------------------------------------|--------------------------|-----------------|------------------------------------------|-------------|
| `K1`  | secp256k1, standard (Bitcoin-style)     | `PUB_K1_...` / `PVT_K1_...`   | base58check     | 33 -- compressed point (1-byte 0x02/0x03 prefix + 32-byte X) | Wire account owner/active signing |
| `R1`  | secp256r1 / NIST P-256 (Secure Enclave) | `PUB_R1_...`               | base58check     | 33 -- compressed point                    | Hardware-backed Wire signing |
| `EM`  | secp256k1, Ethereum EIP-191 personal_sign | `PUB_EM_04...`           | **hex**, uncompressed | 33 on-wire (compressed); the string is the 65-byte uncompressed form (0x04 marker + X + Y) | MetaMask / external Ethereum signer |
| `ED`  | ed25519 (Solana)                        | `PUB_ED_...`               | base58 (no csum) | 32 -- raw point                           | External Solana signer |
| `BLS` | BLS12-381 (G1)                          | `PUB_BLS_...`              | base64url       | 96 -- affine point: X (48) + Y (48)         | Finalizer (consensus) keys |
| `WA`  | WebAuthn (secp256r1 + metadata)         | `PUB_WA_...`               | base58check     | 33-byte compressed point + user-presence byte + rpid | WebAuthn-backed signing |

`K1` additionally has a **legacy** form with no `PUB_`/`PVT_` prefix: public keys
print as `SYS...` and private keys as a Bitcoin WIF (`5...`). `clio create key` with no
curve flag emits this legacy K1 form; `--k1` emits the prefixed `PUB_K1_`/`PVT_K1_`
form of the same key.

## Encoding

The string body after `PUB_<TYPE>_` is encoded differently per type -- they are NOT
all the same base58check:

- **`K1`, `R1`, `WA`** -- base58 of `serialize() || checksum`, where
  `checksum = ripemd160(serialize() || "<TYPE>")[:4]` is a 4-byte checksum **salted by
  the type string** (so a `K1` body will not validate as `R1`). This is the classic
  Antelope key encoding. `K1` also has the legacy `SYS...` / WIF form (same checksum,
  `SYS` base prefix), emitted when no `PUB_` prefix is requested.
- **`EM`** -- plain **hex**, no base58 and no checksum, of the **uncompressed** point: a
  `0x04` marker byte, then the 32-byte X and 32-byte Y (65 bytes, 130 hex). `0x04` is SEC1's
  "uncompressed point" tag -- which is why a `PUB_EM_` string always begins `04`; a
  *compressed* point instead starts `0x02`/`0x03` (encoding Y's parity), as `K1`/`R1` do. On
  the wire / in OPP an `EM` key is the 33-byte compressed point; the string merely prints the
  uncompressed form -- same secp256k1 key either way.
- **`ED`** -- plain **base58** of the **raw 32-byte** ed25519 point, with **no checksum**
  (Solana's native pubkey format). Unlike `K1`/`R1` there is no ripemd checksum.
- **`BLS`** -- **base64url** (not base58) of `serialize() || checksum`, where
  `checksum = ripemd160(serialize())[:4]` is **unsalted** (no type string). The `-` / `_`
  characters in a `PUB_BLS_...` string are the base64url alphabet.

In short: `K1`/`R1`/`WA` share the salted base58check scheme; `ED` is base58 but
checksum-less; `BLS` is base64url with an unsalted checksum; `EM` is hex.

The above are the **string** forms. When a key is serialized in **binary** -- in action
data, a contract table, or a signature -- it is a tagged variant: a leading 1-byte index
identifies the type, followed by the raw key bytes. The index is the variant discriminant:

| index | 0  | 1  | 2  | 3  | 4  | 5   |
|-------|----|----|----|----|----|-----|
| type  | K1 | R1 | WA | EM | ED | BLS |

That index is how `K1`, `R1`, and `EM` are told apart even though all three are the same
33-byte secp256k1 point -- so a binary `public_key` is self-describing, and any of the six
types is unambiguous with no external context. The exception is OPP attestation `bytes`
fields (e.g. `actor_pub_key`): those carry the bare key bytes with **no** index, so the
type is supplied out of band by `ChainKind` (see the OPP section below) -- which is why
those fields only cover the external-chain key types.

## Signing performance: K1 vs R1

K1 (secp256k1) is faster than R1 (secp256r1 / NIST P-256), both for signing and
especially for verification. The main reason in this codebase is the implementation
behind each:

- **K1** -> libsecp256k1 (Bitcoin Core's library), one of the most heavily optimized ECC
  libraries that exists. secp256k1 also has structural advantages: `a = 0` simplifies the
  point arithmetic, the prime `p = 2^256 - 2^32 - 977` reduces fast, and it admits the GLV
  endomorphism, which roughly halves the cost of scalar multiplication.
- **R1** -> OpenSSL's ECDSA over NIST P-256. P-256 has `a = -3` and no
  efficiently-computable endomorphism, so verification in particular is meaningfully slower.

Rough ballpark on modern x86:

| Op     | K1 (libsecp256k1) | R1 (OpenSSL P-256)       |
|--------|-------------------|--------------------------|
| Sign   | ~30-50 us         | ~30-60 us (close)        |
| Verify | ~40-60 us         | persistently 2-4x slower |

Signing is roughly comparable; verification is where K1 pulls clearly ahead, which
matters for a blockchain node since it verifies far more signatures than it produces.

Caveats:

- On hardware with dedicated P-256 acceleration (HSMs, Secure Enclave, some TPMs), R1 can
  win -- that is why WebAuthn / secure-element keys use R1.
- The gap is implementation-dominated, not purely curve-theoretic. OpenSSL's P-256 has
  improved a lot, but still does not match libsecp256k1's tuning.

So for raw software performance in nodeop: K1.

## Generating keys (CLI)

```bash
clio create key --to-console        # legacy K1 (SYS... / WIF)   -- default
clio create key --k1 --to-console   # K1   (PUB_K1_ / PVT_K1_)
clio create key --r1 --to-console   # R1   (PUB_R1_ / PVT_R1_)
clio create key --em --to-console   # EM   (PUB_EM_ / PVT_EM_)  -- MetaMask/Ethereum
clio create key --sol --to-console  # ED   (PUB_ED_ / PVT_ED_)  -- Solana ed25519
sys-util bls create key --to-console  # BLS (PUB_BLS_ / PVT_BLS_) + proof of possession
```

`--k1/--r1/--em/--sol` are mutually exclusive. Use `--file <path>` instead of
`--to-console` to write to a file. WebAuthn (`WA`) keys are produced by a WebAuthn
authenticator, not generated by the CLI.

Convert / inspect existing keys:

```bash
clio convert k1_private_key --private-key <PVT_K1_ or WIF> --to-console  # all K1 forms
clio convert k1_public_key  <PUB_K1_ or SYS...>                            # both K1 public forms
clio convert em_private_key --private-key <0x...64hex or PVT_EM_> --to-console  # -> PVT_EM_/PUB_EM_
```

A Wire account's owner/active authority uses a **K1** public key; an Ethereum (`EM`)
or Solana (`ED`) key can additionally be *linked* to the account
(see `sysio.authex::createlink`) for external-chain attestations.

## OPP protobuf form

OPP attestation messages carry public keys as protobuf fields, **not** the `PUB_*_`
string. There are two carriage forms, both defined in
`libraries/opp/proto/sysio/opp/attestations/attestations.proto`:

**1. External-chain key as raw `bytes`, curve implied by `ChainKind`.** The depositor's
key on the originating chain rides as a bare `bytes` field (e.g. `actor_pub_key`); the
depot (`sysio.msgch`) rebuilds a Wire `public_key` via
`public_key_from_op_address(chain_kind, bytes)`:

| `ChainKind`       | Wire key type | `bytes` length / form                              |
|-------------------|---------------|----------------------------------------------------|
| `CHAIN_KIND_EVM`  | `EM`          | 33 -- compressed secp256k1 (0x02/0x03 prefix + 32-byte X) |
| `CHAIN_KIND_SVM`  | `ED`          | 32 -- raw ed25519 point                             |
| `CHAIN_KIND_WIRE` | `K1`          | 33 -- compressed secp256k1                          |

**2. Typed key in a `WireKey` message.** `WireKey` is `{key_type, key}`: `key_type` (the
`WireKeyType` enum) names the `public_key` variant and `key` holds the **raw** key bytes
with **no** variant-index prefix (the prefix is unneeded -- `key_type` is the discriminant):

| `WireKeyType`      | Variant | `key` form                  |
|--------------------|---------|-----------------------------|
| `WIRE_KEY_TYPE_K1` | 0 (K1)  | 33 -- compressed secp256k1   |
| `WIRE_KEY_TYPE_R1` | 1 (R1)  | 33 -- compressed secp256r1   |
| `WIRE_KEY_TYPE_EM` | 3 (EM)  | 33 -- compressed secp256k1   |
| `WIRE_KEY_TYPE_ED` | 4 (ED)  | 32 -- raw ed25519            |

(`WIRE_KEY_TYPE_WA` and `_BLS` exist in the enum but the node-owner depot decode does
not accept them as account keys: WebAuthn is variable-length and BLS is a
consensus/finalizer key, never an account owner/active authority.)

`WireKey` is a standalone message, deliberately **not** a field on the shared `ChainAddress`:
adding a usually-default field to `ChainAddress` would be omitted by proto3 on the wire but
expected by the regenerated abi, breaking the table decodes of every contract that stores a
`ChainAddress`.

`NodeOwnerRegistration` uses both forms: the depositor's external key as `actor_pub_key`
(form 1, EVM) and the **new Wire account's** owner/active key as `wire_pub_key` (form 2, a
`WireKey`). So a Wire-native key *does* ride this attestation -- it is the account key being
established. The depot creates the account from `wire_pub_key`, registers the owner, and
records the depositor's external key in `sysio.authex::links` (keyed by account+chain via
the `bynamechain` index), which is separate from the account's own owner/active authority.

## Using an existing Ethereum keypair as a Wire K1 key

An Ethereum keypair is plain secp256k1 -- the **same curve** as Wire `K1` -- so an
existing Ethereum key can be reused as a Wire `K1` signing key. Two `clio convert`
subcommands do this and print the derived Ethereum address so you can confirm the
result matches the account you expect:

```bash
# Private key: raw Ethereum secret -> PVT_K1_ / PUB_K1_ (+ Ethereum address)
clio convert eth_to_k1_private --private-key 0x<64 hex> --to-console

# Public key only: uncompressed Ethereum pubkey -> PUB_K1_ (+ Ethereum address)
clio convert eth_to_k1_public 0x04<128 hex>      # or 0x<128 hex> for raw X||Y
```

Worked example (EIP-155's published example key `0x46...46`):

```text
$ clio convert eth_to_k1_private --private-key 0x4646464646464646464646464646464646464646464646464646464646464646 --to-console
Private key: PVT_K1_Xx4wsMynSZ39WiwEn8wzRL9trcKK33ZzwaZKJmymYx5UTjKvv
Public key: PUB_K1_5TrYnZP1RkDSUMzBY4GanCy6AP68kCMdkAb5EACkAwkdc8tm4t
Ethereum address: 0x9d8a62f656a8d1615c1294fd71e9cfb3e4855a4f
```

(The address `0x9d8a62f6...4855a4f` is EIP-155's documented sender address for that
key -- an independent check that the conversion is correct.)

### Caveats

- **Signing scheme.** A `K1` key signs with Wire's *standard* secp256k1 scheme, **not**
  Ethereum's EIP-191 `personal_sign`. A browser wallet (MetaMask) **cannot** sign Wire
  transactions for a `K1` key -- the private key must live in a Wire signer (`kiod`).
  If you want to keep signing with the Ethereum wallet, import the key as **`EM`**
  instead (`clio convert em_private_key`), which preserves the EIP-191 path. `EM` and
  `K1` derived from one Ethereum key share the same secret and address; only the
  signature scheme differs.
- **The Ethereum address is one-way.** It is `keccak256(uncompressed_pubkey)[-20:]`,
  a hash -- it cannot be turned back into a public key. The address is printed only for
  verification; conversion always starts from the private key or the public key.
- **Secret exposure.** Passing `--private-key` on the command line leaves the secret in
  shell history / `ps`. Omit it to be prompted instead.
