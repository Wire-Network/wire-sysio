#!/usr/bin/env python3
import os
import re
import shutil
import signal
import time
from datetime import datetime, timedelta
from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from TestHarness.Node import BlockType

###############################################################
# nodeop_late_block_test
#
# Set up a cluster of 4 producer nodes so that 3 can reach consensus.
# Node_00 - defproducera,b,c
# Node_01 - defproducerd,e,f
# Node_02 - defproducerg,h,i
#  Node_04 - bridge between 2 & 3
# Node_03 - defproducerj,k,l
#
# When Node_02 is producing shutdown Node_04 and bring it back up when Node_03 is producing.
# Verify that Node_03 realizes it should switch over to fork other nodes have chosen.
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

args=TestHelper.parse_args({"-d","--keep-logs","--dump-error-details","-v","--leave-running","--unshared"})
pnodes=4
total_nodes=pnodes + 1
canonicalConvergenceTimeout=90
expectedIProducerBlocks=9
forkSwitchLogTimeout=30
producerSlotBlockCount=12
bridgeReconnectProducer="defproducerk"
node3ProducerNames=("defproducerj", "defproducerk", "defproducerl")
lockoutFastPathLogPrefix="applying blocks while producing: head's branch is locked out"
delay=args.d
debug=args.v
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True, keepRunning=args.leave_running, keepLogs=args.keep_logs)

