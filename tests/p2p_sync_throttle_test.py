#!/usr/bin/env python3

import json
import signal
import struct
import time
import urllib.request
from pathlib import Path

from TestHarness import Cluster, TestHelper, Utils, WalletMgr, CORE_SYMBOL, createAccountKeys
from TestHarness.Node import BlockType

###############################################################
# p2p_sync_throttle_test
#
# A rate limit on a p2p-listen-endpoint paces the blocks a peer syncs through it. The limit is a token bucket holding
# burstWindowSec of the limit, and every block sent under it is charged, so the blocks sent before the last one exceed
# the limit over the time between the first and last send by at most one burst:
#
#    T >= (B - burst) / limit
#
# T is measured on the syncing node and B is read from a block log.
#
# Topology:
#    prodNode <-> nonProdNode <-> throttlingNode <-> throttledNode    (limited listener)
#                                                <-> unThrottledNode  (unthrottledRate listener)
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

args=TestHelper.parse_args({"-d","--keep-logs","--activate-if"
                            ,"--dump-error-details","-v","--leave-running"
                            ,"--unshared"})
pnodes=1
delay=args.d
debug=args.v
prod_count = 2
total_nodes=5
activateIF=args.activate_if
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

prodNodeId = 0
syncFetchSpan = 5                   # blocks per sync request, so the limit has to hold across many requests
trxGenTps = 500
trxGenDurationSec = 60
trxGenTerminatedEarly = -3 % 256    # exit status of trx_generator's TERMINATED_EARLY, when it falls behind its TPS
firstSyncedBlockNum = 2             # every node starts with the genesis block
minPayloadBytes = 500 * 1000        # bytes synced under the limit, for the timing check to mean anything
targetSyncSec = 45                  # payload / limit
minSyncSec = 10                     # the largest-block floor on the limit can hold a small payload under targetSyncSec
maxBlockGapSec = 2                  # block_sync_rate_limiter::max_send_gap: no charge overdraws more of the limit
burstWindowSec = 1                  # block_sync_rate_limiter::burst_window
blockFramingBytes = 5               # length prefix and message type around a block sent to a peer
pollIntervalSec = 0.25
syncTimeoutSec = 4 * targetSyncSec
unthrottledRate = "1TB/s"
throttleLogText = "throttling block sync"
blockLogPreamble = struct.Struct("<II")   # version, first block number
blockLogPosition = struct.Struct("<Q")    # a blocks.index entry, also the trailer of each blocks.log entry

def blockLogSizes(node, first, last):
    """Total and largest size, in bytes, of the packed blocks first..last in node's block log."""
    blocksDir = Path(node.data_dir) / "blocks"
    with open(blocksDir / "blocks.log", "rb") as log:
        _, firstLogBlockNum = blockLogPreamble.unpack(log.read(blockLogPreamble.size))
    positionCount = last - first + 2
    with open(blocksDir / "blocks.index", "rb") as index:
        index.seek((first - firstLogBlockNum) * blockLogPosition.size)
        positions = [pos for (pos,) in blockLogPosition.iter_unpack(index.read(positionCount * blockLogPosition.size))]
    assert len(positions) == positionCount, f"block log of node {node.nodeId} ends before block {last + 1}"
    sizes = [end - begin - blockLogPosition.size for begin, end in zip(positions, positions[1:])]
    return sum(sizes), max(sizes)

def headBlockNum(node):
    """Head block number of node, or None while its HTTP endpoint does not answer."""
    request = urllib.request.Request(f"{node.endpointHttp}/v1/chain/get_info", method="POST")
    try:
        with urllib.request.urlopen(request, timeout=1) as response:
            return json.load(response)["head_block_num"]
    except (OSError, ValueError, KeyError):
        return None

