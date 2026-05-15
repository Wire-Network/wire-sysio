# Metamask transaction-signing harness

End-to-end test that a Wire transaction signed by **MetaMask** (or any wallet that
exposes EIP-191 `personal_sign` over secp256k1) is accepted by `nodeop`.

Wire supports this in C++ -- `signature_shim::recover()` applies the EIP-191
prefix to a `sha256` transaction digest and recovers an `em::public_key`, and
the `expandauth_sign_with_eth_key` unit test proves the full in-process flow.
This directory adds the **external-signer** half: real MetaMask in a browser,
signing a real trx that gets pushed to nodeop.

The automated path drives `clio`'s own EM tooling (`clio create key --em`,
`clio convert em_sign` / `em_recover`), which is libfc's own crypto -- the exact
code nodeop runs to validate these signatures. There is **no Python crypto
dependency**: the only thing this harness needs is the `clio` binary the build
already produces. clio's EM path is pinned byte-for-byte against the
Ethereum-ecosystem reference (`eth_account` / MetaMask `personal_sign`) by the
separate, hermetic `clio_em_key_test` ctest; this harness exercises the full
chain round-trip.


## What's here

| File | Role |
| ---- | ---- |
| `metamask-sign.html` | Static page that talks to MetaMask via `window.ethereum`. Calls `personal_sign` on a 32-byte digest, locally ecRecovers the public key, and displays both raw hex (MetaMask format) and Wire's `SIG_EM_` / `PUB_EM_` forms ready to paste into the push helper. |
| `push_metamask_trx.py` | TestHarness-driven end-to-end test. Stands up a single-node cluster, creates an account, calls `expandauth` to add the EM key, seeds it with SYS transferred from `sysio`, builds two unsigned `sysio.token::transfer` trxs, signs each digest (via clio in `--simulate`, or pasted-from-browser in manual mode), and pushes one via HTTP `send_transaction2` and one via `clio push transaction`. |


## Quick offline sanity check

No node or Python deps -- just clio:

```bash
cd $BUILD_DIR
eval "$(bin/clio create key --em --to-console | sed 's/Private key: /PRIV=/;s/Public key: /PUB=/')"
DIGEST=$(printf 'wire metamask sanity' | sha256sum | cut -d' ' -f1)   # any 32-byte sha256
SIG=$(bin/clio convert em_sign "$DIGEST" --private-key "$PRIV" | sed 's/Signature: //')
bin/clio convert em_recover "$SIG" "$DIGEST"   # -> Public key: $PUB
```

The recovered `PUB_EM_` equals the generated one when the EIP-191 envelope, the
secp256k1 recovery, and Wire's plain-hex `SIG_EM_` / `PUB_EM_` framing all
round-trip. The exhaustive version of this check, including the frozen
known-answer vector minted from `eth_account`, is `ctest -R clio_em_key_test`.


## Full end-to-end (automated)

This is the path `ctest -R metamask_trx_signing_test` runs.

```bash
cd $BUILD_DIR
python3 tests/metamask/push_metamask_trx.py --simulate -v
```

The script:

1. Launches a single-node cluster via TestHarness.
2. Creates `mmtest11`, gives it K1 keys.
3. Generates an EM keypair with `clio create key --em` and adds the `PUB_EM_`
   to `mmtest11@active` via `sysio::expandauth`. (`--sim-private-key` pins a
   fixed `PVT_EM_` or raw `0x` Ethereum secret for reproducibility.)
4. Transfers `100.0000 SYS` from `sysio` to `mmtest11` (bootstrap issued the
   full max_supply to `sysio`, so just transfer instead of `issue`).
5. **Round 1 - HTTP path:** builds an unsigned `sysio.token::transfer
   mmtest11 -> defproducera` via `clio push action -s -d --return-packed -j`,
   computes `sig_digest = sha256(chain_id || packed_trx || zero_32)`, signs
   via `clio convert em_sign`, and `POST`s the packed_transaction directly to
   `/v1/chain/send_transaction2`.
6. **Round 2 - clio path:** builds a second unsigned transfer (different
   memo so the trx id is unique), computes its digest, signs, and pushes via
   `clio convert unpack_transaction ... | clio push transaction <json> --signature SIG_EM_... -s`.
7. After each round, fetches `mmtest11`'s SYS balance and asserts it dropped by
   exactly the transfer amount (integer minimal units, no float), and that the
   trx reports `processed.except == null` and `processed.error_code == null`.


## Full end-to-end (real MetaMask)

```bash
cd $BUILD_DIR
python3 tests/metamask/push_metamask_trx.py -v
```

Open `metamask-sign.html` in a browser with MetaMask. If the browser can open
the local file directly, that works with no server. Under WSL (browser on the
Windows side) serve it instead, e.g. from `tests/metamask/`:
`python3 -m http.server 8866 --bind 0.0.0.0`, then open
`http://localhost:8866/metamask-sign.html`. The script will:

1. Spin up the cluster as above.
2. Prompt for your Wire `PUB_EM_` -- the HTML page shows it once you sign any
   test message.
3. Add the `PUB_EM_` to the test account via `expandauth`.
4. Print the trx `sig_digest`. Paste that into the HTML page's "Digest to sign"
   field, click "Sign with Metamask", copy the `SIG_EM_` value.
5. Paste the `SIG_EM_` back into the script. It is recovered offline via
   `clio convert em_recover` and checked against the registered `PUB_EM_`
   before being sent; a stale or wrong-digest signature is rejected locally and
   re-prompted.
6. Push, check execution. Repeated for Round 2 (different digest).


## Notes / gotchas

- MetaMask's `personal_sign` accepts the message either as a UTF-8 string or as
  `0x`-prefixed hex. **Always pass `0x` + 64 hex chars** for a 32-byte digest:
  if you pass a UTF-8 string by mistake, MetaMask prefix-wraps the UTF-8 bytes
  of the *string*, not the underlying digest, and the chain rejects the
  signature.
- EIP-191 v-byte is `27` or `28` (recovery_id + 27). libfc's `em::sign_sha256`
  emits `27/28`; recovery accepts either.
- ECDSA signatures must be canonical (`s < n/2`). MetaMask and libfc produce
  canonical signatures, but ad-hoc third-party signers may not; Wire's
  `em::public_key::is_canonical()` rejects them.
- The Wire EM form is hex, not base58 + ripemd160 like K1/R1/BLS:
    - `SIG_EM_0x<130hex>`: `to_hex()` always prefixes 0x, so the 0x lives
      between `SIG_EM_` and the hex chars.
    - `PUB_EM_<130hex>`: no `0x` in the middle; first two hex chars are the
      SEC1 `04` uncompressed prefix.
- The Wire send_transaction2 reply's success indicator is
  `processed.except == null` and `processed.error_code == null`; the trx-level
  `receipt` field can be `null` while the trx still executes successfully.
- `--trx-expiration` (default 600s) must be in `(0, 3600]` -- the chain's
  `max_transaction_lifetime` under TestHarness. Manual signing needs the wide
  window because the expiration is baked into the bytes the signer hashes and
  cannot be changed after signing.
- This test pushes through `sysio.token::transfer` because it requires
  authentication to succeed and visibly mutates state (balance change). The
  same path works for any action; swap the body of `push_metamask_trx.py` if
  you need to exercise something else.
- `--sim-private-key` and `clio convert em_private_key/em_sign --private-key`
  put a secret on the command line, which is visible in `ps` and shell history.
  These are test/throwaway keys, so it does not matter here; for real keys omit
  the option and enter it at clio's interactive prompt instead.
