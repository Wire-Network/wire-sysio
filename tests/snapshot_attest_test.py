#!/usr/bin/env python3

"""Exercise provider registration and the scheduled-height attestation boundary."""

import json
import signal

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from TestHarness.TestHelper import AppArgs

###############################################################
# snapshot_attest_test
#
#  Tests on-chain snapshot attestation via sysio.system contract.
#  This test validates the snapshot serving pipeline:
#  - Snapshot provider registration and account rotation
#  - Manual snapshot creation
#  - Rejection of votes outside the configured block-height cadence
#
#  Successful fixed-K formation and attestation-record persistence use controlled
#  scheduled heights in the C++ contract and unit test suites.
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
# Small supported cadence configured through the same contract path used in production.
snapshotAttestationBlockSpacing=250
snapshotProviderDescription="snapshot-provider auto"
providerTestBlockSpacing=17
updatedProviderBlockSpacing=19
manualSnapshotDescription="operator manual cadence test"
manualSnapshotHeight=2**32-1


def setSnapshotCadence(node, blockSpacing):
    """Configure the cadence and wait until a provider can restart from irreversible state."""
    success, trans = node.pushMessage("sysio", "setsnpcfg",
        json.dumps({"min_providers": 1, "block_spacing": blockSpacing}),
        "--permission sysio@active")
    assert success, f"Failed to set snapshot config: {trans}"
    transId = node.getTransId(trans)
    assert node.waitForTransFinalization(transId, timeout=60), "Snapshot config was not finalized"
    return transId


def snapshotRequests(node):
    """Read the live provider/manual schedule without changing it."""
    result = node.processUrllibRequest("producer", "get_snapshot_requests")
    assert result is not None, "Failed to read snapshot requests"
    return result["payload"]["snapshot_requests"]


