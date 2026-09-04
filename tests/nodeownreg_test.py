#!/usr/bin/env python3

"""
nodeownreg_test.py — OPP Node Owner NFT Registration integration test (create-in-flow).

Drives sysio.roa the way the depot (sysio.msgch) does for an inbound NodeOwnerRegistration,
pushing the two inline actions directly as sysio.roa:
  1. newnameduser(account, wire_key, tier)        -- create the claim account
  2. nodeownreg(account, tier, eth_key, wire_key[, eth_address])
                                                   -- register + inline-record the ETH link

Verifies the nodeowners + nodeownerreg tables, plus the soft-fail (audit row) and hard-fail paths.
The sysio.authex.active <- sysio.roa@sysio.code delegation that the inline recordlink needs is wired
by the bios bootstrap (Cluster.py).
"""

import json
import re
import subprocess
import urllib.request

from TestHarness import Account, Cluster, TestHelper, Utils, WalletMgr

# nodeownerreg reg_status + reject_reason values (mirror sysio.roa.hpp).
CONFIRMED, REJECTED = 0, 1
R_NAME_INVALID, R_OWNER_NOT_ACCOUNT, R_ACCOUNT_KEY_MISMATCH, R_DUPLICATE = 1, 2, 3, 4


# ---------------------------------------------------------------------------
#  kiod wallet HTTP helper
# ---------------------------------------------------------------------------

def wallet_create_key(host, port, key_type, wallet_name="ignition"):
    """Create a key of the given type (EM / K1 / ...) in the kiod wallet; return the public key."""
    url = f"http://{host}:{port}/v1/wallet/create_key"
    body = json.dumps([wallet_name, key_type]).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    try:
        resp = urllib.request.urlopen(req)
        return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"kiod create_key({key_type}) failed ({e.code}): {e.read().decode()}") from e


# ---------------------------------------------------------------------------
#  sysio.roa action pushers (signed as sysio.roa, as the depot would inline-send)
# ---------------------------------------------------------------------------

def push_newnameduser(node, account, wire_key, tier, silent=False):
    data = json.dumps({"account": account, "pubkey": wire_key, "tier": tier})
    return node.pushMessage("sysio.roa", "newnameduser", data,
                            "--permission sysio.roa@active", silentErrors=silent)


def ethereum_address(public_key):
    """Derive the canonical 20-byte address from a PUB_EM key with clio's offline converter."""
    assert public_key.startswith("PUB_EM_"), f"expected PUB_EM key, got {public_key}"
    result = subprocess.run(
        [Utils.SysClientPath, "--no-auto-kiod", "convert", "eth_to_k1_public",
         public_key.removeprefix("PUB_EM_")],
        check=True, capture_output=True, text=True,
    )
    match = re.search(r"Ethereum address:\s*0x([0-9a-fA-F]{40})", result.stdout)
    assert match is not None, f"clio did not return an Ethereum address: {result.stdout}"
    return match.group(1).lower()


def push_nodeownreg(node, owner, tier, eth_address, eth_key, wire_key, silent=False):
    data = json.dumps({"owner": owner, "tier": tier,
                       "eth_pub_key": eth_key, "wire_pub_key": wire_key,
                       "eth_address": eth_address})
    return node.pushMessage("sysio.roa", "nodeownreg", data,
                            "--permission sysio.roa@active", silentErrors=silent)


def push_nodeownreg_legacy(node, owner, tier, eth_key, wire_key, silent=False):
    """Push the pre-WIRE-352 four-field action shape (no trailing binary extension)."""
    data = json.dumps({"owner": owner, "tier": tier,
                       "eth_pub_key": eth_key, "wire_pub_key": wire_key})
    return node.pushMessage("sysio.roa", "nodeownreg", data,
                            "--permission sysio.roa@active", silentErrors=silent)


def get_nodeowner(node, owner):
    """nodeowners row for owner (scope = network_gen = 0); kv::table rows wrap the struct in 'value'."""
    rows = node.getTableRows("sysio.roa", "0", "nodeowners")
    return next((r["value"] for r in rows if r["value"]["owner"] == owner), None)


def get_audit(node, owner):
    """nodeownerreg audit row for owner (status / reason)."""
    rows = node.getTableRows("sysio.roa", "0", "nodeownerreg")
    return next((r["value"] for r in rows if r["value"]["owner"] == owner), None)


