#!/usr/bin/env python3

"""clio_set_syscode_test.py -- integration test for `clio system setcode` / `system setabi`.

These two clio subcommands deploy code/abi to a system-contract account *through* sysio.roa
(rather than the chain's native setcode/setabi): the privileged roa contract performs the inline
setcode/setabi, flips the target privileged, and gifts exactly the RAM the code/abi consumes out
of sysio's pool. The outer sysio.roa::setsyscode/setsysabi action is authorized by sysio@active.

This drives the real commands end-to-end against a live chain. The action-data packing and the
sysio@active default are covered hermetically elsewhere (the packing round-trips through
`clio convert pack_action_data --abi-file`, and the chain-side RAM-gift accounting is covered by
contracts/tests/sysio.roa_tests.cpp). What only a live run can prove is exercised here: that
`clio system setcode <acct> <wasm>` assembles a transaction the chain accepts and applies.

Flow:
  1. Launch a cluster with sysio.roa active (loadSystemContract=False, activateIF=True).
  2. Create a ROA-managed (finite-RAM) target account via sysio.roa::newnameduser -- setsyscode's
     giftram step rejects an unlimited-RAM target, so the account must carry a finite quota first.
  3. `clio system setcode <tgt> sysio.token.wasm -p sysio@active`  -> code deployed + account privileged.
  4. `clio system setabi  <tgt> sysio.token.abi  -p sysio@active`  -> abi deployed.

The privileged flag is the tell that the roa path (not a plain setcode) ran: native `set code`
never flips privileged, but sysio.roa::setsyscode always does.
"""

import json
import os
import urllib.request

from TestHarness import Cluster, ReturnType, TestHelper, Utils, WalletMgr

Print = Utils.Print

ZERO_CODE_HASH = "0" * 64


# ---------------------------------------------------------------------------
#  helpers
# ---------------------------------------------------------------------------

def wallet_create_key(host, port, key_type, wallet_name="ignition"):
    """Create a key of the given type (K1 / EM / ...) in the kiod wallet; return the public key."""
    url = f"http://{host}:{port}/v1/wallet/create_key"
    body = json.dumps([wallet_name, key_type]).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    try:
        resp = urllib.request.urlopen(req)
        return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"kiod create_key({key_type}) failed ({e.code}): {e.read().decode()}") from e


def push_newnameduser(node, account, wire_key, tier):
    """Create an account the way the depot does (vanity name) -- signed as sysio.roa."""
    data = json.dumps({"account": account, "pubkey": wire_key, "tier": tier})
    return node.pushMessage("sysio.roa", "newnameduser", data, "--permission sysio.roa@active")


def push_forcereg(node, owner, tier):
    """Run the ROA tier allocator on an existing account, giving it a *finite* RAM limit.

    This is the privileged bootstrap/test registration path (sysio.roa::forcereg -> regnodeowner),
    the same one contracts/tests/sysio.roa_tests.cpp uses to prepare a setsyscode target. It is
    required because under loadSystemContract=False, sysio runs bios (not sysio.system), so the
    account created by newnameduser keeps an unlimited (-1) RAM limit -- and setsyscode's giftram
    step rejects an unlimited target. forcereg's set_resource_limits gives it a finite limit; giftram
    then tops it up by exactly the code/abi RAM out of sysio's pool.
    """
    data = json.dumps({"owner": owner, "tier": tier})
    return node.pushMessage("sysio.roa", "forcereg", data, "--permission sysio.roa@active")


def get_code_hash(node, account):
    """Parse `clio get code` -> 'code hash: <hash>'. Returns the 64-char hex hash (zeros if no code)."""
    out = node.processClioCmd(f"get code {account}", "get code", silentErrors=False, returnType=ReturnType.raw)
    return out.split(' ')[2].strip() if out else None


def is_privileged(node, account):
    """`clio get account` prints 'privileged: true' only when the account is privileged."""
    out = node.processClioCmd(f"get account {account}", "get account", silentErrors=False, returnType=ReturnType.raw)
    return out is not None and "privileged: true" in out


# ---------------------------------------------------------------------------
#  test
# ---------------------------------------------------------------------------

args = TestHelper.parse_args({"-v", "--dump-error-details", "--leave-running", "--keep-logs", "--unshared"})
Utils.Debug = args.v
dumpErrorDetails = args.dump_error_details

cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True)
cluster.setWalletMgr(walletMgr)