def providerSchedule(node):
    """Require exactly one canonical provider-owned request."""
    requests = [request for request in snapshotRequests(node)
                if request["snapshot_description"] == snapshotProviderDescription]
    assert len(requests) == 1, f"Expected one automatic snapshot request, got {requests}"
    return requests[0]

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

    # Register producers via regproducer (required before regfinkey)
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

    # regfinkey asserts "is not a registered producer" if the producers row is missing.
    # pushMessage only confirms speculative execution, and waitForHeadToAdvance() does not
    # guarantee these specific transactions were applied — with multiple producers a transaction
    # pushed to node0 can be forwarded into a peer's block. Wait for both regproducer
    # transactions to appear in a block first.
    assert node0.waitForTransactionsInBlock(regProducerTransIds, timeout=60), \
        "regproducer transactions did not make it into a block before regfinkey"

    # Snapshot-provider eligibility is POSITION in the score-ordered producer index, not a rank
    # governance assigns. A producer holds a position only when it is schedulable, which requires
    # an active finalizer key on top of the opreg operator row the bootstrap already created.
    finKeyTransIds = []
    for name in [producerA, producerB]:
        node = next(cluster.getNode(i) for i in range(pnodes)
                    if cluster.getNode(i).producerName == name)
        Print(f"Register finalizer key for {name}")
        success, trans = node0.pushMessage("sysio", "regfinkey",
            json.dumps({
                "finalizer_name": name,
                "finalizer_key": node.keys[0].blspubkey,
                "proof_of_possession": node.keys[0].blspop
            }),
            f"--permission {name}@active")
        assert success, f"Failed to register finalizer key for {name}: {trans}"
        finKeyTransIds.append(node0.getTransId(trans))

    # regsnapprov walks the producer index for its eligibility check, so wait for those
    # registrations specifically rather than a generic head advance.
    assert node0.waitForTransactionsInBlock(finKeyTransIds, timeout=60), \
        "regfinkey transactions did not make it into a block before regsnapprov"

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
    # Set attestation config: fixed K=1 and a small block-height cadence.
    # This confirms governance configuration independently of the cadence rejection below.
    # ---------------------------------------------------------------
    Print("Set snapshot attestation config")
    setCfgTransId = setSnapshotCadence(node0, snapshotAttestationBlockSpacing)

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

    # Avoid making the assertion probabilistic now that the test cadence is deliberately small.
    # If the first manual snapshot happens to land on the cadence, advance LIB and retry once.
    for attempt in range(2):
        ret = node0.createSnapshot()
        assert ret is not None, "Snapshot creation failed"
        snapInfo = ret["payload"]
        snapBlockNum = snapInfo["head_block_num"]
        snapBlockId = snapInfo["head_block_id"]
        snapRootHash = snapInfo["root_hash"]
        if snapBlockNum % snapshotAttestationBlockSpacing != 0:
            break
        assert attempt == 0, "Manual snapshots repeatedly landed on the configured cadence"
        assert node0.waitForLibToAdvance(timeout=30), \
            "LIB did not advance before retrying the manual snapshot"
    Print(f"Manual snapshot: block_num={snapBlockNum}, root_hash={snapRootHash}")

    assert snapBlockNum % snapshotAttestationBlockSpacing != 0, (
        "Wall-clock integration snapshot unexpectedly landed on the configured cadence"
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
    # Test 3: Provider and API cadence is fixed at startup until a restart.
    # ---------------------------------------------------------------
    Print("=== Test 3: Startup-only provider cadence and restart ===")
    providerNode = cluster.getNode(2)
    configuredTransId = setSnapshotCadence(node0, providerTestBlockSpacing)
    assert providerNode.waitForTransFinalization(configuredTransId, timeout=60), \
        "Provider did not receive finalized snapshot configuration"
    providerNode.kill(signal.SIGTERM)
    assert providerNode.relaunch(
        chainArg=f"--snapshot-provider-account {snapProv1.name} --plugin sysio::snapshot_api_plugin"), \
        "Provider failed to start after snapshot configuration"
    schedule = providerSchedule(providerNode)
    assert schedule["block_spacing"] == providerTestBlockSpacing
    assert schedule["start_block_num"] == providerTestBlockSpacing

    # An actual finalized file demonstrates that the startup schedule reaches the API catalog.
    snapshotHeight = (providerNode.getInfo()["head_block_num"] // providerTestBlockSpacing + 1) \
        * providerTestBlockSpacing
    assert Utils.waitForBool(
        lambda: providerNode.processUrllibRequest("snapshot", "by_block",
            payload={"block_num": snapshotHeight}, silentErrors=True) is not None,
        timeout=90), "Provider did not finalize its scheduled snapshot"

    changedTransId = setSnapshotCadence(node0, updatedProviderBlockSpacing)
    assert providerNode.waitForTransFinalization(changedTransId, timeout=60)
    assert providerSchedule(providerNode)["block_spacing"] == providerTestBlockSpacing, \
        "Provider unexpectedly refreshed cadence without restarting"

    # A manual request must survive replacing only the differently-spaced automatic request.
    manual = providerNode.processUrllibRequest("producer", "schedule_snapshot", payload={
        "start_block_num": manualSnapshotHeight,
        "end_block_num": manualSnapshotHeight,
        "snapshot_description": manualSnapshotDescription
    })
    assert manual is not None, "Manual snapshot schedule failed"
    manualRequestId = manual["payload"]["snapshot_request_id"]
    providerNode.kill(signal.SIGTERM)
    assert providerNode.relaunch(), "Provider failed to restart with changed snapshot cadence"
    schedule = providerSchedule(providerNode)
    assert schedule["block_spacing"] == updatedProviderBlockSpacing
    assert schedule["start_block_num"] == updatedProviderBlockSpacing
    requests = snapshotRequests(providerNode)
    assert len(requests) == 2, f"Unexpected requests after cadence restart: {requests}"
    assert any(request["snapshot_request_id"] == manualRequestId
               and request["snapshot_description"] == manualSnapshotDescription
               for request in requests), "Restart removed the operator's manual request"
    providerNode.kill(signal.SIGTERM)
    assert providerNode.relaunch(), "Provider failed to restart with an unchanged snapshot cadence"
    assert providerSchedule(providerNode)["snapshot_request_id"] == schedule["snapshot_request_id"], \
        "Unchanged restart did not reuse the automatic request"
    assert len(snapshotRequests(providerNode)) == 2, "Unchanged restart duplicated a request"
    Print("Test 3 PASSED: Provider cadence changes on restart and scheduled files enter the catalog")

    # ---------------------------------------------------------------
    Print("All snapshot attestation tests PASSED")
    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
