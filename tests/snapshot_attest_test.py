#!/usr/bin/env python3

"""Exercise provider registration and the scheduled-height attestation boundary."""

import json

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from TestHarness.TestHelper import AppArgs

###############################################################
# snapshot_attest_test
#
#  Tests on-chain snapshot attestation via sysio.system contract.
#  This test validates the snapshot serving pipeline:
#  - Snapshot provider registration and account rotation
#  - Manual snapshot creation
#  - Rejection of votes outside the fixed 25,000-block cadence
#
#  Successful fixed-K formation and attestation-record persistence use synthetic
#  scheduled heights in the C++ contract and unit test suites. Waiting for that
#  height in this wall-clock integration test would take hours.
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
# Fixed production cadence shared by provider scheduling and the system contract.
snapshotAttestationBlockSpacing=25000

testSuccessful=False

pnodes=2
totalNodes=pnodes+1
port=Utils.getPort(Utils.PortNodeHttp)
walletPort=Utils.getPort(Utils.PortWallet)

cluster=Cluster(port=port, walletPort=walletPort, unshared=args.unshared,
                keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True, nodeopPort=port, port=walletPort)

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
    regProducerTransIds = []
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
        regProducerTransIds.append(node0.getTransId(trans))
        Print(f"Registered producer {name}")

    # setrank reads the on-chain producers table and asserts "producer not found"
    # if a registration is missing.
    # pushMessage only confirms speculative
    # execution, and waitForHeadToAdvance() does not guarantee these specific
    # transactions were applied — with multiple producers a transaction pushed
    # to node0 can be forwarded into a peer's block. Wait for both regproducer
    # transactions to appear in a block so setrank speculatively executes
    # against state that already contains the registrations.
    assert node0.waitForTransactionsInBlock(regProducerTransIds, timeout=60), \
        "regproducer transactions did not make it into a block before setrank"

    setRankTransIds = []
    Print(f"Set rank for {producerA}")
    success, trans = node0.pushMessage("sysio", "setrank",
        json.dumps({"producer": producerA, "rank": 1}),
        "--permission sysio@active")
    assert success, f"Failed to set rank for {producerA}: {trans}"
    setRankTransIds.append(node0.getTransId(trans))

    Print(f"Set rank for {producerB}")
    success, trans = node0.pushMessage("sysio", "setrank",
        json.dumps({"producer": producerB, "rank": 2}),
        "--permission sysio@active")
    assert success, f"Failed to set rank for {producerB}: {trans}"
    setRankTransIds.append(node0.getTransId(trans))

    # regsnapprov reads producer ranks from the on-chain producers table. A
    # generic head advance can race the exact setrank transactions under
    # multi-producer scheduling, so wait for those transactions specifically.
    assert node0.waitForTransactionsInBlock(setRankTransIds, timeout=60), \
        "setrank transactions did not make it into a block before regsnapprov"

    # ---------------------------------------------------------------
    # Register snapshot providers
    # ---------------------------------------------------------------
    regSnapProvTransIds = []
    Print(f"Register snapshot provider {snapProv1.name} for {producerA}")
    success, trans = node0.pushMessage("sysio", "regsnapprov",
        json.dumps({"producer": producerA, "snap_account": snapProv1.name}),
        f"--permission {producerA}@active")
    assert success, f"Failed to register snapshot provider: {trans}"
    regSnapProvTransIds.append(node0.getTransId(trans))

    Print(f"Register snapshot provider {snapProv2.name} for {producerB}")
    success, trans = node0.pushMessage("sysio", "regsnapprov",
        json.dumps({"producer": producerB, "snap_account": snapProv2.name}),
        f"--permission {producerB}@active")
    assert success, f"Failed to register snapshot provider: {trans}"
    regSnapProvTransIds.append(node0.getTransId(trans))

    # ---------------------------------------------------------------
    # Set attestation config: fixed K=1.
    # This confirms governance configuration independently of the cadence rejection below.
    # ---------------------------------------------------------------
    Print("Set snapshot attestation config")
    success, trans = node0.pushMessage("sysio", "setsnpcfg",
        json.dumps({"min_providers": 1}),
        "--permission sysio@active")
    assert success, f"Failed to set snapshot config: {trans}"
    setCfgTransId = node0.getTransId(trans)

    # ---------------------------------------------------------------
    # Verify provider registration via table query
    # ---------------------------------------------------------------
    # Ensure both provider registrations and the attestation config are applied
    # in a block before proceeding. The exact-count assertion below depends on
    # those transactions being visible in table state.
    assert node0.waitForTransactionsInBlock(regSnapProvTransIds + [setCfgTransId], timeout=60), \
        "regsnapprov/setsnpcfg transactions did not make it into a block"
    Print("Verify snapshot providers registered")
    providers = node0.getTableRows("sysio", "sysio", "snapprovs")
    assert providers is not None, "Failed to read snapprovs table"
    assert len(providers) == 2, f"Expected 2 providers, got {len(providers)}"
    Print(f"Registered providers: {providers}")

    # ---------------------------------------------------------------
    # Test 1: Manual snapshots cannot enter the scheduled attestation tally
    # ---------------------------------------------------------------
    Print("=== Test 1: Manual snapshot vote rejection ===")

    ret = node0.createSnapshot()
    assert ret is not None, "Snapshot creation failed"
    snapInfo = ret["payload"]
    snapBlockNum = snapInfo["head_block_num"]
    snapBlockId = snapInfo["head_block_id"]
    snapRootHash = snapInfo["root_hash"]
    Print(f"Manual snapshot: block_num={snapBlockNum}, root_hash={snapRootHash}")

    assert snapBlockNum % snapshotAttestationBlockSpacing != 0, (
        "Wall-clock integration snapshot unexpectedly landed on the production cadence"
    )
    assert len(snapRootHash) == 64 and all(c in '0123456789abcdef' for c in snapRootHash), \
        f"Snapshot root_hash should be a 64-char hex string, got: {snapRootHash}"

    success, trans = node0.pushMessage("sysio", "votesnaphash",
        json.dumps({
            "snap_account": snapProv1.name,
            "block_id": snapBlockId,
            "snapshot_hash": snapRootHash
        }),
        f"--permission {snapProv1.name}@active",
        silentErrors=True)
    assert not success, f"Unscheduled manual snapshot vote unexpectedly succeeded: {trans}"
    assert "snapshot block is not a scheduled attestation height" in str(trans), \
        f"Unexpected manual snapshot vote failure: {trans}"

    records = node0.getTableRows("sysio", "sysio", "snaprecords")
    assert records is not None, "Failed to read snaprecords table"
    assert not any(r["value"]["block_num"] == snapBlockNum for r in records), \
        f"Manual snapshot block {snapBlockNum} unexpectedly entered snaprecords"

    Print("Test 1 PASSED: Manual snapshot stays outside the scheduled attestation tally")

    # ---------------------------------------------------------------
    # Test 2: Rotate a producer's snapshot provider account
    # ---------------------------------------------------------------
    Print("=== Test 2: Provider account rotation ===")

    Print(f"Rotate {producerB} snapshot provider from {snapProv2.name} to {producerB}")
    success, trans = node0.pushMessage("sysio", "regsnapprov",
        json.dumps({"producer": producerB, "snap_account": producerB}),
        f"--permission {producerB}@active")
    assert success, f"Failed to rotate provider account: {trans}"
    rotateSnapProvTransId = node0.getTransId(trans)

    assert node0.waitForTransactionsInBlock([rotateSnapProvTransId], timeout=60), \
        "provider rotation transaction did not make it into a block before table check"

    providers = node0.getTableRows("sysio", "sysio", "snapprovs")
    assert len(providers) == 2, f"Expected 2 providers after rotation, got {len(providers)}"
    providerAccounts = {provider["value"]["snap_account"] for provider in providers}
    assert providerAccounts == {snapProv1.name, producerB}, \
        f"Unexpected providers after rotation: {providerAccounts}"

    Print("Test 2 PASSED: Provider account rotation replaces the old mapping")

    # ---------------------------------------------------------------
    Print("All snapshot attestation tests PASSED")
    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