# ---------------------------------------------------------------------------
#  Test
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

    # Bootstraps sysio.roa (active) + sysio.authex + the authex.active <- sysio.roa@sysio.code grant.
    assert cluster.launch(
        pnodes=1,
        prodCount=1,
        totalProducers=1,
        totalNodes=2,
        loadSystemContract=False,
        activateIF=True,
    ), "Failed to launch cluster"

    node = cluster.getNode(0)

    # The claim's depositor ETH key (EM) and the new account's owner/active Wire key (K1).
    eth_key = wallet_create_key(walletMgr.host, walletMgr.port, "EM")
    eth_address = ethereum_address(eth_key)
    wire_key = wallet_create_key(walletMgr.host, walletMgr.port, "K1")
    Utils.Print(f"  eth (EM) key:  {eth_key}")
    Utils.Print(f"  wire (K1) key: {wire_key}")

    # ---- Test 1: happy path -- create the account then register it ----
    Utils.Print("=== Test 1: newnameduser + nodeownreg happy path ===")
    owner = "claimacct1a"
    assert push_newnameduser(node, owner, wire_key, 2)[0], "newnameduser failed"
    assert push_nodeownreg(node, owner, 2, eth_address, eth_key, wire_key)[0], "nodeownreg failed"

    reg = get_nodeowner(node, owner)
    assert reg is not None, "node owner not found in nodeowners table"
    assert int(reg["tier"]) == 2, f"expected tier 2, got {reg['tier']}"
    audit = get_audit(node, owner)
    assert audit is not None and int(audit["status"]) == CONFIRMED, f"expected CONFIRMED, got {audit}"
    Utils.Print(f"  Verified: {owner} registered tier-2, audit CONFIRMED")

    # ---- Test 1b: the pre-WIRE-352 payload remains valid after contract rollout ----
    Utils.Print("=== Test 1b: legacy four-field nodeownreg payload ===")
    legacy_owner = "claimacct1b"
    legacy_wire_key = wallet_create_key(walletMgr.host, walletMgr.port, "K1")
    legacy_eth_key = wallet_create_key(walletMgr.host, walletMgr.port, "EM")
    assert push_newnameduser(node, legacy_owner, legacy_wire_key, 2)[0], \
        "legacy newnameduser failed"
    assert push_nodeownreg_legacy(node, legacy_owner, 2, legacy_eth_key, legacy_wire_key)[0], \
        "legacy nodeownreg payload failed"
    legacy_reg = get_nodeowner(node, legacy_owner)
    legacy_audit = get_audit(node, legacy_owner)
    assert legacy_reg is not None and int(legacy_reg["tier"]) == 2, \
        f"legacy node owner registration missing: {legacy_reg}"
    assert legacy_audit is not None and int(legacy_audit["status"]) == CONFIRMED, \
        f"legacy nodeownreg expected CONFIRMED, got {legacy_audit}"
    Utils.Print("  Verified: legacy payload remains accepted")

    # ---- Test 2: account controlled by a different key -> soft-fail ACCOUNT_KEY_MISMATCH ----
    Utils.Print("=== Test 2: nodeownreg with a non-matching wire key (soft-fail) ===")
    owner2 = "claimacct2a"
    real_key = wallet_create_key(walletMgr.host, walletMgr.port, "K1")
    other_key = wallet_create_key(walletMgr.host, walletMgr.port, "K1")
    assert push_newnameduser(node, owner2, real_key, 2)[0], "newnameduser (owner2) failed"
    # Claim owner2 with a different wire key than it was created with.
    assert push_nodeownreg(node, owner2, 2, eth_address, eth_key, other_key)[0], \
        "nodeownreg should soft-fail, not abort"
    assert get_nodeowner(node, owner2) is None, "owner2 must not be registered"
    audit2 = get_audit(node, owner2)
    assert audit2 is not None and int(audit2["status"]) == REJECTED \
        and int(audit2["reason"]) == R_ACCOUNT_KEY_MISMATCH, f"expected ACCOUNT_KEY_MISMATCH, got {audit2}"
    Utils.Print("  Correctly soft-failed ACCOUNT_KEY_MISMATCH")

    # ---- Test 3: re-register the owner -> soft-fail DUPLICATE ----
    # Use a fresh ETH key so the replay is a *distinct* transaction: a byte-identical resubmission of
    # Test 1's call shares its trx id, so the chain rejects it as tx_duplicate before the contract's
    # DUPLICATE check can run. The eth key does not affect the duplicate path (the check precedes the
    # recordlink), so a new EM key still drives the soft-fail.
    Utils.Print("=== Test 3: nodeownreg replay (soft-fail DUPLICATE) ===")
    replay_eth_key = wallet_create_key(walletMgr.host, walletMgr.port, "EM")
    replay_eth_address = ethereum_address(replay_eth_key)
    assert push_nodeownreg(node, owner, 2, replay_eth_address, replay_eth_key, wire_key)[0], \
        "nodeownreg replay should soft-fail, not abort"
    audit_dup = get_audit(node, owner)
    assert int(audit_dup["status"]) == REJECTED and int(audit_dup["reason"]) == R_DUPLICATE, \
        f"expected DUPLICATE, got {audit_dup}"
    Utils.Print("  Correctly soft-failed DUPLICATE")

    # ---- Test 4: invalid tier -> hard abort (depot/system invariant) ----
    Utils.Print("=== Test 4: nodeownreg with invalid tier (hard abort) ===")
    assert not push_nodeownreg(node, owner, 0, eth_address, eth_key, wire_key, silent=True)[0], \
        "tier 0 must abort"
    assert not push_nodeownreg(node, owner, 4, eth_address, eth_key, wire_key, silent=True)[0], \
        "tier 4 must abort"
    Utils.Print("  Correctly aborted invalid tiers")

    # ---- Test 5: non-EM depositor key -> hard abort ----
    Utils.Print("=== Test 5: nodeownreg with a non-EM eth key (hard abort) ===")
    k1_as_eth = wallet_create_key(walletMgr.host, walletMgr.port, "K1")
    assert not push_nodeownreg(node, "claimacct3a", 2, "00" * 20, k1_as_eth, wire_key, silent=True)[0], \
        "non-EM eth_pub_key must abort"
    Utils.Print("  Correctly aborted non-EM eth key")

    testSuccessful = True
    Utils.Print("=== All nodeownreg integration tests passed ===")

finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

exit(0 if testSuccessful else 1)