testSuccessful = False
try:
    TestHelper.printSystemInfo("BEGIN")

    # Bootstraps sysio.roa (active) + the sysio key in the wallet (needed to sign sysio@active).
    assert cluster.launch(
        pnodes=1,
        prodCount=1,
        totalProducers=1,
        totalNodes=2,
        loadSystemContract=False,
        activateIF=True,
    ), "Failed to launch cluster"

    node = cluster.getNode(0)

    wasm = os.path.abspath(os.path.join(os.getcwd(), "contracts", "sysio.token", "sysio.token.wasm"))
    abi = os.path.abspath(os.path.join(os.getcwd(), "contracts", "sysio.token", "sysio.token.abi"))
    assert os.path.exists(wasm), f"missing test wasm: {wasm}"
    assert os.path.exists(abi), f"missing test abi: {abi}"

    # ---- create a target account, then give it a finite ROA quota ----
    Print("=== creating finite-RAM target account (newnameduser + forcereg) ===")
    wire_key = wallet_create_key(walletMgr.host, walletMgr.port, "K1")
    target = "codetarget1"
    assert push_newnameduser(node, target, wire_key, 2)[0], "newnameduser failed"
    node.waitForNextBlock()
    # newnameduser only creates the account; forcereg gives it the finite RAM limit setsyscode needs.
    assert push_forcereg(node, target, 1)[0], "forcereg failed"
    node.waitForNextBlock()
    assert get_code_hash(node, target) == ZERO_CODE_HASH, "precondition: target must start with no code"
    assert not is_privileged(node, target), "precondition: target must start unprivileged"

    # ---- Test 1: system setcode deploys code, makes the account privileged ----
    Print("=== Test 1: clio system setcode ===")
    node.processClioCmd(f"system setcode {target} {wasm} --permission sysio@active",
                        "system setcode", silentErrors=False, exitOnError=True, returnType=ReturnType.raw)
    node.waitForNextBlock()
    code_hash = get_code_hash(node, target)
    assert code_hash != ZERO_CODE_HASH, f"system setcode did not deploy code (hash still {code_hash})"
    # Native `set code` never sets privileged; sysio.roa::setsyscode does. This proves the roa path ran.
    assert is_privileged(node, target), "system setcode did not make the account privileged (roa setpriv path)"
    Print(f"  Verified: code deployed (hash {code_hash}) and account privileged")

    # ---- Test 2: system setabi deploys the abi ----
    Print("=== Test 2: clio system setabi ===")
    node.processClioCmd(f"system setabi {target} {abi} --permission sysio@active",
                        "system setabi", silentErrors=False, exitOnError=True, returnType=ReturnType.raw)
    node.waitForNextBlock()
    abi_out = node.processClioCmd(f"get abi {target}", "get abi", silentErrors=False, returnType=ReturnType.raw)
    assert abi_out is not None and "transfer" in abi_out, "system setabi did not deploy the token abi"
    Print("  Verified: abi deployed (contains the token 'transfer' action)")

    # ---- Test 2b: an identical-byte redeploy must NOT be skipped on the ROA path ----
    # Native `set code`/`set abi` skip a byte-identical redeploy, but sysio.roa::setsyscode/setsysabi
    # also flip the target privileged and reconcile gifted RAM -- so the action must still execute even
    # when the bytes match. The target currently holds exactly `wasm`/`abi` (from Tests 1-2), so both
    # re-runs are exact duplicates. `-j` prints the pushed transaction to stdout; a duplicate-skip would
    # emit nothing there. Regression guard for the duplicate-check bypass (huangminghuang review).
    Print("=== Test 2b: identical-byte system setcode/setabi still execute (no duplicate skip) ===")
    dup_code = node.processClioCmd(f"system setcode {target} {wasm} -j --permission sysio@active",
                                   "system setcode duplicate", silentErrors=False, exitOnError=True,
                                   returnType=ReturnType.raw)
    assert dup_code and ("transaction_id" in dup_code or "processed" in dup_code), \
        "system setcode skipped an identical-byte redeploy; ROA setpriv/RAM-reconcile would be missed"
    node.waitForNextBlock()
    dup_abi = node.processClioCmd(f"system setabi {target} {abi} -j --permission sysio@active",
                                  "system setabi duplicate", silentErrors=False, exitOnError=True,
                                  returnType=ReturnType.raw)
    assert dup_abi and ("transaction_id" in dup_abi or "processed" in dup_abi), \
        "system setabi skipped an identical-byte redeploy; ROA RAM-reconcile would be missed"
    node.waitForNextBlock()
    assert is_privileged(node, target), "target lost privileged after identical-byte ROA redeploy"
    Print("  Verified: ROA setcode/setabi re-emit the action on identical bytes (not skipped)")

    # ---- Test 3: re-deploy a smaller contract via system setcode (redeploy path) ----
    # setsyscode is bidirectional: redeploying reclaims/gifts the RAM delta. A second deploy must
    # succeed and replace the code (the bios contract is a different, valid WASM than the token).
    Print("=== Test 3: clio system setcode redeploy (different code) ===")
    bios_wasm = os.path.abspath(os.path.join(os.getcwd(), "libraries", "testing", "contracts",
                                             "sysio.bios", "sysio.bios.wasm"))
    if os.path.exists(bios_wasm):
        node.processClioCmd(f"system setcode {target} {bios_wasm} --permission sysio@active",
                            "system setcode redeploy", silentErrors=False, exitOnError=True, returnType=ReturnType.raw)
        node.waitForNextBlock()
        new_hash = get_code_hash(node, target)
        assert new_hash != ZERO_CODE_HASH and new_hash != code_hash, \
            f"redeploy did not replace the code (hash still {new_hash})"
        Print(f"  Verified: redeploy replaced code (new hash {new_hash})")
    else:
        Print(f"  SKIP redeploy: bios wasm not found at {bios_wasm}")

    testSuccessful = True
    Print("=== All clio system setcode / system setabi integration tests passed ===")

finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

exit(0 if testSuccessful else 1)