def throttleLogLines(node, peerP2pPort):
    """Lines of node's log that report throttling block sync to the peer listening on peerP2pPort."""
    with open(Path(node.data_dir) / "stderr.txt", errors="replace") as log:
        return [line for line in log if throttleLogText in line and f":{peerP2pPort} - " in line]

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True)

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)

    accounts=createAccountKeys(2)
    if accounts is None:
        Utils.errorExit("FAILURE - create keys")
    accounts[0].name="tester111111"
    accounts[1].name="tester222222"

    Print(f'producing nodes: {pnodes}, delay between nodes launch: {delay} second{"s" if delay != 1 else ""}')

    Print("Stand up cluster")
    # The generator's accounts have no CPU of their own, so subjective billing would reject most of its transactions.
    noSubjectiveBilling = " ".join(f"--disable-subjective-account-billing {account.name}" for account in accounts)
    if cluster.launch(pnodes=pnodes, unstartedNodes=3, totalNodes=total_nodes, prodCount=prod_count,
                      extraNodeopArgs=f"--sync-fetch-span {syncFetchSpan}",
                      specificExtraNodeopArgs={prodNodeId: noSubjectiveBilling},
                      topo='./tests/p2p_sync_throttle_test_shape.json', delay=delay, activateIF=activateIF) is False:
        errorExit("Failed to stand up sys cluster.")

    prodNode = cluster.getNode(prodNodeId)
    nonProdNode = cluster.getNode(1)

    testWalletName="test"
    Print("Creating wallet \"%s\"." % (testWalletName))
    testWallet=walletMgr.create(testWalletName, [cluster.sysioAccount,accounts[0],accounts[1]])

    # create accounts via sysio as otherwise a bid is needed
    for account in accounts:
        Print("Create new account %s via %s" % (account.name, cluster.sysioAccount.name))
        trans=nonProdNode.createInitializeAccount(account, cluster.sysioAccount, stakedDeposit=0, waitForTransBlock=True, stakeNet=1000, stakeCPU=1000, buyRAM=1000, exitOnError=True)
        transferAmount="100000000.0000 {0}".format(CORE_SYMBOL)
        Print("Transfer funds %s from account %s to %s" % (transferAmount, cluster.sysioAccount.name, account.name))
        nonProdNode.transferFunds(cluster.sysioAccount, account, transferAmount, "test transfer", waitForTransBlock=True)

    Print("Configure and launch txn generators")
    cluster.launchTrxGenerators(contractOwnerAcctName=cluster.noopAccount.name,
                                acctNamesList=[account.name for account in accounts],
                                acctPrivKeysList=[account.activePrivateKey for account in accounts], nodeId=prodNode.nodeId,
                                tpsPerGenerator=trxGenTps, numGenerators=1, durationSec=trxGenDurationSec, waitToComplete=True)
    # A generator that falls behind its TPS on a loaded host quits early; the payload check below covers that.
    trxGenExitCodes = [generator.returncode for generator in cluster.trxGenLauncher.subprocess_ret_codes]
    if trxGenTerminatedEarly in trxGenExitCodes:
        Print(f"trx generator terminated early; see trx_generator_*.txt in {Utils.DataDir}")
    assert all(code in (0, trxGenTerminatedEarly) for code in trxGenExitCodes), \
        f"trx generator exited with {trxGenExitCodes}; see trx_generator_*.txt in {Utils.DataDir}"

    targetBlockNum = nonProdNode.getHeadBlockNum()
    assert nonProdNode.waitForBlock(targetBlockNum, blockType=BlockType.lib), \
        f"block {targetBlockNum} did not become irreversible on node {nonProdNode.nodeId}"
    # The limit paces the blocks sent before the target block; the target block itself may overdraw it.
    payloadBytes, maxBlockBytes = blockLogSizes(nonProdNode, firstSyncedBlockNum, targetBlockNum - 1)
    assert payloadBytes >= minPayloadBytes, \
        f"blocks {firstSyncedBlockNum}-{targetBlockNum - 1} hold {payloadBytes} bytes, expected at least {minPayloadBytes}"
    # No block may reach max_send_gap of the limit, so every block is charged in full and the bound below is exact.
    maxBlockMessageBytes = maxBlockBytes + blockFramingBytes
    limit = max(payloadBytes // targetSyncSec, -(-maxBlockMessageBytes // maxBlockGapSec))
    assert maxBlockMessageBytes <= limit * maxBlockGapSec, f"block of {maxBlockBytes} bytes exceeds the send gap"
    burstBytes = limit * burstWindowSec
    minThrottledSyncSec = (payloadBytes - burstBytes) / limit
    Print(f"blocks {firstSyncedBlockNum}-{targetBlockNum - 1}: {payloadBytes} bytes, largest {maxBlockBytes} bytes; "
          f"limit {limit} bytes/sec, minimum throttled sync {minThrottledSyncSec:.2f} s")
    assert minThrottledSyncSec >= minSyncSec, f"throttled sync of {minThrottledSyncSec:.2f} s is too short to measure"

    throttlingNode, throttledNode, unThrottledNode = cluster.unstartedNodes
    i = throttlingNode.cmd.index('--p2p-listen-endpoint')
    throttleListenAddr = throttlingNode.cmd[i+1]
    throttlingNode.cmd[i+1] = f'{throttleListenAddr}:{limit}B/s'
    throttleListenIP = throttleListenAddr.split(':')[0]
    unThrottleListenAddr = f'{throttleListenIP}:{Utils.getPort(Utils.PortAlternateP2P, 0)}'
    throttlingNode.cmd += ['--p2p-listen-endpoint', f'{unThrottleListenAddr}:{unthrottledRate}']

    cluster.biosNode.kill(signal.SIGTERM)

    Print("Launch throttling node")
    cluster.launchUnstarted(1)
    assert throttlingNode.verifyAlive(), "throttling node did not launch"

    # Throttling node was offline during block generation and once online receives blocks as fast as possible
    assert throttlingNode.waitForBlock(targetBlockNum, timeout=targetBlockNum/2), \
        f'wait for block {targetBlockNum} on throttling node timed out'

    throttledNode.cmd += ['--p2p-peer-address', throttleListenAddr]
    unThrottledNode.cmd += ['--p2p-peer-address', unThrottleListenAddr]

    Print("Launch throttled and un-throttled nodes")
    syncNodes = {"throttled": throttledNode, "un-throttled": unThrottledNode}
    launchTime = time.monotonic()
    cluster.launchUnstarted(2, waitForAlive=False)
    for name, node in syncNodes.items():
        assert node.verifyAlive(), f"{name} node did not launch"

    # Poll both nodes until each holds the target block, noting when each first holds its first synced block.
    syncStart = {}
    syncEnd = {}
    deadline = launchTime + syncTimeoutSec
    while len(syncEnd) < len(syncNodes):
        assert time.monotonic() < deadline, \
            f"{sorted(set(syncNodes) - set(syncEnd))} did not sync block {targetBlockNum} within {syncTimeoutSec} s"
        for name, node in syncNodes.items():
            if name in syncEnd:
                continue
            assert node.verifyAlive(), f"{name} node exited while syncing"
            head = headBlockNum(node)
            now = time.monotonic()
            if head is not None and head >= firstSyncedBlockNum:
                syncStart.setdefault(name, now)
            if head is not None and head >= targetBlockNum:
                syncEnd[name] = now
        time.sleep(pollIntervalSec)

    for name in syncNodes:
        fromLaunch = syncEnd[name] - launchTime
        fromSyncStart = syncEnd[name] - syncStart[name]
        Print(f"{name} sync of blocks {firstSyncedBlockNum}-{targetBlockNum}: {fromLaunch:.2f} s from launch, "
              f"{fromSyncStart:.2f} s from block {firstSyncedBlockNum}, "
              f"{payloadBytes / max(fromSyncStart, pollIntervalSec):.0f} bytes/sec")

    # The throttled node's first block was sent after its launch, and its target block before the poll that saw it,
    # so the time from launch is no shorter than the time between those sends.
    throttledSyncSec = syncEnd["throttled"] - launchTime
    assert throttledSyncSec >= minThrottledSyncSec, \
        f"throttled node synced {payloadBytes} bytes in {throttledSyncSec:.2f} s, " \
        f"under the {minThrottledSyncSec:.2f} s a {limit} bytes/sec limit allows"

    throttledLines = throttleLogLines(throttlingNode, cluster.getNodeP2pPort(throttledNode.nodeId))
    unThrottledLines = throttleLogLines(throttlingNode, cluster.getNodeP2pPort(unThrottledNode.nodeId))
    Print(f"throttling node log: {len(throttledLines)} throttle lines for the throttled node, "
          f"{len(unThrottledLines)} for the un-throttled node")
    assert len(throttledLines) > 0, "throttling node never logged throttling the throttled node"
    assert len(unThrottledLines) == 0, f"throttling node throttled the un-throttled node: {unThrottledLines[0]}"

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
