# Metamask transaction-signing harness

End-to-end test that a Wire transaction signed by **MetaMask** (or any wallet that
exposes EIP-191 `personal_sign` over secp256k1) is accepted by `nodeop`.

Wire already supports this in C++ -- `signature_shim::recover()` applies the
EIP-191 prefix to a `sha256` transaction digest and recovers an `em::public_key`,
and the `expandauth_sign_with_eth_key` unit test proves the full in-process flow.
This directory adds the **external-signer** half: real MetaMask in a browser,
signing a real trx that gets pushed to nodeop.


## What's here

| File | Role |
| ---- | ---- |
| `metamask-sign.html` | Static page that talks to MetaMask via `window.ethereum`. Calls `personal_sign` on a 32-byte digest, locally ecRecovers the public key, and displays both raw hex (Metamask format) and Wire's `SIG_EM_` / `PUB_EM_` forms ready to paste into the push helper. |
| `em_sig_to_wire.py` | Bidirectional converter between Metamask `0x...` hex and Wire `SIG_EM_` / `PUB_EM_` forms. (Unlike K1/R1/BLS keys, Wire's EM form is plain hex, not base58 + ripemd160: `SIG_EM_0x<130hex>` and `PUB_EM_<130hex>`.) Importable; also runs as a CLI. |
| `verify_em_sig.py` | Offline check: given `(digest, signature, expected-address-or-pub)`, recover the pubkey using the same EIP-191 envelope nodeop applies, and assert it matches. Uses `eth_account`. |
| `metamask_sim.py` | Headless substitute for MetaMask. Generates an EM key and signs an arbitrary digest the same way `personal_sign` would. Used by automated test runs. |
| `push_metamask_trx.py` | TestHarness-driven end-to-end test. Stands up a single-node cluster, creates an account, calls `expandauth` to add the EM key, seeds it with SYS transferred from `sysio`, builds two unsigned `sysio.token::transfer` trxs, signs each digest (via simulator or pasted-from-browser), and pushes one via HTTP `send_transaction2` and one via `clio push transaction`. |


## Python deps

The eth\_account stack is required. From repo root:

```bash
.venv/bin/python -m pip install eth-account eth-keys eth-utils pycryptodome
```

(or rely on the project's `requirements.txt` install.)

The manual-path pre-flight and the simulator run as subprocesses that import
the eth_account stack. Invoking the harness with `.venv/bin/python` is the
simplest path. If you launch it with a bare `python3` that lacks those deps,
the harness auto-detects this and falls back to `$VIRTUAL_ENV` or the first
`.venv` found walking up from the script (so it works from the source tree or
the build-dir copy); it errors early, before launching the cluster, only if no
suitable interpreter exists.


## Quick offline sanity check

```bash
.venv/bin/python tests/metamask/metamask_sim.py keygen > /tmp/k.json
PRIV=$(jq -r .private_key /tmp/k.json)
WIRE_PUB=$(jq -r .wire_pub_em /tmp/k.json)
ADDR=$(jq -r .eth_address /tmp/k.json)

SIG=$(.venv/bin/python tests/metamask/metamask_sim.py sign \
        --private-key "$PRIV" --digest 0xdeadbeef --raw)

.venv/bin/python tests/metamask/verify_em_sig.py \
    --digest 0xdeadbeef \
    --signature "$SIG" \
    --expect-addr "$ADDR" \
    --expect-pub  "$WIRE_PUB"
```

Exit 0 means the EIP-191 envelope, the secp256k1 recovery, and Wire's plain-hex
`SIG_EM_` / `PUB_EM_` framing all round-trip.


## Full end-to-end (automated)

This is the path `ctest -R metamask_trx_signing_test` runs.

```bash
cd $BUILD_DIR
.venv/bin/python tests/metamask/push_metamask_trx.py --simulate -v
```

The script:

1. Launches a single-node cluster via TestHarness.
2. Creates `mmtest11`, gives it K1 keys.
3. Generates an EM keypair in the simulator and adds the `PUB_EM_` to
   `mmtest11@active` via `sysio::expandauth`.
4. Transfers `100.0000 SYS` from `sysio` to `mmtest11` (bootstrap issued the
   full max_supply to `sysio`, so just transfer instead of `issue`).
5. **Round 1 - HTTP path:** builds an unsigned `sysio.token::transfer
   mmtest11 -> defproducera` via `clio push action -s -d --return-packed -j`,
   computes `sig_digest = sha256(chain_id || packed_trx || zero_32)`, signs
   via the simulator, and `POST`s the packed_transaction directly to
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
.venv/bin/python tests/metamask/push_metamask_trx.py -v
```

Open `metamask-sign.html` in a browser with MetaMask. If the browser can open
the local file directly, that works with no server. Under WSL (browser on the
Windows side) serve it instead, e.g. from `tests/metamask/`:
`python3 -m http.server 8866 --bind 0.0.0.0`, then open
`http://localhost:8866/metamask-sign.html`. The script will:

1. Spin up the cluster as above.
2. Prompt for your ETH address + uncompressed pubkey + Wire `PUB_EM_` --
   the HTML page shows all three once you sign any test message. The three
   pasted values are cross-checked offline for mutual consistency before use.
3. Add the `PUB_EM_` to the test account via `expandauth`.
4. Print the trx `sig_digest`. Paste that into the HTML page's "Digest to sign"
   field, click "Sign with Metamask", copy the `SIG_EM_` value.
5. Paste the `SIG_EM_` back into the script. It is recovered offline and
   checked against the registered `PUB_EM_` before being sent; a stale or
   wrong-digest signature is rejected locally and re-prompted.
6. Push, check execution. Repeated for Round 2 (different digest).


## Notes / gotchas

- MetaMask's `personal_sign` accepts the message either as a UTF-8 string or as
  `0x`-prefixed hex. **Always pass `0x` + 64 hex chars** for a 32-byte digest:
  if you pass a UTF-8 string by mistake, MetaMask prefix-wraps the UTF-8 bytes
  of the *string*, not the underlying digest, and the chain rejects the
  signature.
- EIP-191 v-byte is `27` or `28` (recovery_id + 27). Some wallets emit raw `0/1`.
  Both `metamask_sig_to_wire` and `verify_em_sig` normalize to `27/28` before
  encoding.
- ECDSA signatures must be canonical (`s < n/2`). MetaMask and `eth_account`
  produce canonical signatures, but ad-hoc third-party signers may not; Wire's
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