try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    Print(f'producing nodes: {pnodes}, delay between nodes launch: {delay} second{"s" if delay != 1 else ""}')

    Print("Stand up cluster")
    # do not allow pause production to interfere with late block test
    extraNodeopArgs=" --production-pause-vote-timeout-ms 0 "

    if cluster.launch(pnodes=pnodes, totalNodes=total_nodes, extraNodeopArgs=extraNodeopArgs,
                      topo="./tests/nodeop_late_block_test_shape.json", delay=delay, loadSystemContract=False,
                      biosFinalizer=False,
                      activateIF=True, signatureProviderForNonProducer=True) is False:
        errorExit("Failed to stand up sys cluster.")

    assert cluster.biosNode.getInfo(exitOnError=True)["head_block_producer"] != "sysio", "launch should have waited for production to change"
    cluster.biosNode.kill(signal.SIGTERM)
    cluster.waitOnClusterSync(blockAdvancing=5)

    node3 = cluster.getNode(3)
    node2 = cluster.getNode(2)
    node4 = cluster.getNode(4) # bridge between 2 & 3

    def firstBlockForProducer(node, producer, blockNum):
        """Return the first contiguous block produced by producer ending at blockNum."""
        firstBlockNum = blockNum
        while firstBlockNum > 1:
            previousBlock = node.getBlock(firstBlockNum - 1, silentErrors=True, exitOnError=False)
            if previousBlock is None or previousBlock["producer"] != producer:
                break
            firstBlockNum -= 1
        return firstBlockNum

    def getProducerBlockIds(node, producer, firstBlockNum, maxBlocks):
        """Return contiguous producer block numbers and IDs starting at firstBlockNum."""
        blocks = []
        for blockNum in range(firstBlockNum, firstBlockNum + maxBlocks):
            block = node.getBlock(blockNum, silentErrors=True, exitOnError=False)
            if block is None:
                return None
            if block["producer"] != producer:
                break
            blocks.append({"block_num": blockNum, "id": block["id"]})
        return blocks

    Print("Wait for producer before j")
    node3.waitForAnyProducer("defproducerh", exitOnError=True)
    node3.waitForAnyProducer("defproduceri", exitOnError=True)
    iProdBlockNum = node3.getHeadBlockNum()
    firstIProdBlockNum = firstBlockForProducer(node3, "defproduceri", iProdBlockNum)

    node4.kill(signal.SIGTERM)
    assert not node4.verifyAlive(), "Node4 did not shutdown"

    Print(f"Wait until Node_03 is producing on its isolated fork ({bridgeReconnectProducer})")
    node3.waitForProducer(bridgeReconnectProducer, exitOnError=True)
    isolatedForkInfo = node3.getInfo(exitOnError=True)
    isolatedForkHeadId = isolatedForkInfo["head_block_id"]
    isolatedForkHeadNum = isolatedForkInfo["head_block_num"]

    Print("Relaunch bridge to reconnect Node_02 and Node_03 while Node_03 is still producing")
    node4.relaunch()

    def collectCanonicalIProducerBlocks():
        """Return canonical defproduceri blocks once node_02 has the whole verification window."""
        blocks = getProducerBlockIds(node2, "defproduceri", firstIProdBlockNum, producerSlotBlockCount)
        if blocks is None or len(blocks) < expectedIProducerBlocks:
            return None
        return blocks[:expectedIProducerBlocks]
    canonicalIProducerBlocks = Utils.waitForObj(collectCanonicalIProducerBlocks, timeout=forkSwitchLogTimeout)
    assert canonicalIProducerBlocks, \
        f"Expected at least {expectedIProducerBlocks} canonical defproduceri blocks from block {firstIProdBlockNum}"

    Print("Verify Node_03 converges from its isolated fork back to Node_02's canonical branch")
    def node3ConvergedToCanonicalBranch():
        """Return true when node_03 has adopted node_02's canonical block at the fork height."""
        info = node3.getInfo(silentErrors=True, exitOnError=False)
        if info is None or info["head_block_id"] == isolatedForkHeadId:
            return False

        node3Block = node3.getBlock(isolatedForkHeadNum, silentErrors=True, exitOnError=False)
        node2Block = node2.getBlock(isolatedForkHeadNum, silentErrors=True, exitOnError=False)
        return node3Block is not None \
            and node2Block is not None \
            and node3Block["id"] == node2Block["id"] \
            and node3Block["id"] != isolatedForkHeadId

    assert Utils.waitForBool(node3ConvergedToCanonicalBranch, timeout=canonicalConvergenceTimeout), \
        f"Expected node_03 to leave isolated fork head #{isolatedForkHeadNum} {isolatedForkHeadId} " \
        "and converge to node_02's canonical branch at that height"

    Print("Verify fork switch - poll for log entry in case sync is still settling")
    # The fork switch log may reference any of node_03's producers (j, k, or l) depending
    # on exactly when the bridge reconnects and blocks propagate.
    switchForkLogPattern = re.compile(r"switching forks from \S+ \(block number (\d+) (defproducer[jkl])\)")

    def partitionForkSwitchLines():
        """Return node_03 'switching forks' log lines that abandon a post-partition fork block."""
        lines = []
        for line in node3.linesInLog("switching forks"):
            match = switchForkLogPattern.search(line)
            if match and int(match.group(1)) > firstIProdBlockNum:
                lines.append(line)
        return lines

    def partitionForkSwitches():
        """Return node_03 producers abandoned after the partition, in log order."""
        return [switchForkLogPattern.search(line).group(2) for line in partitionForkSwitchLines()]

    switchLines = Utils.waitForObj(lambda: partitionForkSwitchLines() or None, timeout=forkSwitchLogTimeout)
    assert switchLines, "Expected to find 'switching forks' from a node_03 producer in node_03 log"

    # Verify the lockout-detection optimization: when the canonical branch reaches node_03 while it
    # is actively building a block on its isolated fork, the branch's strong QC locks node_03's head
    # out of fork-choice and the producer plugin must apply the incoming blocks immediately
    # ("applying blocks while producing") instead of deferring them to the end of the production
    # round. The fast path can only run while a block is under construction, and every production
    # round ends with a gap where none is: the produce-block-offset ramp completes a round's last
    # block up to ~450ms before its slot time, and the next round's first block may not start before
    # its slot begins ("Not starting block until"). A reconnect landing in such a gap - or past
    # node_03's whole window - applies the blocks through the regular non-producing path and the
    # diagnostic is legitimately absent. Prefer the direct fast-path diagnostic when present; fall
    # back to producer-state inference only when it is absent so release-style CI logs that omit
    # debug-only "Starting block" lines still validate the externally visible behavior above.
    logTimePattern = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+")

    def logLineTime(line):
        """Return the wall-clock time a nodeop log line was emitted at."""
        match = logTimePattern.search(line)
        assert match, f"expected a leading timestamp in node_03 log line: {line}"
        return datetime.strptime(match.group(0), "%Y-%m-%dT%H:%M:%S.%f")

    firstSwitchTime = min(logLineTime(line) for line in switchLines)

    fastPathPattern = re.compile(
        r"applying blocks while producing: head's branch is locked out .* fork-db head #(\d+)")

    def partitionFastPathLines():
        """Return node_03 lockout fast-path diagnostics for the post-partition fork."""
        lines = []
        for line in node3.linesInLog(lockoutFastPathLogPrefix):
            match = fastPathPattern.search(line)
            if match and int(match.group(1)) > firstIProdBlockNum:
                lines.append(line)
        return lines

    fastPathLines = [line for line in partitionFastPathLines() if logLineTime(line) <= firstSwitchTime]
    if fastPathLines:
        Print("Node_03 applied canonical blocks while producing; lockout fast path observed")
    else:
        startingBlockPattern = re.compile(r"Starting block #(\d+) \S+ producer (defproducer[a-z]+),")
        producedBlockPattern = re.compile(
            r"Produced block \S+ #(\d+) @ .* signed by (defproducer[a-z]+).* producing time: (\d+) us")
        node3ProducerStarts = []

        for line in node3.linesInLog("Starting block #"):
            match = startingBlockPattern.search(line)
            if match and match.group(2) in node3ProducerNames \
                    and logLineTime(line) < firstSwitchTime:
                node3ProducerStarts.append((logLineTime(line), int(match.group(1))))

        def lastStartedBlockWasProduced(lastStartTime, lastStartBlockNum):
            """Return true when node_03 finished producing its last debug-started block before the fork switch."""
            for line in node3.linesInLog("Produced block "):
                match = producedBlockPattern.search(line)
                if match and int(match.group(1)) == lastStartBlockNum \
                        and lastStartTime <= logLineTime(line) <= firstSwitchTime:
                    return True
            return False

        def producedBlockSpanningSwitch():
            """Return the node_03 producer block whose info-level production interval spans the fork switch."""
            for line in node3.linesInLog("Produced block "):
                match = producedBlockPattern.search(line)
                if not match or match.group(2) not in node3ProducerNames:
                    continue

                blockNum = int(match.group(1))
                if blockNum <= firstIProdBlockNum:
                    continue

                endTime = logLineTime(line)
                startTime = endTime - timedelta(microseconds=int(match.group(3)))
                if startTime <= firstSwitchTime <= endTime:
                    return blockNum
            return None

        if node3ProducerStarts:
            lastStartTime, lastStartBlockNum = max(node3ProducerStarts)
            assert lastStartedBlockWasProduced(lastStartTime, lastStartBlockNum), \
                "Expected node_03 to apply blocks mid-slot upon detecting strong-QC lockout of its " \
                f"isolated fork while building block #{lastStartBlockNum}"
            Print(f"Node_03 was between blocks at the fork switch (block #{lastStartBlockNum} already "
                  "produced); the lockout fast path was legitimately idle")
        else:
            spanningBlockNum = producedBlockSpanningSwitch()
            assert spanningBlockNum is None, \
                "Expected node_03 to apply blocks mid-slot upon detecting strong-QC lockout of its " \
                f"isolated fork while producing block #{spanningBlockNum}"
            Print("Node_03 log has no debug-only 'Starting block' entries; no info-level produced-block "
                  "interval spans the fork switch, so the lockout fast path was not required")

    Print("Wait until Node_00 to produce")
    node3.waitForProducer("defproducera")

    # verify the LIB blocks of defproduceri made it into the canonical chain
    # defproducerk has produced at least one block, but possibly more by time of relaunch, so verify only some of the round
    for canonicalBlock in canonicalIProducerBlocks:
        block = node3.getBlock(canonicalBlock["block_num"], exitOnError=True)
        assert block["id"] == canonicalBlock["id"], \
            f"expected canonical defproduceri block {canonicalBlock['id']} at block {canonicalBlock['block_num']}, instead: {block['id']}"
        assert block["producer"] == "defproduceri", \
            f"expected defproduceri for block {canonicalBlock['block_num']}, instead: {block['producer']}"

    # verify the post-partition canonical chain holds blocks from a plausible producer.
    # Normally node_03 resumes producing on the canonical branch and defproducerk and/or
    # defproducerl blocks land in the verification window. When the reconnect is slow enough
    # that node_03 burns its k and l slots on the abandoned fork, defproducera may own the
    # whole window instead - but only when node_03's log also shows it abandoned one of its
    # own defproducerl blocks. Decide from the window content rather than from the fork-switch
    # log alone: a routine slot-handoff micro-fork past the window (defproducera winning the
    # boundary race against node_03's last defproducerl block) also logs an abandoned
    # defproducerl block and must not change what the window is expected to hold.
    iProdBlockNum += producerSlotBlockCount  # into the next set of blocks
    lastWindowBlockNum = iProdBlockNum + producerSlotBlockCount - 1
    windowProducers = [node3.getBlockProducerByNum(blockNum)
                       for blockNum in range(iProdBlockNum, lastWindowBlockNum + 1)]
    if not {"defproducerk", "defproducerl"} & set(windowProducers):
        # re-check the fork switches: cascade switch lines can land after the first wait
        assert "defproducerl" in partitionForkSwitches(), \
            f"expected defproducerk or defproducerl in blocks {iProdBlockNum}-{lastWindowBlockNum} " \
            f"when node_03 abandoned no defproducerl block on its fork, instead: {windowProducers}"
        assert "defproducera" in windowProducers, \
            f"expected defproducera in blocks {iProdBlockNum}-{lastWindowBlockNum} after node_03 " \
            f"burned its production slots on the abandoned fork, instead: {windowProducers}"

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
