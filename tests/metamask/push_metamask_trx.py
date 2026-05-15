#!/usr/bin/env python3
"""End-to-end Metamask-signed transaction test.

What this exercises:
    1. Stand up a single-node TestHarness cluster.
    2. Create a test account with K1 keys on its active permission.
    3. Append a Metamask-style EM pubkey to the active permission via
       sysio::expandauth.
    4. Seed the test account by transferring SYS from `sysio` (the bootstrap
       issues the full max_supply to `sysio`, so issuing more would exceed it).
    5. For each of two rounds, build an unsigned `sysio.token::transfer`,
       compute its sig_digest (sha256(chain_id || packed_trx || cfd_digest)),
       sign that digest, push it, and assert the sender balance dropped by
       exactly the transfer amount:
         Round 1 - POST the packed_transaction to /v1/chain/send_transaction2.
         Round 2 - clio convert unpack_transaction | clio push transaction
                    --signature SIG_EM_... -s.
    6. Signing has two modes:
         --simulate : clio's own EM tooling drives the signing automatically
                      (`clio create key --em` for the identity,
                      `clio convert em_sign` for the signature). This is what
                      the `metamask_trx_signing_test` ctest runs, so CI needs
                      no browser and no Python eth stack -- the only dependency
                      is the clio binary the build already produces.
         (default)  : print the digest and wait for the user to paste the
                      SIG_EM_... value from metamask-sign.html (real MetaMask).
                      Every pasted signature is recovered offline via
                      `clio convert em_recover` and checked against the EM key
                      registered on the account before anything reaches the
                      chain, so a stale or wrong paste is rejected locally with
                      a clear message instead of as an opaque
                      unsatisfied_authorization.

clio's EM path is libfc's own crypto -- the exact code nodeop runs to validate
these signatures -- and is pinned byte-for-byte against the Ethereum-ecosystem
reference (eth_account / MetaMask personal_sign) by the separate, hermetic
`clio_em_key_test` ctest. This harness exercises the full chain round-trip.

Run from your build directory (so $BUILD_DIR/programs/clio resolves):

    cd $BUILD_DIR
    python3 tests/metamask/push_metamask_trx.py --simulate
    python3 tests/metamask/push_metamask_trx.py            # manual (real MetaMask)

Use `--leave-running` to leave the cluster up after the test.
"""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # tests/ -- so `import TestHarness` resolves

# TestHarness shaping
from TestHarness import (  # noqa: E402
    Cluster,
    TestHelper,
    Utils,
    WalletMgr,
    createAccountKeys,
)
from TestHarness.TestHelper import AppArgs  # noqa: E402


Print = Utils.Print
errorExit = Utils.errorExit

# Mirrors the max_transaction_lifetime the TestHarness launcher configures
# (tests/TestHarness/launcher.py). A built transfer's expiration cannot exceed
# this, so an out-of-range --trx-expiration is rejected up front with a clear
# message rather than failing opaquely inside clio.
MAX_TRX_EXPIRATION_SECONDS = 3600

def _prompt(label: str) -> str:
    """input() that turns EOF / Ctrl-C into a clean TestHarness exit.

    Manual mode is interactive; if stdin is closed (piped, no TTY) a bare
    input() raises EOFError and dumps a traceback. Fail with a clear message
    instead so the cluster still shuts down via main()'s finally.
    """
    try:
        return input(label).strip()
    except (EOFError, KeyboardInterrupt):
        errorExit("aborted: no input on stdin (manual mode needs an interactive terminal)")


