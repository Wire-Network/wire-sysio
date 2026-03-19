#!/usr/bin/env python3

"""
nodeownreg_test.py — OPP Node Owner NFT Registration Integration Test

Simulates the OPP NodeOwnerRegistration attestation flow:
  1. Bootstrap chain with sysio.roa and sysio.authex
  2. Create an ETH (EM) key pair and link it via authex createlink
  3. Call nodeownreg on sysio.roa (faking depot processing an OPP message)
  4. Verify node owner registration in the nodeowners table
  5. Negative-path tests: wrong key, no link, invalid tier, already registered
"""

import calendar
import json
import time
import urllib.request

from TestHarness import Account, Cluster, TestHelper, Utils, WalletMgr

# ---------------------------------------------------------------------------
#  Keccak-256 via clio convert keccak256
# ---------------------------------------------------------------------------

def keccak256_hex(text: str) -> str:
    """Compute Keccak-256 hash of a text string using clio. Returns hex digest."""
    cmd = f"{Utils.SysClientPath} convert keccak256 '{text}'"
    return Utils.runCmdReturnStr(cmd).strip()


# ---------------------------------------------------------------------------
#  kiod wallet HTTP helpers
# ---------------------------------------------------------------------------

def kiod_request(host, port, endpoint, params):
    """POST JSON to kiod wallet API and return parsed response."""
    url = f"http://{host}:{port}{endpoint}"
    body = json.dumps(params).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    try:
        resp = urllib.request.urlopen(req)
        return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        err_body = e.read().decode()
        raise RuntimeError(f"kiod {endpoint} failed ({e.code}): {err_body}") from e


def wallet_create_em_key(host, port, wallet_name="ignition"):
    """Create an EM (secp256k1/Ethereum) key in the kiod wallet. Returns the public key string."""
    return kiod_request(host, port, "/v1/wallet/create_key", [wallet_name, "EM"])


def wallet_sign_digest(host, port, digest_hex, pub_key):
    """Sign a 32-byte digest (hex) with the given public key via kiod. Returns signature string."""
    return kiod_request(host, port, "/v1/wallet/sign_digest", [digest_hex, pub_key])


# ---------------------------------------------------------------------------
#  authex createlink helper
# ---------------------------------------------------------------------------

def compress_em_pubkey(pub_key_str):
    """Convert uncompressed PUB_EM_ key to compressed form used by contract's pubkey_to_string.
    Uncompressed: PUB_EM_04{x}{y} (65 bytes) → Compressed: PUB_EM_02/03{x} (33 bytes)."""
    assert pub_key_str.startswith("PUB_EM_"), f"Expected PUB_EM_ prefix, got: {pub_key_str}"
    hex_data = pub_key_str[7:]
    raw = bytes.fromhex(hex_data)
    if raw[0] == 0x04 and len(raw) == 65:
        x = raw[1:33]
        y = raw[33:65]
        prefix = b'\x02' if y[-1] % 2 == 0 else b'\x03'
        return "PUB_EM_" + (prefix + x).hex()
    return pub_key_str  # Already compressed


def build_link_message(pub_key_str, account, chain_kind, nonce):
    """Build the exact message string that sysio.authex hashes for signature verification.
    Uses compressed key format to match the contract's pubkey_to_string."""
    compressed = compress_em_pubkey(pub_key_str)
    return f"{compressed}|{account}|{chain_kind}|{nonce}|createlink auth"


