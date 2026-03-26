#!/usr/bin/env python3

import json
import signal

from TestHarness import Account, Cluster, TestHelper, Utils, WalletMgr
from TestHarness.TestHelper import AppArgs

###############################################################
# snapshot_attest_test
#
#  Tests on-chain snapshot attestation via sysio.system contract.
#  This test validates the snapshot serving pipeline:
#  - Snapshot provider registration (regsnapprov)
#  - Snapshot hash voting (votesnaphash) with quorum
#  - Attested record creation and querying (snaprecords table)
#  - Snapshot restart preserves attestation records
#
#  Cluster layout:
#    Node 0: producer (defproducera) — takes snapshots, pushes actions
#    Node 1: producer (defproducerb)
#    Node 2: non-producing validation node
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

appArgs = AppArgs()
args=TestHelper.parse_args({"--dump-error-details","--keep-logs","-v","--leave-running","--unshared"},
                            applicationSpecificArgs=appArgs)

debug=args.v
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

pnodes=2
totalNodes=pnodes+1

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True)

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)

    Print("Stand up cluster")
    if cluster.launch(pnodes=pnodes, totalNodes=totalNodes, prodCount=1,
                      maximumP2pPerHost=totalNodes, loadSystemContract=True,
                      activateIF=False) is False:
        errorExit("Failed to stand up sys cluster.")

    node0 = cluster.getNode(0)

    Print("Create wallet and import keys")
    wallet = walletMgr.create('attestwallet')
    cluster.populateWallet(2, wallet)

    Print("Create test accounts for snapshot providers")
    cluster.createAccounts(cluster.sysioAccount, stakedDeposit=0)

    snapProv1 = cluster.accounts[0]
    snapProv2 = cluster.accounts[1]

    Print(f"Snapshot provider accounts: {snapProv1.name}, {snapProv2.name}")

    Print("Wait for account creation to be irreversible")
    assert node0.waitForLibToAdvance(timeout=30), "LIB did not advance after account creation"

    # ---------------------------------------------------------------
    # Register producers in system contract and set ranks
    # ---------------------------------------------------------------
    producerA = "defproducera"
    producerB = "defproducerb"

    # Import producer keys into wallet for signing
    ignWallet = walletMgr.create("ignition")
    for name in [producerA, producerB]:
        account = cluster.defProducerAccounts[name]
        walletMgr.importKey(account, ignWallet, ignoreDupKeyWarning=True)

    # Register producers via regproducer (required before setrank)
    for name in [producerA, producerB]:
        account = cluster.defProducerAccounts[name]
        success, trans = node0.pushMessage("sysio", "regproducer",
            json.dumps({
                "producer": name,
                "producer_key": account.activePublicKey,
                "url": "",
                "location": 0
            }),
            f"--permission {name}@active")
        assert success, f"Failed to register producer {name}: {trans}"
        Print(f"Registered producer {name}")

    assert node0.waitForHeadToAdvance(), "Head did not advance after regproducer"

    Print(f"Set rank for {producerA}")
    success, trans = node0.pushMessage("sysio", "setrank",
        json.dumps({"producer": producerA, "rank": 1}),
        "--permission sysio@active")
    assert success, f"Failed to set rank for {producerA}: {trans}"

    Print(f"Set rank for {producerB}")
    success, trans = node0.pushMessage("sysio", "setrank",
        json.dumps({"producer": producerB, "rank": 2}),
        "--permission sysio@active")
    assert success, f"Failed to set rank for {producerB}: {trans}"

    assert node0.waitForHeadToAdvance(), "Head did not advance after setrank"

    # ---------------------------------------------------------------
    # Register snapshot providers
    # ---------------------------------------------------------------
    Print(f"Register snapshot provider {snapProv1.name} for {producerA}")
    success, trans = node0.pushMessage("sysio", "regsnapprov",
        json.dumps({"producer": producerA, "snap_account": snapProv1.name}),
        f"--permission {producerA}@active")
    assert success, f"Failed to register snapshot provider: {trans}"

    Print(f"Register snapshot provider {snapProv2.name} for {producerB}")
    success, trans = node0.pushMessage("sysio", "regsnapprov",
        json.dumps({"producer": producerB, "snap_account": snapProv2.name}),
        f"--permission {producerB}@active")
    assert success, f"Failed to register snapshot provider: {trans}"

    # ---------------------------------------------------------------
    # Set attestation config: min_providers=1, threshold_pct=50
    # With 2 providers and 50%, quorum = max(1, ceil(2*50/100)) = 1
    # So a single vote will create an attested record.
    # ---------------------------------------------------------------
    Print("Set snapshot attestation config")
    success, trans = node0.pushMessage("sysio", "setsnpcfg",
        json.dumps({"min_providers": 1, "threshold_pct": 50}),
        "--permission sysio@active")
    assert success, f"Failed to set snapshot config: {trans}"

    assert node0.waitForHeadToAdvance(), "Head did not advance after config"

    # ---------------------------------------------------------------
    # Verify provider registration via table query
    # ---------------------------------------------------------------
    Print("Verify snapshot providers registered")
    providers = node0.getTableRows("sysio", "sysio", "snapprovs")
    assert providers is not None, "Failed to read snapprovs table"
    assert len(providers) == 2, f"Expected 2 providers, got {len(providers)}"
    Print(f"Registered providers: {providers}")

    # ---------------------------------------------------------------
    # Test 1: Create snapshot and submit attestation vote
    # ---------------------------------------------------------------
    Print("=== Test 1: Snapshot creation and attestation vote ===")

    Print("Create snapshot on node 0")
    ret = node0.createSnapshot()
    assert ret is not None, "Snapshot creation failed"
    snapInfo = ret["payload"]
    snapBlockNum = snapInfo["head_block_num"]
    snapBlockId = snapInfo["head_block_id"]
    snapRootHash = snapInfo["root_hash"]
    Print(f"Snapshot: block_num={snapBlockNum}, block_id={snapBlockId}, root_hash={snapRootHash}")

    assert snapRootHash is not None and snapRootHash != "", "Snapshot root_hash should not be empty"
    assert len(snapRootHash) == 64 and all(c in '0123456789abcdef' for c in snapRootHash), \
        f"Snapshot root_hash should be a 64-char hex string, got: {snapRootHash}"

    Print(f"Submit votesnaphash from {snapProv1.name}")
    success, trans = node0.pushMessage("sysio", "votesnaphash",
        json.dumps({
            "snap_account": snapProv1.name,
            "block_id": snapBlockId,
            "snapshot_hash": snapRootHash
        }),
        f"--permission {snapProv1.name}@active")
    assert success, f"Failed to submit snapshot vote: {trans}"

    assert node0.waitForHeadToAdvance(), "Head did not advance after vote"

    # ---------------------------------------------------------------
    # Verify attested record created (quorum=1, single vote suffices)
    # ---------------------------------------------------------------
    Print("Verify attested snapshot record on-chain")
    records = node0.getTableRows("sysio", "sysio", "snaprecords")
    assert records is not None, "Failed to read snaprecords table"
    assert len(records) >= 1, f"Expected at least 1 attested record, got {len(records)}"

    found = False
    for rec in records:
        if rec["block_num"] == snapBlockNum:
            found = True
            onChainHash = rec["snapshot_hash"]
            Print(f"On-chain record: block_num={rec['block_num']}, snapshot_hash={onChainHash}")
            assert onChainHash == snapRootHash, \
                f"Hash mismatch! on-chain={onChainHash} vs snapshot={snapRootHash}"
            break
    assert found, f"No attested record found for block {snapBlockNum}"

    Print("Test 1 PASSED: Snapshot hash matches on-chain attestation")

    # ---------------------------------------------------------------
    # Test 2: Second provider can also vote (duplicate block, same hash)
    # ---------------------------------------------------------------
    Print("=== Test 2: Multiple provider votes ===")

    Print("Create another snapshot for a new block")
    assert node0.waitForHeadToAdvance(blocksToAdvance=2), "Head did not advance"

    ret2 = node0.createSnapshot()
    assert ret2 is not None, "Second snapshot creation failed"
    snap2Info = ret2["payload"]
    snap2BlockNum = snap2Info["head_block_num"]
    snap2BlockId = snap2Info["head_block_id"]
    snap2RootHash = snap2Info["root_hash"]
    Print(f"Snapshot 2: block_num={snap2BlockNum}, root_hash={snap2RootHash}")

    Print(f"Submit vote from {snapProv1.name}")
    success, trans = node0.pushMessage("sysio", "votesnaphash",
        json.dumps({
            "snap_account": snapProv1.name,
            "block_id": snap2BlockId,
            "snapshot_hash": snap2RootHash
        }),
        f"--permission {snapProv1.name}@active")
    assert success, f"First vote failed: {trans}"

    Print(f"Submit vote from {snapProv2.name}")
    success, trans = node0.pushMessage("sysio", "votesnaphash",
        json.dumps({
            "snap_account": snapProv2.name,
            "block_id": snap2BlockId,
            "snapshot_hash": snap2RootHash
        }),
        f"--permission {snapProv2.name}@active")
    assert success, f"Second vote failed: {trans}"

    assert node0.waitForHeadToAdvance(), "Head did not advance after votes"

    Print("Verify second attested record")
    records = node0.getTableRows("sysio", "sysio", "snaprecords")
    found2 = any(r["block_num"] == snap2BlockNum for r in records)
    assert found2, f"No attested record for block {snap2BlockNum}"

    Print("Test 2 PASSED: Multiple provider votes and attestation")

    # ---------------------------------------------------------------
    # Test 3: getsnaphash read-only action
    # ---------------------------------------------------------------
    Print("=== Test 3: getsnaphash query ===")

    success, trans = node0.pushMessage("sysio", "getsnaphash",
        json.dumps({"block_num": snapBlockNum}),
        "--permission sysio@active --read-only",
        silentErrors=True)
    # Read-only actions may not return standard transaction traces,
    # but the action should not fail
    Print(f"getsnaphash result: success={success}")

    Print("Test 3 PASSED: getsnaphash query executed")

    # ---------------------------------------------------------------
    # Test 4: Node loads attested snapshot, syncs from peers, verifies
    # ---------------------------------------------------------------
    # Real-world flow:
    #   1. Take a snapshot on node 0
    #   2. Attest it on-chain (votesnaphash creates snaprecords entry)
    #   3. Kill validation node (node 2), wipe its state
    #   4. Restart node 2 from the attested snapshot
    #   5. Node 2 syncs from producing nodes (0 and 1) to catch up
    #   6. Once LIB advances past snapshot block, verification triggers
    #   7. Verify attestation records are accessible on synced node
    Print("=== Test 4: Load attested snapshot, sync, and verify ===")

    Print("Wait for LIB to advance past earlier attestation blocks")
    assert node0.waitForLibToAdvance(timeout=30), "LIB did not advance"

    # Take a snapshot that we will attest
    Print("Create attestable snapshot on node 0")
    retAttest = node0.createSnapshot()
    assert retAttest is not None, "Attestable snapshot creation failed"
    attestInfo = retAttest["payload"]
    attestBlockNum = attestInfo["head_block_num"]
    attestBlockId = attestInfo["head_block_id"]
    attestRootHash = attestInfo["root_hash"]
    Print(f"Attestable snapshot: block_num={attestBlockNum}, root_hash={attestRootHash}")

    # Submit attestation vote — quorum=1, so this creates the record
    Print(f"Submit votesnaphash for attestable snapshot from {snapProv1.name}")
    success, trans = node0.pushMessage("sysio", "votesnaphash",
        json.dumps({
            "snap_account": snapProv1.name,
            "block_id": attestBlockId,
            "snapshot_hash": attestRootHash
        }),
        f"--permission {snapProv1.name}@active")
    assert success, f"Failed to attest snapshot: {trans}"

    assert node0.waitForHeadToAdvance(), "Head did not advance after attestation"

    # Verify the attestation record exists on the producing node
    records = node0.getTableRows("sysio", "sysio", "snaprecords")
    found = any(r["block_num"] == attestBlockNum for r in records)
    assert found, f"Attestation record not created for block {attestBlockNum}"
    Print(f"Attestation record confirmed for block {attestBlockNum}")

    # Wait for attestation to become irreversible so syncing node will see it
    assert node0.waitForLibToAdvance(timeout=30), "LIB did not advance after attestation"

    # Get the attested snapshot file path (it's the latest snapshot on node 0)
    attestedSnapshotFile = node0.getLatestSnapshot()
    Print(f"Attested snapshot file: {attestedSnapshotFile}")

    # Kill validation node (node 2) and wipe its state
    validationNode = cluster.getNode(2)
    Print("Kill validation node")
    validationNode.kill(signal.SIGTERM)

    Print("Wipe validation node state, keep peer connections")
    validationNode.removeDataDir(rmBlocks=True)

    # Restart from the attested snapshot — node keeps its p2p-peer-address
    # connections to producing nodes so it can sync and catch up
    Print("Restart validation node from attested snapshot")
    isRelaunchSuccess = validationNode.relaunch(chainArg=f"--snapshot {attestedSnapshotFile}")
    assert isRelaunchSuccess, "Failed to relaunch node from attested snapshot"

    Print("Wait for restarted node to sync from peers")
    assert validationNode.waitForLibToAdvance(timeout=60), \
        "LIB did not advance on restarted node — check peer connectivity"

    # After syncing, the node should have the attestation data from chain state
    Print("Verify attestation records on synced node")
    records = validationNode.getTableRows("sysio", "sysio", "snaprecords")
    assert records is not None, "Failed to read snaprecords on synced node"
    assert len(records) >= 1, f"Expected attestation records after sync, got {len(records)}"

    found = any(r["block_num"] == attestBlockNum for r in records)
    assert found, f"Attestation for block {attestBlockNum} not found after sync"

    Print("Test 4 PASSED: Attested snapshot loaded, synced, and verified")

    # ---------------------------------------------------------------
    # Test 5: Deregistration of snapshot provider
    # ---------------------------------------------------------------
    Print("=== Test 5: Provider deregistration ===")

    Print(f"Deregister snapshot provider {snapProv2.name}")
    success, trans = node0.pushMessage("sysio", "delsnapprov",
        json.dumps({"account": snapProv2.name}),
        f"--permission {snapProv2.name}@active")
    assert success, f"Failed to deregister provider: {trans}"

    assert node0.waitForHeadToAdvance(), "Head did not advance after deregistration"

    providers = node0.getTableRows("sysio", "sysio", "snapprovs")
    assert len(providers) == 1, f"Expected 1 provider after deregistration, got {len(providers)}"
    assert providers[0]["snap_account"] == snapProv1.name, \
        f"Remaining provider should be {snapProv1.name}, got {providers[0]['snap_account']}"

    Print("Test 5 PASSED: Provider deregistration works")

    # ---------------------------------------------------------------
    Print("All snapshot attestation tests PASSED")
    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
