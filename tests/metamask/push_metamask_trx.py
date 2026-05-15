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
         --simulate : metamask_sim.py drives the signing automatically. This is
                      what the `metamask_trx_signing_test` ctest runs, so CI
                      needs no browser.
         (default)  : print the digest and wait for the user to paste the
                      SIG_EM_... value from metamask-sign.html. The pasted
                      identity and every signature are verified offline before
                      anything reaches the chain, so a stale or wrong paste is
                      rejected locally with a clear message instead of as an
                      opaque unsatisfied_authorization.

Run from your build directory (so $BUILD_DIR/programs/clio resolves):

    cd $BUILD_DIR
    .venv/bin/python tests/metamask/push_metamask_trx.py --simulate
    .venv/bin/python tests/metamask/push_metamask_trx.py            # manual

Use `--leave-running` to leave the cluster up after the test.
"""

from __future__ import annotations

import functools
import hashlib
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))

from em_sig_to_wire import (  # noqa: E402
    eth_address_from_pub,
    metamask_pub_to_wire,
    wire_pub_to_metamask,
)

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

# metamask_sim.py (simulate) and verify_em_sig.py (manual pre-flight) run as
# subprocesses and import these. Probed once at startup against a chosen
# interpreter so a deps-less launcher fails fast with guidance instead of
# rejecting every signature deep in the manual flow.
_REQUIRED_DEPS = ("eth_account", "eth_keys", "eth_utils")


@functools.lru_cache(maxsize=1)
def _dep_python() -> str:
    """Return a Python interpreter that can import the eth_account stack.

    Relying on sys.executable breaks when the harness is launched with a bare
    `python3` that lacks the deps: the pre-flight subprocess then fails with an
    opaque "No module named 'eth_account'" and the manual loop rejects every
    paste forever. Prefer the launching interpreter if it already has the deps;
    otherwise fall back to $VIRTUAL_ENV or the first `.venv` found walking up
    from this script (works whether run from the source tree or the build-dir
    copy). errorExit with actionable guidance if nothing qualifies.
    """
    probe = "import " + ", ".join(_REQUIRED_DEPS)

    def has_deps(py: str) -> bool:
        try:
            return subprocess.run([py, "-c", probe],
                                  capture_output=True, text=True).returncode == 0
        except OSError:
            return False

    if has_deps(sys.executable):
        return sys.executable

    candidates: list[str] = []
    venv_env = os.environ.get("VIRTUAL_ENV")
    if venv_env:
        candidates.append(str(Path(venv_env) / "bin" / "python"))
    for parent in [HERE, *HERE.parents]:
        cand = parent / ".venv" / "bin" / "python"
        if cand.is_file():
            candidates.append(str(cand))

    tried: list[str] = []
    for cand in candidates:
        if cand in tried:
            continue
        tried.append(cand)
        if has_deps(cand):
            return cand

    errorExit(
        "no Python with the eth_account stack "
        f"({', '.join(_REQUIRED_DEPS)}) could be found.\n"
        f"  launched with : {sys.executable}\n"
        f"  also tried    : {tried or '(none)'}\n"
        "  fix: run the harness with the repo virtualenv, e.g.\n"
        "       <wire-sysio>/.venv/bin/python tests/metamask/push_metamask_trx.py ...\n"
        "  or:  <that venv>/bin/python -m pip install "
        "eth-account eth-keys eth-utils pycryptodome")


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


def _validate_pasted_identity(em_info: dict[str, str]):
    """Offline check of the three values pasted from metamask-sign.html.

    The signature gets a recovery pre-flight; the identity it must recover to
    deserves the same treatment. Confirm the pasted uncompressed pubkey, the
    pasted Ethereum address, and the pasted PUB_EM_ are mutually consistent so
    a typo fails here with a clear message instead of opaquely at expandauth.
    """
    eth_address = em_info["eth_address"]
    pub_hex = em_info["uncompressed_pub_hex"]
    wire_pub_em = em_info["wire_pub_em"]
    if not wire_pub_em.startswith("PUB_EM_"):
        errorExit(f"pasted Wire pubkey is not a PUB_EM_ value: {wire_pub_em!r}")
    try:
        # wire_pub_to_metamask validates the PUB_EM_ framing/length/0x04 prefix.
        wire_pub_to_metamask(wire_pub_em)
        derived_wire = metamask_pub_to_wire(pub_hex)
        derived_addr = eth_address_from_pub(pub_hex)
    except Exception as exc:  # noqa: BLE001
        errorExit(f"pasted identity is malformed: {exc}")
    if derived_wire.lower() != wire_pub_em.lower():
        errorExit("pasted uncompressed pubkey does not match the pasted PUB_EM_:\n"
                  f"  from pubkey : {derived_wire}\n"
                  f"  pasted PUB  : {wire_pub_em}")
    if derived_addr.lower() != eth_address.lower():
        errorExit("pasted ETH address does not match the pasted pubkey:\n"
                  f"  from pubkey : {derived_addr}\n"
                  f"  pasted addr : {eth_address}")


def _create_em_key(simulate: bool,
                    private_key_hex: str | None) -> tuple[dict[str, str], str | None]:
    """Obtain the EM identity, either from metamask_sim keygen or by paste.

    Returns (em_info, sim_private_key). `em_info` always has exactly
    {eth_address, uncompressed_pub_hex, wire_pub_em}; `sim_private_key` is the
    simulator's hex private key in --simulate mode (used to sign later) and
    None in manual mode. Keeping the shape identical across both modes avoids
    the previous branch-dependent KeyError footgun.
    """
    if simulate:
        cmd = [_dep_python(), str(HERE / "metamask_sim.py"), "keygen"]
        if private_key_hex:
            cmd += ["--private-key", private_key_hex]
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        raw = json.loads(res.stdout)
        em_info = {
            "eth_address": raw["eth_address"],
            "uncompressed_pub_hex": raw["uncompressed_pub_hex"],
            "wire_pub_em": raw["wire_pub_em"],
        }
        return em_info, raw["private_key"]

    Print("\nOpen tests/metamask/metamask-sign.html in a browser with Metamask")
    Print("installed. Click 'Connect Metamask' and then sign any test message so")
    Print("the page can ecRecover and surface your uncompressed pubkey.")
    Print("Paste the values shown on the page below:\n")
    em_info = {
        "eth_address": _prompt("ETH address (0x...): "),
        "uncompressed_pub_hex": _prompt("uncompressed pubkey (0x04...): "),
        "wire_pub_em": _prompt("Wire PUB_EM_ pubkey: "),
    }
    _validate_pasted_identity(em_info)
    Print("pasted identity is self-consistent (pubkey <-> address <-> PUB_EM_)")
    return em_info, None


def _verify_sig_locally(digest_hex: str, sig_em: str,
                        expected_wire_pub: str) -> tuple[int, str]:
    """Offline pre-flight before pushing: recover the signer pubkey from
    (digest, sig) using the SAME EIP-191 envelope nodeop applies, and confirm it
    matches the EM key registered on the account.

    Delegates to verify_em_sig.py (its documented CLI) rather than reaching into
    its internals, and mirrors how metamask_sim.py is driven elsewhere here.

    Returns (returncode, detail) where returncode follows verify_em_sig.py's
    contract: 0 ok, 1 recovery failed (the pasted text is not a recoverable
    signature), 2 recovered-key mismatch (wrong/stale digest signed), 3 CLI
    error. `detail` carries its recovered-vs-expected output so a bad paste is
    diagnosable HERE instead of as an opaque unsatisfied_authorization.
    """
    cmd = [
        _dep_python(), str(HERE / "verify_em_sig.py"),
        "--digest", digest_hex,
        "--signature", sig_em,
        "--expect-pub", expected_wire_pub,
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    parts = [res.stdout.strip(), res.stderr.strip()]
    detail = "\n".join(p for p in parts if p)
    return res.returncode, detail


def _sign_with_metamask(simulate: bool, private_key_hex: str | None,
                        digest_hex: str, expected_wire_pub: str) -> str:
    """Get a SIG_EM_... signature for `digest_hex` either from sim or from human paste.

    Manual mode runs an offline recovery pre-flight on the pasted signature before
    returning it: anything that does not recover to the account's registered EM
    key is rejected locally with a clear diagnostic and the user is re-prompted,
    rather than letting nodeop reject it later with an opaque
    unsatisfied_authorization. (The simulate path is unchanged.)
    """
    if simulate:
        assert private_key_hex, "simulator mode requires the simulator's private key"
        cmd = [
            _dep_python(), str(HERE / "metamask_sim.py"),
            "sign",
            "--private-key", private_key_hex,
            "--digest", digest_hex,
            "--raw",
        ]
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return res.stdout.strip()

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
        # A missing dependency / CLI failure is not something re-pasting can fix;
        # fail loudly rather than spinning the prompt forever. (_dep_python makes
        # the missing-module case unreachable, but guard it regardless.)
        if "No module named" in detail or rc not in (1, 2):
            errorExit(f"pre-flight could not run (verify_em_sig exit {rc}); "
                      f"this is an environment problem, not a bad signature:\n{detail}")
        Print(f"\n*** REJECTED locally (NOT sent to chain):\n{detail}\n")
        if rc == 2:
            Print("The signature recovers to a DIFFERENT key than the one registered.")
            Print("Almost always a stale signature from an earlier run, or the")
            Print("pubkey-reveal signature instead of this transaction's digest.")
            Print(f"The digest to sign is still: {digest_hex}")
            Print("Re-sign THAT exact digest in the browser and paste again.\n")
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
                          sim_private_key: str | None,
                          expected_wire_pub: str) -> tuple[str, str]:
    """Compute sig_digest and obtain a SIG_EM_... over it. Returns (sig_em, digest_hex).

    `expected_wire_pub` is the PUB_EM_ registered on the account; the manual path
    pre-flights the pasted signature against it before returning.
    """
    sig_digest = _compute_sig_digest(chain_id_hex, packed["packed_trx"])
    sig_em = _sign_with_metamask(simulate, sim_private_key, sig_digest, expected_wire_pub)
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
                      help="Drive metamask_sim.py to sign instead of asking the user to paste")
    app_args.add("--sim-private-key", type=str, default=None,
                 help="If --simulate, use this hex private key instead of a fresh one")
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

    # Resolve the eth_account interpreter up front: a deps-less launcher errors
    # here in ~1s with guidance, instead of after a multi-minute cluster launch
    # and (manual mode) a paste that the pre-flight could never accept.
    dep_py = _dep_python()
    if dep_py != sys.executable:
        Print(f"note: {sys.executable} lacks the eth_account stack; "
              f"using {dep_py} for sim / pre-flight subprocesses")

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
        em_info, sim_private_key = _create_em_key(args.simulate, args.sim_private_key)
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
            packed_http, chain_id_hex, args.simulate, sim_private_key,
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
            packed_clio, chain_id_hex, args.simulate, sim_private_key,
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