def create_eth_link(node, walletMgr, account_name, em_pub_key):
    """Create an authex ETH link for the given account using the EM key.
    Returns (success, nonce) tuple."""
    # Get chain head time as nonce (milliseconds, UTC)
    info = node.getInfo()
    head_time = info["head_block_time"]  # e.g. "2025-01-01T00:00:03.000"
    t = time.strptime(head_time[:19], "%Y-%m-%dT%H:%M:%S")
    nonce = int(calendar.timegm(t)) * 1000

    # Build the message the contract will hash
    msg = build_link_message(em_pub_key, account_name, 2, nonce)
    Utils.Print(f"  Link message: {msg}")

    # Keccak-256 hash via clio
    digest_hex = keccak256_hex(msg)
    Utils.Print(f"  Keccak-256 digest: {digest_hex}")

    # Sign via kiod wallet
    sig = wallet_sign_digest(walletMgr.host, walletMgr.port, digest_hex, em_pub_key)
    Utils.Print(f"  Signature: {sig}")

    # Push createlink action
    data = json.dumps({
        "chain_kind": 2,
        "account": account_name,
        "sig": sig,
        "pub_key": em_pub_key,
        "nonce": nonce,
    })
    trans = node.pushMessage("sysio.authex", "createlink", data,
                             f"--permission {account_name}@active")
    Utils.Print(f"  createlink result: success={trans[0]}, type={type(trans[1]).__name__}")
    if not trans[0]:
        Utils.Print(f"  createlink full response: {trans[1]}")
    return trans[0], nonce


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

    # ---- Launch cluster (bootstraps sysio.roa + sysio.authex) ----
    assert cluster.launch(
        pnodes=1,
        prodCount=1,
        totalProducers=1,
        totalNodes=2,
        loadSystemContract=False,
        activateIF=True,
    ), "Failed to launch cluster"

    node = cluster.getNode(0)

    # ---- Create test account for the node owner ----
    Utils.Print("=== Create node owner account ===")
    nodeOwner = Account("nodeowner1a")
    nodeOwner.ownerPublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    nodeOwner.activePublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    node.createAccount(nodeOwner, cluster.sysioAccount)

    # ---- Create EM key in wallet ----
    Utils.Print("=== Create EM key in wallet ===")
    em_pub_key = wallet_create_em_key(walletMgr.host, walletMgr.port)
    Utils.Print(f"  EM public key: {em_pub_key}")

    # ---- Test 1: Create ETH link via authex ----
    Utils.Print("=== Test 1: Create ETH link via sysio.authex createlink ===")
    ok, nonce = create_eth_link(node, walletMgr, nodeOwner.name, em_pub_key)
    assert ok, "createlink failed"
    Utils.Print("  createlink succeeded")

    # ---- Test 2: Happy path — nodeownreg (simulating OPP depot) ----
    Utils.Print("=== Test 2: nodeownreg happy path (faking OPP NodeOwnerRegistration) ===")
    data = json.dumps({
        "owner": nodeOwner.name,
        "tier": 1,
        "eth_pub_key": em_pub_key,
    })
    trans = node.pushMessage("sysio.roa", "nodeownreg", data,
                             "--permission sysio.roa@active")
    assert trans[0], f"nodeownreg failed: {trans[1]}"
    Utils.Print("  nodeownreg succeeded")

    # Verify registration in nodeowners table (scope = network_gen = 0)
    rows = node.getTableRows("sysio.roa", "0", "nodeowners")
    owner_row = next((r for r in rows if r["owner"] == nodeOwner.name), None)
    assert owner_row is not None, "Node owner not found in nodeowners table"
    assert int(owner_row["tier"]) == 1, f"Expected tier 1, got {owner_row['tier']}"
    Utils.Print(f"  Verified: {nodeOwner.name} registered as tier-1 node owner")

    # ---- Test 3: Wrong ETH key — should fail ----
    Utils.Print("=== Test 3: nodeownreg with wrong ETH key (should fail) ===")
    nodeOwner2 = Account("nodeowner2a")
    nodeOwner2.ownerPublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    nodeOwner2.activePublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    node.createAccount(nodeOwner2, cluster.sysioAccount)

    # Create a NEW EM key and link it to nodeOwner2
    em_pub_key2 = wallet_create_em_key(walletMgr.host, walletMgr.port)
    ok2, _ = create_eth_link(node, walletMgr, nodeOwner2.name, em_pub_key2)
    assert ok2, "createlink for nodeOwner2 failed"

    # Try nodeownreg with a DIFFERENT key (not the one linked)
    wrong_key = wallet_create_em_key(walletMgr.host, walletMgr.port)
    data = json.dumps({
        "owner": nodeOwner2.name,
        "tier": 1,
        "eth_pub_key": wrong_key,
    })
    trans = node.pushMessage("sysio.roa", "nodeownreg", data,
                             "--permission sysio.roa@active", silentErrors=True)
    assert not trans[0], "nodeownreg should have failed with wrong ETH key"
    Utils.Print("  Correctly rejected wrong ETH key")

    # ---- Test 4: No ETH link — should fail ----
    Utils.Print("=== Test 4: nodeownreg with no authex link (should fail) ===")
    nodeOwner3 = Account("nodeowner3a")
    nodeOwner3.ownerPublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    nodeOwner3.activePublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    node.createAccount(nodeOwner3, cluster.sysioAccount)

    data = json.dumps({
        "owner": nodeOwner3.name,
        "tier": 1,
        "eth_pub_key": wrong_key,
    })
    trans = node.pushMessage("sysio.roa", "nodeownreg", data,
                             "--permission sysio.roa@active", silentErrors=True)
    assert not trans[0], "nodeownreg should have failed with no authex link"
    Utils.Print("  Correctly rejected account with no ETH link")

    # ---- Test 5: Invalid tier — should fail ----
    Utils.Print("=== Test 5: nodeownreg with invalid tier (should fail) ===")
    data = json.dumps({
        "owner": nodeOwner2.name,
        "tier": 0,
        "eth_pub_key": em_pub_key,
    })
    trans = node.pushMessage("sysio.roa", "nodeownreg", data,
                             "--permission sysio.roa@active", silentErrors=True)
    assert not trans[0], "nodeownreg should have failed with tier 0"

    data = json.dumps({
        "owner": nodeOwner2.name,
        "tier": 4,
        "eth_pub_key": em_pub_key,
    })
    trans = node.pushMessage("sysio.roa", "nodeownreg", data,
                             "--permission sysio.roa@active", silentErrors=True)
    assert not trans[0], "nodeownreg should have failed with tier 4"
    Utils.Print("  Correctly rejected invalid tier values")

    # ---- Test 6: Already registered — should fail ----
    Utils.Print("=== Test 6: nodeownreg for already-registered owner (should fail) ===")
    data = json.dumps({
        "owner": nodeOwner.name,
        "tier": 2,
        "eth_pub_key": em_pub_key,
    })
    trans = node.pushMessage("sysio.roa", "nodeownreg", data,
                             "--permission sysio.roa@active", silentErrors=True)
    assert not trans[0], "nodeownreg should have failed — owner already registered"
    Utils.Print("  Correctly rejected already-registered owner")

    testSuccessful = True
    Utils.Print("=== All nodeownreg integration tests passed ===")

finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