def _run_clio(node, *args, capture_json=True):
    """Run clio against `node` and return stdout (json-decoded if requested).

    A non-zero clio exit is fatal via errorExit, matching how the rest of this
    test reports failures (clean TestHarness shutdown, no raw traceback).
    """
    cmd = [Utils.SysClientPath, *node.sysClientArgs().split(), *args]
    if Utils.Debug:
        Print(f"cmd: {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        errorExit(f"clio failed: {' '.join(cmd)}\nstderr: {res.stderr}\nstdout: {res.stdout}")
    out = res.stdout.strip()
    if not capture_json:
        return out
    return json.loads(out) if out else {}


def _clio_offline(*args, expect_fail=False) -> dict[str, str] | None:
    """Run clio for an offline subcommand (create key / convert em_*); no node.

    These subcommands print `Label: value` lines and never touch a wallet or
    nodeop, so they take no node args. Returns the parsed lines as a dict, or
    None when `expect_fail` and clio exited non-zero (a malformed signature in
    the manual pre-flight, recovered locally rather than at the chain).
    """
    cmd = [Utils.SysClientPath, *args]
    if Utils.Debug:
        Print(f"cmd: {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        if expect_fail:
            return None
        errorExit(f"clio failed: {' '.join(cmd)}\nstderr: {res.stderr}\nstdout: {res.stdout}")
    return {k: v for k, v in re.findall(r'(\w[^:\n]*): ([^\n]+)', res.stdout)}


def _parse_asset(asset: str) -> tuple[int, int, str]:
    """Parse a Wire asset string ("100.0000 SYS") into (units, precision, symbol).

    `units` is the amount in integer minimal units (here 1000000 for the
    example) so balance math never touches floating point.
    """
    asset = (asset or "").strip()
    if not asset:
        raise ValueError("empty asset string")
    amount_str, _, symbol = asset.partition(" ")
    symbol = symbol.strip()
    if not symbol:
        raise ValueError(f"asset missing symbol: {asset!r}")
    whole, dot, frac = amount_str.partition(".")
    precision = len(frac) if dot else 0
    units = int(whole or "0") * (10 ** precision) + (int(frac) if frac else 0)
    return units, precision, symbol


def _assert_balance_decreased(label: str, before: str, after: str, expected_delta: str):
    """Assert `before - after == expected_delta` exactly (integer units, same symbol)."""
    b_units, _, b_sym = _parse_asset(before)
    a_units, _, a_sym = _parse_asset(after)
    d_units, _, d_sym = _parse_asset(expected_delta)
    if not (b_sym == a_sym == d_sym):
        errorExit(f"{label}: asset symbol mismatch: "
                  f"before={before!r} after={after!r} delta={expected_delta!r}")
    if b_units - a_units != d_units:
        errorExit(f"{label}: balance did not drop by {expected_delta}: "
                  f"before={before} after={after} "
                  f"(observed delta {b_units - a_units} minimal units, expected {d_units})")
    Print(f"{label}: balance {before} -> {after} (-{expected_delta}) OK")


def _create_em_key(simulate: bool,
                    sim_priv_arg: str | None) -> tuple[dict[str, str], str | None]:
    """Obtain the EM identity via clio (simulate) or by paste (manual).

    Returns (em_info, sim_priv_em). `em_info` always has exactly {wire_pub_em}.
    `sim_priv_em` is the PVT_EM_ to sign with in --simulate mode and None in
    manual mode (the private key never leaves the browser there). Keeping the
    shape identical across both modes avoids a branch-dependent KeyError.
    """
    if simulate:
        if sim_priv_arg:
            # Reproducible: a fixed PVT_EM_ or raw 0x Ethereum hex secret.
            r = _clio_offline("convert", "em_private_key",
                              "--private-key", sim_priv_arg, "--to-console")
        else:
            r = _clio_offline("create", "key", "--em", "--to-console")
        return {"wire_pub_em": r["Public key"]}, r["Private key"]

    Print("\nOpen tests/metamask/metamask-sign.html in a browser with Metamask")
    Print("installed. Click 'Connect Metamask' and then sign any test message so")
    Print("the page can ecRecover and surface your Wire PUB_EM_ pubkey.")
    Print("Paste it below (the per-signature pre-flight checks every signature")
    Print("recovers to THIS key before anything reaches the chain):\n")
    wire_pub_em = _prompt("Wire PUB_EM_ pubkey: ")
    if not wire_pub_em.startswith("PUB_EM_"):
        errorExit(f"pasted Wire pubkey is not a PUB_EM_ value: {wire_pub_em!r}")
    return {"wire_pub_em": wire_pub_em}, None


def _verify_sig_locally(digest_hex: str, sig_em: str,
                        expected_wire_pub: str) -> tuple[int, str]:
    """Offline pre-flight before pushing: recover the signer pubkey from
    (digest, sig) via `clio convert em_recover` -- the SAME libfc EIP-191 path
    nodeop applies -- and confirm it matches the EM key registered on the
    account.

    Returns (status, detail): 0 ok; 1 not a recoverable signature (clio could
    not recover, e.g. malformed paste); 2 recovered-key mismatch (wrong/stale
    digest signed). `detail` carries the recovered-vs-expected text so a bad
    paste is diagnosable HERE instead of as an opaque unsatisfied_authorization.
    """
    r = _clio_offline("convert", "em_recover", sig_em, digest_hex, expect_fail=True)
    if r is None or "Public key" not in r:
        return 1, "clio could not recover a public key from the pasted signature"
    recovered = r["Public key"]
    if recovered == expected_wire_pub:
        return 0, recovered
    return 2, (f"  recovered : {recovered}\n"
               f"  expected  : {expected_wire_pub}")


def _sign_with_metamask(simulate: bool, sim_priv_em: str | None,
                        digest_hex: str, expected_wire_pub: str) -> str:
    """Get a SIG_EM_... signature for `digest_hex` from clio (sim) or human paste.

    Manual mode runs an offline recovery pre-flight on the pasted signature before
    returning it: anything that does not recover to the account's registered EM
    key is rejected locally with a clear diagnostic and the user is re-prompted,
    rather than letting nodeop reject it later with an opaque
    unsatisfied_authorization.
    """
    if simulate:
        assert sim_priv_em, "simulator mode requires the simulator's PVT_EM_ key"
        # clio convert em_sign applies the same EIP-191 envelope nodeop recovers
        # with; pinned byte-for-byte to MetaMask by the clio_em_key_test ctest.
        return _clio_offline("convert", "em_sign", digest_hex,
                             "--private-key", sim_priv_em)["Signature"]

    Print("\n" + "=" * 72)
    Print("Paste THIS EXACT digest into the HTML page's 'Digest to sign' field:")
    Print(f"    {digest_hex}")
    Print("Click 'Sign with Metamask', then copy the SIG_EM_... value back here.")
    Print("(blank input aborts the test)")
    Print("=" * 72)
    while True:
        sig = _prompt("SIG_EM_...: ")
        if not sig:
            errorExit("aborted: no signature entered")
        rc, detail = _verify_sig_locally(digest_hex, sig, expected_wire_pub)
        if rc == 0:
            Print("pre-flight OK: signature recovers to the registered EM key; pushing")
            return sig
        Print(f"\n*** REJECTED locally (NOT sent to chain):\n{detail}\n")
        if rc == 2:
            Print("The signature recovers to a DIFFERENT key than the one registered.")
            Print("Almost always a stale signature from an earlier run, or the")
            Print("pubkey-reveal signature instead of this transaction's digest.")
            Print(f"The digest to sign is still: {digest_hex}")
            Print("Re-sign THAT exact digest in the browser and paste again.")
            Print("If this persists across a fresh re-sign, the PUB_EM_ you")
            Print("pasted earlier may not be this wallet's key.\n")
        else:  # rc == 1
            Print("That is not a recoverable signature. Paste the full")
            Print("SIG_EM_0x... value exactly as the page's 'Wire SIG_EM_' box")
            Print(f"shows it, signed over digest: {digest_hex}\n")


def _compute_sig_digest(chain_id_hex: str, packed_trx_hex: str) -> str:
    """Wire trx sig_digest: sha256(chain_id || packed_trx || cfd_digest).

    No context-free data: cfd_digest is 32 zero bytes.
    """
    chain_id = bytes.fromhex(chain_id_hex)
    if len(chain_id) != 32:
        raise ValueError(f"chain_id must be 32 bytes, got {len(chain_id)}")
    packed_trx = bytes.fromhex(packed_trx_hex)
    cfd_digest = b"\x00" * 32
    return "0x" + hashlib.sha256(chain_id + packed_trx + cfd_digest).hexdigest()


def _build_unsigned_transfer(node, sender: str, recipient: str, quantity: str,
                             memo: str, expiration_seconds: int) -> dict:
    """Use `clio push action -s -d --return-packed` to build an unsigned trx.

    Returns the {signatures, compression, packed_context_free_data, packed_trx}
    object from clio. Signatures is always [].

    `expiration_seconds` is baked into the packed bytes the signer hashes, so it
    cannot be changed after signing. Manual MetaMask signing has a human in the
    loop (open page, paste digest, approve in the extension, maybe re-sign after
    a pre-flight rejection); clio's 30s default expires long before that. The
    caller passes a generous window (still <= chain max_transaction_lifetime).
    """
    transfer_data = json.dumps({
        "from": sender, "to": recipient,
        "quantity": quantity, "memo": memo,
    })
    return _run_clio(
        node,
        "push", "action", "sysio.token", "transfer", transfer_data,
        "-p", f"{sender}@active",
        "-x", str(expiration_seconds),
        "-s", "-d", "-j", "--return-packed",
    )


def _sign_unsigned_packed(packed, chain_id_hex: str, simulate: bool,
                          sim_priv_em: str | None,
                          expected_wire_pub: str) -> tuple[str, str]:
    """Compute sig_digest and obtain a SIG_EM_... over it. Returns (sig_em, digest_hex).

    `expected_wire_pub` is the PUB_EM_ registered on the account; the manual path
    pre-flights the pasted signature against it before returning.
    """
    sig_digest = _compute_sig_digest(chain_id_hex, packed["packed_trx"])
    sig_em = _sign_with_metamask(simulate, sim_priv_em, sig_digest, expected_wire_pub)
    return sig_em, sig_digest


def _push_via_http(node, packed, sig_em: str) -> dict:
    """POST a packed_transaction directly to /v1/chain/send_transaction2."""
    return _post_send_transaction2(node, packed["packed_trx"], sig_em)


def _push_via_clio(node, packed, sig_em: str) -> dict:
    """Round-trip the packed trx through `clio convert unpack_transaction` to recover
    the JSON form, then hand to `clio push transaction --signature SIG_EM_... -s`.

    fc::raw::pack is deterministic, so the unpack -> repack on the send path yields
    bytes identical to those we used to compute the sig_digest. If that invariant
    ever breaks the test will fail with a recovered-key mismatch.

    `compression` is hardcoded "none": `clio push action -d --return-packed`
    returns an uncompressed packed_trx, and the digest was computed over those
    raw bytes. If clio ever returned a compressed form, repacking here would
    change the bytes and the test would fail loudly at the pre-flight / chain.
    """
    fake_packed = {
        "signatures": [],
        "compression": "none",
        "packed_context_free_data": "",
        "packed_trx": packed["packed_trx"],
    }
    unsigned_trx_obj = _run_clio(node, "convert", "unpack_transaction", json.dumps(fake_packed))
    return _run_clio(
        node,
        "push", "transaction", json.dumps(unsigned_trx_obj),
        "--signature", sig_em,
        "-s", "-j",
    )


def _post_send_transaction2(node, packed_trx_hex: str, sig_em: str) -> dict:
    """POST a packed_transaction directly to /v1/chain/send_transaction2.

    Sending the packed form preserves the exact bytes used to compute the
    sig_digest. Going through `clio push transaction` would re-pack from JSON
    and (in principle) could produce different bytes than the ones we hashed.

    Returns the decoded JSON reply, or a {"http_error": ...} / {"net_error": ...}
    marker so the caller's _assert_executed reports a clean message instead of a
    raw traceback for an HTTP-status error or a connection/timeout failure.
    """
    body = {
        "return_failure_trace": True,
        "retry_trx": False,
        "transaction": {
            "signatures": [sig_em],
            "compression": "none",
            "packed_context_free_data": "",
            "packed_trx": packed_trx_hex,
        },
    }
    url = f"http://{node.host}:{node.port}/v1/chain/send_transaction2"
    req = urllib.request.Request(
        url, data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        return {"http_error": exc.code, "body": exc.read().decode("utf-8")}
    except (urllib.error.URLError, TimeoutError) as exc:
        return {"net_error": str(getattr(exc, "reason", exc)), "url": url}


def _assert_executed(label: str, result: dict):
    """Success criterion shared by both push paths.

    send_transaction2 and `clio push transaction -j` return the same trace
    shape: success means processed.except is null and processed.error_code is
    null. The trx-level `receipt` can be null even on success, so it is not
    checked. http_error / net_error markers from _post_send_transaction2 are
    surfaced as clean failures.
    """
    if "http_error" in result:
        errorExit(f"{label}: HTTP error {result['http_error']}: {result['body']}")
    if "net_error" in result:
        errorExit(f"{label}: could not reach {result['url']}: {result['net_error']}")
    processed = result.get("processed", {})
    exc = processed.get("except")
    if exc:
        errorExit(f"{label}: trx raised: {exc.get('name')}: {exc.get('message')}")
    err_code = processed.get("error_code")
    if err_code:
        errorExit(f"{label}: trx error_code={err_code}")
    trx_id = result.get("transaction_id") or processed.get("id")
    block = processed.get("block_num")
    Print(f"{label}: executed, trx_id={trx_id}, block_num={block}")


def main() -> int:
    app_args = AppArgs()
    app_args.add_bool("--simulate",
                      help="Sign automatically via clio's EM tooling instead of asking the user to paste")
    app_args.add("--sim-private-key", type=str, default=None,
                 help="If --simulate, sign with this fixed key (PVT_EM_... or a raw 0x "
                      "Ethereum hex secret) instead of a freshly generated one")
    app_args.add("--transfer-amount", type=str, default="1.0000 SYS",
                 help="Asset string to transfer (default: '1.0000 SYS')")
    app_args.add("--trx-expiration", type=int, default=600,
                 help="Seconds before each built transfer expires. Manual signing "
                      "needs a wide window; default 600s, must be in "
                      f"(0, {MAX_TRX_EXPIRATION_SECONDS}] (chain max_transaction_lifetime).")
    args = TestHelper.parse_args({
        "--keep-logs", "--dump-error-details", "-v", "--leave-running", "--unshared",
        "--host", "--port",
    }, applicationSpecificArgs=app_args)

    if not 0 < args.trx_expiration <= MAX_TRX_EXPIRATION_SECONDS:
        errorExit(f"--trx-expiration must be in (0, {MAX_TRX_EXPIRATION_SECONDS}] "
                  f"seconds, got {args.trx_expiration}")

    Utils.Debug = args.v
    cluster = Cluster(
        host=args.host, port=args.port,
        unshared=args.unshared,
        keepRunning=args.leave_running, keepLogs=args.keep_logs,
    )
    wallet_mgr = WalletMgr(True, keepRunning=args.leave_running, keepLogs=args.keep_logs)
    cluster.setWalletMgr(wallet_mgr)

    test_ok = False
    try:
        TestHelper.printSystemInfo("BEGIN metamask trx signing test")

        Print("launching single-node cluster")
        if not cluster.launch(pnodes=1, totalNodes=1, prodCount=1, dontBootstrap=False):
            errorExit("cluster launch failed")
        node = cluster.getNode(0)

        Print("creating wallet and test account")
        wallet = wallet_mgr.create("mm_test")

        # Import the bootstrap accounts we need to sign as:
        #   sysio  -> calls expandauth, issue/transfer of test tokens.
        #   defproducera -> creator of the test account.
        defproducera = cluster.defproduceraAccount
        sysio_account = cluster.sysioAccount
        wallet_mgr.importKey(defproducera, wallet, ignoreDupKeyWarning=True)
        wallet_mgr.importKey(sysio_account, wallet, ignoreDupKeyWarning=True)

        test = createAccountKeys(1)[0]
        test.name = "mmtest11"
        wallet_mgr.importKey(test, wallet, ignoreDupKeyWarning=True)

        Print(f"creating account {test.name}")
        # Wire post-ROA: new accounts created by sysio with carl as nodeOwner.
        trans = cluster.createAccountAndVerify(
            test, cluster.sysioAccount,
            nodeOwner=cluster.carlAccount,
            stakedDeposit=0, buyRAM=200000,
        )
        if not trans:
            errorExit("createAccountAndVerify failed")

        Print("setting up Metamask-style EM key on the test account")
        em_info, sim_priv_em = _create_em_key(args.simulate, args.sim_private_key)
        Print(json.dumps(em_info, indent=2))

        Print("calling sysio::expandauth to add the EM key to active permission")
        expand_action_data = {
            "account": test.name,
            "permission": "active",
            "keys": [{"key": em_info["wire_pub_em"], "weight": 1}],
            "accounts": [],
        }
        success, _ = node.pushMessage("sysio", "expandauth",
                                      json.dumps(expand_action_data),
                                      opts="-p sysio@active")
        if not success:
            errorExit("expandauth failed")

        # The bootstrap issues the full max_supply to `sysio`, so just transfer
        # from sysio to the test account. Issuing more would exceed max_supply.
        seed_qty = "100.0000 SYS"
        Print(f"transferring {seed_qty} from sysio to {test.name}")
        seed_ok, _ = node.pushMessage("sysio.token", "transfer",
                                      json.dumps({"from": "sysio", "to": test.name,
                                                  "quantity": seed_qty, "memo": "seed"}),
                                      opts="-p sysio@active", silentErrors=False)
        if not seed_ok:
            errorExit(f"seed transfer of {seed_qty} from sysio to {test.name} failed")

        info = _run_clio(node, "get", "info")
        chain_id_hex = info["chain_id"]
        Print(f"chain_id  : {chain_id_hex}")

        balance_initial = node.getCurrencyBalance("sysio.token", test.name)
        Print(f"balance before any transfer: {balance_initial}")

        recipient = defproducera.name

        # --- Round 1: push via HTTP /v1/chain/send_transaction2 ---
        Print("\n=== Round 1: Metamask-signed transfer via HTTP send_transaction2 ===")
        packed_http = _build_unsigned_transfer(node, test.name, recipient,
                                               args.transfer_amount,
                                               "metamask http transfer",
                                               args.trx_expiration)
        sig_em_http, digest_http = _sign_unsigned_packed(
            packed_http, chain_id_hex, args.simulate, sim_priv_em,
            em_info["wire_pub_em"])
        Print(f"sig_digest: {digest_http}")
        Print(f"signature : {sig_em_http}")
        http_result = _push_via_http(node, packed_http, sig_em_http)
        _assert_executed("HTTP path", http_result)
        balance_after_http = node.getCurrencyBalance("sysio.token", test.name)
        _assert_balance_decreased("HTTP path", balance_initial, balance_after_http,
                                  args.transfer_amount)

        # --- Round 2: push via clio push transaction --signature ---
        Print("\n=== Round 2: Metamask-signed transfer via clio push transaction ===")
        packed_clio = _build_unsigned_transfer(node, test.name, recipient,
                                               args.transfer_amount,
                                               "metamask clio transfer",
                                               args.trx_expiration)
        sig_em_clio, digest_clio = _sign_unsigned_packed(
            packed_clio, chain_id_hex, args.simulate, sim_priv_em,
            em_info["wire_pub_em"])
        Print(f"sig_digest: {digest_clio}")
        Print(f"signature : {sig_em_clio}")
        clio_result = _push_via_clio(node, packed_clio, sig_em_clio)
        _assert_executed("clio path", clio_result)
        balance_after_clio = node.getCurrencyBalance("sysio.token", test.name)
        _assert_balance_decreased("clio path", balance_after_http, balance_after_clio,
                                  args.transfer_amount)

        Print("\nOK: Metamask-signed transactions executed via BOTH HTTP and clio paths")
        test_ok = True
        return 0
    finally:
        TestHelper.shutdown(
            cluster, wallet_mgr,
            testSuccessful=test_ok,
            dumpErrorDetails=args.dump_error_details,
        )


if __name__ == "__main__":
    sys.exit(main())
