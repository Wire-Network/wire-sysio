#!/usr/bin/env python3

import json
import os
import signal
import socket
import urllib.request
import urllib.error
import urllib.parse

from TestHarness import Account, Cluster, TestHelper, Utils, WalletMgr
from TestHarness.testUtils import ReturnType
from TestHarness.TestHelper import AppArgs

###############################################################
# snapshot_api_test
#
#  Tests the snapshot_api_plugin HTTP endpoints and bootstrap
#  from a snapshot endpoint (--snapshot-endpoint CLI option).
#
#  API endpoints tests:
#   - /v1/snapshot/latest    — metadata of highest block snapshot
#   - /v1/snapshot/by_block  — metadata for specific block
#   - /v1/snapshot/download  — binary file download
#   - Range header support for partial downloads
#
#  bootstrap tests:
#   - Bootstrap node from snapshot endpoint
#   - Bootstrap with specific block number in URL
#   - Attestation verification after bootstrap
#
#  Cluster layout:
#    Node 0: producer with snapshot_api_plugin enabled
#    Node 1: producer
#    Node 2: non-producing node (bootstrap target)
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

def waitForAttestationRecord(node, blockNum, timeout=90):
    """Poll a node's sysio.snaprecords table until the attestation for ``blockNum`` appears.

    A snapshot is created first and attested afterwards, so the votesnaphash transaction
    lands in a block *after* the snapshot height. A node that bootstraps from that snapshot
    therefore does not have the attestation in its loaded state -- it must sync forward past
    the attestation block before the record becomes visible.

    A single ``waitForLibToAdvance`` only guarantees one finality round (a few blocks), which
    can fall far short of the attestation block when bootstrapping from an older snapshot
    while the chain head is already many blocks ahead. Poll for the record itself instead so
    the wait tracks the actual condition under test rather than a proxy that races it.

    Returns True once the record is found, False on timeout.
    """
    def recordPresent():
        records = node.getTableRows("sysio", "sysio", "snaprecords")
        return records is not None and any(r["value"]["block_num"] == blockNum for r in records)
    return Utils.waitForBool(recordPresent, timeout=timeout, sleepTime=1)

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

    Print("Stand up cluster with snapshot_api_plugin on node 0")
    if cluster.launch(pnodes=pnodes, totalNodes=totalNodes, prodCount=1,
                      maximumP2pPerHost=totalNodes, loadSystemContract=True,
                      activateIF=False,
                      specificExtraNodeopArgs={
                          0: "--plugin sysio::snapshot_api_plugin"
                      }) is False:
        errorExit("Failed to stand up cluster.")

    node0 = cluster.getNode(0)
    node1 = cluster.getNode(1)
    bootstrapNode = cluster.getNode(2)

    # ---------------------------------------------------------------
    # Setup: Register producers, snapshot providers, attestation config
    # ---------------------------------------------------------------
    Print("Create wallet and import keys")
    wallet = walletMgr.create('snapwallet')
    cluster.populateWallet(2, wallet)

    Print("Create test accounts for snapshot providers")
    cluster.createAccounts(cluster.sysioAccount, stakedDeposit=0)

    snapProv1 = cluster.accounts[0]
    Print(f"Snapshot provider account: {snapProv1.name}")

    assert node0.waitForLibToAdvance(timeout=30), "LIB did not advance after account creation"

    producerA = "defproducera"

    ignWallet = walletMgr.create("ignition")
    account = cluster.defProducerAccounts[producerA]
    walletMgr.importKey(account, ignWallet, ignoreDupKeyWarning=True)

    success, trans = node0.pushMessage("sysio", "regproducer",
        json.dumps({
            "producer": producerA,
            "producer_key": account.activePublicKey,
            "url": "",
            "location": 0
        }),
        f"--permission {producerA}@active")
    assert success, f"Failed to register producer {producerA}: {trans}"
    regProducerTransId = node0.getTransId(trans)

    # Wait for regproducer to be in a block before setrank reads the on-chain
    # producers table. waitForHeadToAdvance() does not guarantee this specific
    # transaction was applied -- with multiple producers a transaction pushed to
    # node0 can be forwarded into a peer's block -- which would make setrank fail
    # with "producer not found".
    assert node0.waitForTransactionInBlock(regProducerTransId, timeout=60), \
        "regproducer transaction did not make it into a block before setrank"

    success, trans = node0.pushMessage("sysio", "setrank",
        json.dumps({"producer": producerA, "rank": 1}),
        "--permission sysio@active")
    assert success, f"Failed to set rank for {producerA}: {trans}"

    assert node0.waitForHeadToAdvance(), "Head did not advance after setrank"

    success, trans = node0.pushMessage("sysio", "regsnapprov",
        json.dumps({"producer": producerA, "snap_account": snapProv1.name}),
        f"--permission {producerA}@active")
    assert success, f"Failed to register snapshot provider: {trans}"
    regSnapProvTransId = node0.getTransId(trans)

    # Set attestation config: min_providers=1, threshold_pct=50 (single vote = quorum)
    success, trans = node0.pushMessage("sysio", "setsnpcfg",
        json.dumps({"min_providers": 1, "threshold_pct": 50}),
        "--permission sysio@active")
    assert success, f"Failed to set snapshot config: {trans}"
    setCfgTransId = node0.getTransId(trans)

    # Ensure provider registration and config are applied in a block before the
    # snapshot/attestation flow below depends on them.
    assert node0.waitForTransactionsInBlock([regSnapProvTransId, setCfgTransId], timeout=60), \
        "regsnapprov/setsnpcfg transactions did not make it into a block"

    # ===================================================================
    # Snapshot API endpoint tests
    # ===================================================================

    # ---------------------------------------------------------------
    # Test 1: /v1/snapshot/latest returns 404 when no snapshots exist
    # ---------------------------------------------------------------
    Print("=== Test 1: /v1/snapshot/latest returns 404 with no snapshots ===")

    try:
        result = node0.processUrllibRequest("snapshot", "latest", silentErrors=True)
        if result is not None and result.get("code") == 404:
            Print("Got expected 404 for empty catalog")
        elif result is None:
            Print("Got expected error (no snapshots)")
        else:
            errorExit(f"Expected 404 or error for empty catalog, got: {result}")
    except urllib.error.HTTPError as e:
        assert e.code == 404, f"Expected 404, got {e.code}"
        Print("Got expected 404 for empty catalog")

    Print("Test 1 PASSED")

    # ---------------------------------------------------------------
    # Test 2: Create snapshot and query /v1/snapshot/latest
    # ---------------------------------------------------------------
    Print("=== Test 2: Create snapshot and query /v1/snapshot/latest ===")

    assert node0.waitForHeadToAdvance(blocksToAdvance=3), "Head did not advance"

    Print("Create snapshot on node 0 via producer API")
    ret = node0.createSnapshot()
    assert ret is not None, "Snapshot creation failed"
    snapInfo = ret["payload"]
    snapBlockNum = snapInfo["head_block_num"]
    snapBlockId = snapInfo["head_block_id"]
    snapRootHash = snapInfo["root_hash"]
    Print(f"Created snapshot: block_num={snapBlockNum}, root_hash={snapRootHash}")

    Print("Query /v1/snapshot/latest")
    result = node0.processUrllibRequest("snapshot", "latest")
    assert result is not None and result.get("code") == 200, \
        f"latest endpoint failed: {result}"
    meta = result["payload"]

    assert meta["block_num"] == snapBlockNum, \
        f"block_num mismatch: expected {snapBlockNum}, got {meta['block_num']}"
    assert meta["block_id"] == snapBlockId, \
        f"block_id mismatch: expected {snapBlockId}, got {meta['block_id']}"
    assert meta["root_hash"] == snapRootHash, \
        f"root_hash mismatch: expected {snapRootHash}, got {meta['root_hash']}"

    Print("Test 2 PASSED")

    # ---------------------------------------------------------------
    # Test 3: /v1/snapshot/by_block
    # ---------------------------------------------------------------
    Print("=== Test 3: /v1/snapshot/by_block ===")

    result = node0.processUrllibRequest("snapshot", "by_block",
                                         payload={"block_num": snapBlockNum})
    assert result is not None and result.get("code") == 200, \
        f"by_block endpoint failed: {result}"
    meta = result["payload"]

    assert meta["block_num"] == snapBlockNum
    assert meta["root_hash"] == snapRootHash

    Print("Test 3 PASSED")

    # ---------------------------------------------------------------
    # Test 4: /v1/snapshot/by_block returns 404 for non-existent block
    # ---------------------------------------------------------------
    Print("=== Test 4: /v1/snapshot/by_block 404 ===")

    try:
        result = node0.processUrllibRequest("snapshot", "by_block",
                                             payload={"block_num": 999999},
                                             silentErrors=True)
        assert result is None or result.get("code") == 404, \
            f"Expected 404 for non-existent block, got: {result}"
    except urllib.error.HTTPError as e:
        assert e.code == 404, f"Expected 404, got {e.code}"

    Print("Test 4 PASSED")

    # ---------------------------------------------------------------
    # Test 5: /v1/snapshot/download serves the snapshot file
    # ---------------------------------------------------------------
    Print("=== Test 5: /v1/snapshot/download ===")

    downloadUrl = f"{node0.endpointHttp}/v1/snapshot/download"
    payload = json.dumps({"block_num": snapBlockNum}).encode()

    req = urllib.request.Request(downloadUrl, data=payload, method="POST")
    req.add_header("Content-Type", "application/json")

    with urllib.request.urlopen(req) as response:
        assert response.getcode() == 200, f"Expected 200, got {response.getcode()}"

        contentType = response.getheader("Content-Type")
        assert contentType == "application/octet-stream", f"Unexpected Content-Type: {contentType}"
        assert response.getheader("Accept-Ranges") == "bytes"
        assert "attachment" in (response.getheader("Content-Disposition") or "")

        downloadedData = response.read()
        contentLength = int(response.getheader("Content-Length"))
        assert len(downloadedData) == contentLength

    # Verify against on-disk snapshot
    snapFile = node0.getLatestSnapshot()
    with open(snapFile, "rb") as f:
        diskData = f.read()

    assert downloadedData == diskData, "Downloaded data does not match on-disk snapshot"

    Print(f"Test 5 PASSED (downloaded {len(downloadedData)} bytes)")

    # ---------------------------------------------------------------
    # Test 6: Range header support
    # ---------------------------------------------------------------
    Print("=== Test 6: Range header partial download ===")

    fileSize = len(diskData)
    rangeStart = 0
    rangeEnd = 1023

    req = urllib.request.Request(downloadUrl, data=payload, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Range", f"bytes={rangeStart}-{rangeEnd}")

    with urllib.request.urlopen(req) as response:
        assert response.getcode() == 206, f"Expected 206, got {response.getcode()}"
        contentRange = response.getheader("Content-Range")
        expectedRange = f"bytes {rangeStart}-{rangeEnd}/{fileSize}"
        assert contentRange == expectedRange, f"Content-Range: expected '{expectedRange}', got '{contentRange}'"

        partialData = response.read()
        assert len(partialData) == (rangeEnd - rangeStart + 1)
        assert partialData == diskData[rangeStart:rangeEnd+1]

    # Test 6b: mid-file range. A range ending before EOF must return exactly the
    # requested bytes; a server that streams from the seek position to EOF would
    # return the correct prefix but over-send, which Test 6c detects.
    rangeStart = fileSize // 2
    rangeEnd = rangeStart + 4095
    assert rangeEnd < fileSize - 1, f"snapshot too small ({fileSize} bytes) for mid-file range test"

    req = urllib.request.Request(downloadUrl, data=payload, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Range", f"bytes={rangeStart}-{rangeEnd}")

    with urllib.request.urlopen(req) as response:
        assert response.getcode() == 206, f"Expected 206, got {response.getcode()}"
        contentRange = response.getheader("Content-Range")
        expectedRange = f"bytes {rangeStart}-{rangeEnd}/{fileSize}"
        assert contentRange == expectedRange, f"Content-Range: expected '{expectedRange}', got '{contentRange}'"

        partialData = response.read()
        assert len(partialData) == (rangeEnd - rangeStart + 1)
        assert partialData == diskData[rangeStart:rangeEnd+1]

    # Test 6c: over-send detection. urllib stops reading at Content-Length, so it
    # cannot tell whether the server wrote bytes past the advertised range. Issue
    # the same mid-file range request on a raw socket, read until the server closes
    # the connection, and verify the body is exactly the advertised length.
    parsed = urllib.parse.urlparse(node0.endpointHttp)
    rawRequest = (
        f"POST /v1/snapshot/download HTTP/1.1\r\n"
        f"Host: {parsed.hostname}:{parsed.port}\r\n"
        f"Content-Type: application/json\r\n"
        f"Content-Length: {len(payload)}\r\n"
        f"Range: bytes={rangeStart}-{rangeEnd}\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    ).encode() + payload

    with socket.create_connection((parsed.hostname, parsed.port), timeout=60) as sock:
        sock.sendall(rawRequest)
        rawResponse = b""
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            rawResponse += chunk

    headerEnd = rawResponse.find(b"\r\n\r\n")
    assert headerEnd != -1, "No header terminator in raw range response"
    statusLine = rawResponse[:rawResponse.find(b"\r\n")].decode()
    assert " 206 " in statusLine, f"Expected 206 status in raw response, got: {statusLine}"
    rawBody = rawResponse[headerEnd + 4:]
    expectedLen = rangeEnd - rangeStart + 1
    assert len(rawBody) == expectedLen, \
        f"Server over-sent range response: {len(rawBody)} body bytes on the wire, expected {expectedLen}"
    assert rawBody == diskData[rangeStart:rangeEnd+1]

    # Test 6d: a range end past EOF is clamped to the last byte of the file
    rangeStart = fileSize - 100
    req = urllib.request.Request(downloadUrl, data=payload, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Range", f"bytes={rangeStart}-{fileSize + 1000}")

    with urllib.request.urlopen(req) as response:
        assert response.getcode() == 206, f"Expected 206, got {response.getcode()}"
        contentRange = response.getheader("Content-Range")
        expectedRange = f"bytes {rangeStart}-{fileSize - 1}/{fileSize}"
        assert contentRange == expectedRange, f"Content-Range: expected '{expectedRange}', got '{contentRange}'"

        partialData = response.read()
        assert partialData == diskData[rangeStart:]

    Print("Test 6 PASSED")

    # ---------------------------------------------------------------
    # Test 7: Second snapshot updates catalog
    # ---------------------------------------------------------------
    Print("=== Test 7: Second snapshot updates catalog ===")

    assert node0.waitForHeadToAdvance(blocksToAdvance=3), "Head did not advance"

    ret2 = node0.createSnapshot()
    assert ret2 is not None, "Second snapshot creation failed"
    snap2Info = ret2["payload"]
    snap2BlockNum = snap2Info["head_block_num"]
    snap2RootHash = snap2Info["root_hash"]
    assert snap2BlockNum > snapBlockNum

    result = node0.processUrllibRequest("snapshot", "latest")
    assert result is not None and result["payload"]["block_num"] == snap2BlockNum

    # First snapshot still accessible
    result = node0.processUrllibRequest("snapshot", "by_block",
                                         payload={"block_num": snapBlockNum})
    assert result is not None and result["payload"]["block_num"] == snapBlockNum

    Print("Test 7 PASSED")

    # ===================================================================
    # Bootstrap from snapshot endpoint
    # ===================================================================
    #
    # Real-world flow: snapshot is taken first, then attested afterwards.
    # The attestation transaction lands in a block AFTER the snapshot.
    # The bootstrap node loads from the snapshot, syncs forward, and
    # eventually reaches the block containing the attestation. The
    # verify_snapshot_attestation check retries on each irreversible block
    # until it finds the record; for auto-fetched snapshots a record still
    # missing after a grace window past the snapshot height is fatal, while
    # manual --snapshot downgrades to a warning at the chain tip.

    # ---------------------------------------------------------------
    # Test 8: Bootstrap from snapshot endpoint (latest)
    # ---------------------------------------------------------------
    Print("=== Test 8: Bootstrap from snapshot endpoint ===")

    # Use snap2 (the second snapshot from test 7) — attest it now.
    # The attestation lands in a block after snap2BlockNum.
    snap2BlockId = snap2Info["head_block_id"]
    Print(f"Submit votesnaphash for snapshot at block {snap2BlockNum}")
    success, trans = node0.pushMessage("sysio", "votesnaphash",
        json.dumps({
            "snap_account": snapProv1.name,
            "block_id": snap2BlockId,
            "snapshot_hash": snap2RootHash
        }),
        f"--permission {snapProv1.name}@active")
    assert success, f"Failed to submit snapshot vote: {trans}"

    assert node0.waitForHeadToAdvance(), "Head did not advance after vote"

    # KV table rows are returned as {"key": {...}, "value": {...}}
    records = node0.getTableRows("sysio", "sysio", "snaprecords")
    found = any(r["value"]["block_num"] == snap2BlockNum for r in records)
    assert found, f"Attestation record not created for block {snap2BlockNum}"

    # Wait for attestation to become irreversible so the bootstrap node
    # will see it when syncing
    assert node0.waitForLibToAdvance(timeout=30), "LIB did not advance after attestation"

    # Kill and wipe bootstrap node
    Print("Kill and wipe bootstrap node (node 2)")
    bootstrapNode.kill(signal.SIGTERM)
    bootstrapNode.removeDataDir(rmBlocks=True)

    endpointUrl = node0.endpointHttp
    Print(f"Restart bootstrap node with --snapshot-endpoint {endpointUrl}")

    # Fetches latest snapshot (snap2). The attestation is NOT in the snapshot —
    # it's in blocks after snap2BlockNum. The bootstrap node syncs forward and
    # finds the attestation record once it reaches those blocks.
    isRelaunchSuccess = bootstrapNode.relaunch(
        chainArg=f"--delete-all-blocks --snapshot-endpoint {endpointUrl}")
    assert isRelaunchSuccess, "Failed to relaunch bootstrap node from snapshot endpoint"

    # The attestation is in blocks after the snapshot height, so wait for the bootstrap
    # node to sync forward until the record is visible (not just one LIB advance).
    Print("Wait for bootstrap node to sync past the attestation block")
    assert waitForAttestationRecord(bootstrapNode, snap2BlockNum), \
        f"Attestation for block {snap2BlockNum} not found on bootstrap node"

    Print("Test 8 PASSED")

    # ---------------------------------------------------------------
    # Test 9: Bootstrap with specific block number in URL
    # ---------------------------------------------------------------
    Print("=== Test 9: Bootstrap with specific block number ===")

    # Also attest the first snapshot so we can bootstrap from it
    Print(f"Submit votesnaphash for first snapshot at block {snapBlockNum}")
    success, trans = node0.pushMessage("sysio", "votesnaphash",
        json.dumps({
            "snap_account": snapProv1.name,
            "block_id": snapBlockId,
            "snapshot_hash": snapRootHash
        }),
        f"--permission {snapProv1.name}@active")
    assert success, f"Failed to attest first snapshot: {trans}"

    assert node0.waitForLibToAdvance(timeout=30), "LIB did not advance"

    # Kill and wipe bootstrap node again
    bootstrapNode.kill(signal.SIGTERM)
    bootstrapNode.removeDataDir(rmBlocks=True)

    # Bootstrap requesting the FIRST snapshot by block number in URL.
    # Use addSwapFlags to replace --snapshot-endpoint value (avoid duplicate flags
    # from the previous relaunch which stored the modified cmd).
    endpointUrlWithBlock = f"{endpointUrl}/{snapBlockNum}"
    Print(f"Restart with --snapshot-endpoint {endpointUrlWithBlock}")

    isRelaunchSuccess = bootstrapNode.relaunch(
        addSwapFlags={"--snapshot-endpoint": endpointUrlWithBlock})
    assert isRelaunchSuccess, "Failed to relaunch with specific block number"

    # Bootstrapped from the FIRST (older) snapshot, so the attestation block is many
    # blocks ahead of the loaded state -- a single LIB advance is not enough. Poll for
    # the record until the node syncs forward to it.
    Print("Wait for bootstrap node to sync past the attestation block")
    assert waitForAttestationRecord(bootstrapNode, snapBlockNum), \
        "First snapshot attestation not found after specific-block bootstrap"

    Print("Test 9 PASSED")

    # ---------------------------------------------------------------
    # Test 10: max-bytes-in-flight admission control on raw downloads
    # ---------------------------------------------------------------
    # Raw file downloads must obey the same http-max-bytes-in-flight cap as
    # JSON responses: an unranged download larger than the cap is rejected
    # with 503 before anything is sent, while admission control sees the
    # CLAMPED payload length, so an absurd raw Range end on a small tail
    # window still passes.
    Print("=== Test 10: download admission control (http-max-bytes-in-flight) ===")

    assert fileSize > 1024 * 1024, \
        f"snapshot ({fileSize} bytes) must exceed the 1 MiB cap for this test"

    Print("Relaunch API node with --http-max-bytes-in-flight-mb 1")
    node0.kill(signal.SIGTERM)
    isRelaunchSuccess = node0.relaunch(addSwapFlags={"--http-max-bytes-in-flight-mb": "1"})
    assert isRelaunchSuccess, "Failed to relaunch API node with reduced in-flight cap"
    assert node0.waitForLibToAdvance(timeout=60), "LIB did not advance after API node relaunch"

    # Full-file download exceeds the 1 MiB cap: expect the standard 503 busy
    # rejection instead of a streamed body.
    req = urllib.request.Request(downloadUrl, data=payload, method="POST")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req) as response:
            assert False, f"Expected 503 over-cap rejection, got {response.getcode()}"
    except urllib.error.HTTPError as e:
        assert e.code == 503, f"Expected 503 over-cap rejection, got {e.code}"

    # A raw Range end far past EOF clamps to a tiny tail window, and admission
    # control must track the clamped length -- not the raw request -- so this
    # succeeds under the same 1 MiB cap.
    rangeStart = fileSize - 4096
    req = urllib.request.Request(downloadUrl, data=payload, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Range", f"bytes={rangeStart}-18446744073709551615")

    with urllib.request.urlopen(req) as response:
        assert response.getcode() == 206, f"Expected 206, got {response.getcode()}"
        contentRange = response.getheader("Content-Range")
        expectedRange = f"bytes {rangeStart}-{fileSize - 1}/{fileSize}"
        assert contentRange == expectedRange, f"Content-Range: expected '{expectedRange}', got '{contentRange}'"
        partialData = response.read()
        assert partialData == diskData[rangeStart:]

    Print("Test 10 PASSED")

    # ---------------------------------------------------------------
    Print("All snapshot API and bootstrap tests PASSED")
    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
