#!/usr/bin/env python3

import os
import tempfile
import shutil
import signal

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from TestHarness.Node import BlockType

###############################################################################
# ship_restart_test
#
# This test verifies SHiP shuts down gracefully or recovers when restarting
# with various scenarios of corrupted log and/or index files.
#
# It also verifies restarting from a snapshot with pre-existing SHiP data:
# a snapshot older than the SHiP head starts successfully, preserves the
# existing log data and appends new blocks contiguously, while a snapshot
# newer than the SHiP head is refused since writing the first post-snapshot
# block would leave a gap in the logs.
#
###############################################################################

Print=Utils.Print

args = TestHelper.parse_args({"--dump-error-details","--keep-logs","-v","--leave-running","--unshared"})

Utils.Debug=args.v
cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
dumpErrorDetails=args.dump_error_details
walletPort=TestHelper.DEFAULT_WALLET_PORT

totalProducerNodes=1
totalNonProducerNodes=1 # for SHiP node
totalNodes=totalProducerNodes+totalNonProducerNodes

walletMgr=WalletMgr(True, port=walletPort)
testSuccessful=False

prodNodeId = 0
shipNodeId = 1

# SHiP log rotation stride. The shutdown guard below keeps the stop point clear of stride
# boundaries so the head log/index files the test operates on are never empty.
shipStride = 200

tmpDir                = None
origStateHistoryLog   = ""
stateHistoryLog       = ""
origStateHistoryIndex = ""
stateHistoryIndex     = ""
origStateHistoryDir   = ""

# Verifies that SHiP should fail to restart with a corrupted first entry header
def corruptedHeaderTest(pos, corruptedValue, shipNode):
    # restore log and index
    shutil.copyfile(origStateHistoryLog, stateHistoryLog)
    shutil.copyfile(origStateHistoryIndex, stateHistoryIndex)

    with open(stateHistoryLog, 'rb+') as f: # opened as binary file
        f.seek(pos) # seek to the position to corrupt
        f.write(corruptedValue) # corrupt it

    isRelaunchSuccess = shipNode.relaunch()
    assert not isRelaunchSuccess, "SHiP node should have failed to relaunch"

def isFilePrefix(prefixPath, fullPath):
    """Return True if the content of the file at prefixPath is a byte-for-byte prefix of the file at fullPath."""
    prefixSize = os.path.getsize(prefixPath)
    if os.path.getsize(fullPath) < prefixSize:
        return False
    chunkSize = 1024*1024
    with open(prefixPath, 'rb') as prefixFile, open(fullPath, 'rb') as fullFile:
        remaining = prefixSize
        while remaining > 0:
            readSize = min(chunkSize, remaining)
            if prefixFile.read(readSize) != fullFile.read(readSize):
                return False
            remaining -= readSize
    return True

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)
    Print("Stand up cluster")

    specificExtraNodeopArgs={}
    specificExtraNodeopArgs[prodNodeId]="--plugin sysio::producer_api_plugin"
    specificExtraNodeopArgs[shipNodeId]=(
        "--plugin sysio::state_history_plugin "
        "--trace-history --chain-state-history --finality-data-history "
        f"--state-history-stride {shipStride} "
        f"--state-history-endpoint 127.0.0.1:{Utils.getPort(Utils.PortStateHistory)} "
        "--plugin sysio::net_api_plugin --plugin sysio::producer_api_plugin"
    )

    # biosFinalizer=False keeps the bios node out of the finalizer policy so that finality
    # (and with it snapshot completion in part 3) keeps working after bios is shut down
    if cluster.launch(topo="mesh", pnodes=totalProducerNodes, totalNodes=totalNodes,
                      activateIF=True, biosFinalizer=False,
                      specificExtraNodeopArgs=specificExtraNodeopArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")

    # Verify nodes are in sync and advancing
    cluster.waitOnClusterSync(blockAdvancing=5)
    Print("Cluster in Sync")

    Print("Shutdown unneeded bios node")
    cluster.biosNode.kill(signal.SIGTERM)

    prodNode = cluster.getNode(prodNodeId)
    shipNode = cluster.getNode(shipNodeId)

    Print("Shutdown producer and SHiP nodes")

    # Stop clear of a state-history-stride boundary: on an exact multiple the head log/index have
    # just rotated into retained/ and are 0 bytes, so the file surgery below would operate on empty
    # files.
    assert prodNode.waitForBlockClearOfStride(shipStride, timeout=30), \
        "producer did not advance clear of a state-history-stride boundary"

    prodNode.processUrllibRequest("producer", "pause", exitOnError=True)
    blockNum = prodNode.getHeadBlockNum()
    shipNode.waitForBlock(blockNum)
    prodNode.kill(signal.SIGTERM)
    shipNode.kill(signal.SIGTERM)

    shipDir               = os.path.join(Utils.getNodeDataDir(shipNodeId), "state-history")
    stateHistoryLog       = os.path.join(shipDir, "chain_state_history.log")
    stateHistoryIndex     = os.path.join(shipDir, "chain_state_history.index")
    tmpDir                = tempfile.mkdtemp()
    origStateHistoryLog   = os.path.join(tmpDir, "chain_state_history.log")
    origStateHistoryIndex = os.path.join(tmpDir, "chain_state_history.index")

    # save original chain_state_history log and index files
    Print("Save original SHiP log and index")
    shutil.copyfile(stateHistoryLog, origStateHistoryLog)
    shutil.copyfile(stateHistoryIndex, origStateHistoryIndex)

    # save the whole state-history dir (all three logs and indexes) for the
    # snapshot-newer-than-SHiP test in part 3
    origStateHistoryDir = os.path.join(tmpDir, "state-history-orig")
    shutil.copytree(shipDir, origStateHistoryDir)

    ############## Part 1: tests while producer node is down  #################
    
    #-------- Index file is removed. It should be regenerated at restart.
    Print("index file removed test")

    os.remove(stateHistoryIndex)

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"
    assert Utils.compareFiles(stateHistoryLog, origStateHistoryLog, mode="rb") # log unchanged
    assert Utils.compareFiles(stateHistoryIndex, origStateHistoryIndex, mode="rb") # index regenerated

    shipNode.kill(signal.SIGTERM) # shut down ship node for next test

    '''
    Test failure 1: index file was not regenerated. Reenable this after https://github.com/AntelopeIO/spring/issues/990 is fixed.

    #-------- Index file last entry is corrupted. It should be regenerated at restart.
    with open(stateHistoryIndex, 'rb+') as stateHistoryIndexFile: # opened as binary file
        # seek to last entry (8 bytes before the end of file)
        stateHistoryIndexFile.seek(-8, 2) # -8 for backward, 2 for starting at end

        # set the index to a random value
        stateHistoryIndexFile.write(b'\x00\x01\x02\x03\x04\x05\x06\x07')

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"
    assert Utils.compareFiles(stateHistoryLog, origStateHistoryLog, mode="rb")
    assert Utils.compareFiles(stateHistoryIndex, origStateHistoryIndex, mode="rb")
    '''

    #-------- Truncate index file. It should be regenerated
    #         because index size is not the same as expected size
    Print("Truncated index file test")

    # restore log and index
    shutil.copyfile(origStateHistoryLog, stateHistoryLog)
    shutil.copyfile(origStateHistoryIndex, stateHistoryIndex)

    with open(stateHistoryIndex, 'rb+') as f:
        indexFileSize = os.path.getsize(stateHistoryIndex)
        newSize       = indexFileSize - 8 # truncate 8 bytes
        f.truncate(newSize)

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"
    assert Utils.compareFiles(stateHistoryLog, origStateHistoryLog, mode="rb") # log file unchanged
    assert Utils.compareFiles(stateHistoryIndex, origStateHistoryIndex, mode="rb") # index file regenerated

    shipNode.kill(signal.SIGTERM) # shut down it for next test

    #-------- Add an extra entry to index file. It should be regenerated
    #         because index size is not the same as expected size
    Print("Extra entry in index file test")

    # restore log and index
    shutil.copyfile(origStateHistoryLog, stateHistoryLog)
    shutil.copyfile(origStateHistoryIndex, stateHistoryIndex)

    with open(stateHistoryIndex, 'rb+') as stateHistoryIndexFile: # opened as binary file
        stateHistoryIndexFile.seek(0, 2) # seek to end of file
        stateHistoryIndexFile.write(b'\x00\x00\x00\x00\x00\x00\x01\x0F') # write a small value

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"
    assert Utils.compareFiles(stateHistoryLog, origStateHistoryLog, mode="rb") # log file not changed
    assert Utils.compareFiles(stateHistoryIndex, origStateHistoryIndex, mode="rb") # index file regenerated

    shipNode.kill(signal.SIGTERM) # shut down it for next test

    #-------- Remove log file. The log file should be reconstructed from state
    #         and restart succeeds
    Print("Removed log file test")

    shutil.copyfile(origStateHistoryIndex, stateHistoryIndex)

    os.remove(stateHistoryLog)

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"

    shipNode.kill(signal.SIGTERM) # shut down it for next test

    #-------- Corrupt first entry's magic. Relaunch should fail
    Print("first entry magic corruption test")
    corruptedHeaderTest(0, b'\x00\x01\x02\x03\x04\x05\x06\x07', shipNode) # 0 is magic's position

    #-------- Corrupt first entry's block_id. Relaunch should fail
    Print("first entry block_id corruption test")
    corruptedHeaderTest(8, b'\x00\x01\x02\x03\x04\x05\x06\x07', shipNode) # 8 is block_id's position

    '''
    # Test failure 2: Reenable this after https://github.com/AntelopeIO/spring/issues/989 is fixed.
    #-------- Corrupt last entry's position . It should be repaired.
    # After producer node restarts, head on SHiP node should advance.
    Print("last entry postion corruption test")

    shutil.copyfile(origStateHistoryLog, stateHistoryLog)
    shutil.copyfile(origStateHistoryIndex, stateHistoryIndex)

    with open(stateHistoryLog, 'rb+') as stateHistoryLogFile: # opened as binary file
        # seek to last index (8 bytes before the end of file)
        stateHistoryLogFile.seek(-8, 2) # -8 for backward, 2 for starting at end

        # set the index to a random value
        stateHistoryLogFile.write(b'\x00\x01\x02\x03\x04\x05\x06\x07')

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"
    isRelaunchSuccess = prodNode.relaunch(chainArg="--enable-stale-production")
    assert isRelaunchSuccess, "Failed to relaunch prodNode"

    assert shipNode.waitForHeadToAdvance(), "Head did not advance on shipNode"
    prodNode.kill(signal.SIGTERM)
    shipNode.kill(signal.SIGTERM)
    '''

    '''
    # Test failure 3: Reenable this after https://github.com/AntelopeIO/spring/issues/989 is fixed.
    #-------- Corrupt last entry's header. It should be repaired.
    # After producer node restarts, head on SHiP node should advance.
    Print("last entry header corruption test")

    shutil.copyfile(origStateHistoryLog, stateHistoryLog)
    shutil.copyfile(origStateHistoryIndex, stateHistoryIndex)

    with open(stateHistoryLog, 'rb+') as f: # opened as binary file
        # seek to last index (8 bytes before the end of file)
        f.seek(-8, 2) # -8 for backward, 2 for starting at end

        data = f.read(8)
        integer_value = int.from_bytes(data, byteorder='little')
        f.seek(integer_value)

        # corrupt the header
        f.write(b'\x00\x01\x02\x03\x04\x05\x06\x07')

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"
    isRelaunchSuccess = prodNode.relaunch(chainArg="--enable-stale-production")
    assert isRelaunchSuccess, "Failed to relaunch prodNode"

    assert shipNode.waitForHeadToAdvance(), "Head did not advance on shipNode"
    prodNode.kill(signal.SIGTERM)
    shipNode.kill(signal.SIGTERM)
    '''

    ############## Part 2: tests while producer node is up  #################

    isRelaunchSuccess = prodNode.relaunch(chainArg="--enable-stale-production")
    assert isRelaunchSuccess, "Failed to relaunch prodNode"

    shutil.copyfile(origStateHistoryLog, stateHistoryLog)
    shutil.copyfile(origStateHistoryIndex, stateHistoryIndex)

    #-------- Index file is removed. It should be regenerated at restart
    Print("Index file removed while producer node is up test")

    os.remove(stateHistoryIndex)

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"
    assert shipNode.waitForHeadToAdvance(), "Head did not advance on shipNode"

    shipNode.kill(signal.SIGTERM) # shut down it for next test

    '''
    # Test failure 4: Reenable this after issue https://github.com/AntelopeIO/spring/issues/989 fixed.
    #-------- Corrupt last entry of log file. It should be repaired
    # and head should advance
    with open(stateHistoryLog, 'rb+') as stateHistoryLogFile: # opened as binary file
        # seek to last index, 8 bytes before the end of file
        stateHistoryLogFile.seek(-8, 2) # -8 for backward, 2 for starting at end

        # set the index to a random value
        stateHistoryLogFile.write(b'\x00\x01\x02\x03\x04\x05\x06\x07')

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"
    assert shipNode.waitForHeadToAdvance(), "Head did not advance on shipNode"
    '''

    ############## Part 3: restart from snapshot with pre-existing SHiP data  #################

    # The producer node is running; the SHiP node is down with intact state-history files.

    #-------- Restart from a snapshot older than the SHiP head. Replayed blocks the logs
    #         already contain are no-op rewrites of identical block ids, so the node starts,
    #         the pre-existing SHiP data is preserved, and new blocks append contiguously.
    Print("Restart from snapshot older than SHiP head test")

    isRelaunchSuccess = shipNode.relaunch()
    assert isRelaunchSuccess, "Failed to relaunch shipNode"

    # ensure the snapshot lands well past the SHiP data saved at the start of the test so
    # the snapshot-newer-than-SHiP test below has a genuine gap to detect
    assert shipNode.waitForBlock(blockNum + 10), "shipNode did not advance past the part 1 head"

    ret = shipNode.createSnapshot()
    assert ret is not None and "payload" in ret, f"Snapshot creation failed: {ret}"
    snapshotHead = ret["payload"]["head_block_num"]
    Print(f"Snapshot created at head block {snapshotHead}")

    # the pending snapshot is written to disk once its block becomes irreversible; then
    # advance SHiP data past the snapshot head so the snapshot is older than the logs
    assert shipNode.waitForBlock(snapshotHead + 1, blockType=BlockType.lib), \
        "snapshot block did not become irreversible"
    assert shipNode.waitForBlock(snapshotHead + 10), "shipNode did not advance past the snapshot head"

    # stop clear of a stride boundary with enough headroom that the append-only prefix
    # comparison below is not invalidated by a head-log rotation during the short run
    # after the snapshot restart
    assert shipNode.waitForBlockClearOfStride(shipStride, upperMargin=60, timeout=60), \
        "shipNode did not advance clear of a state-history-stride boundary"

    shipNode.kill(signal.SIGTERM)

    snapshotPath = shipNode.getLatestSnapshot()

    # save the SHiP log and index as they exist before the snapshot restart; the restart
    # must preserve this data byte-for-byte, only appending new blocks after it
    preSnapshotLog   = os.path.join(tmpDir, "pre_snapshot_chain_state_history.log")
    preSnapshotIndex = os.path.join(tmpDir, "pre_snapshot_chain_state_history.index")
    shutil.copyfile(stateHistoryLog, preSnapshotLog)
    shutil.copyfile(stateHistoryIndex, preSnapshotIndex)

    # --snapshot requires an empty chainbase; keep blocks/ and state-history/
    shipNode.removeState()

    isRelaunchSuccess = shipNode.relaunch(chainArg=f"--snapshot {snapshotPath}")
    assert isRelaunchSuccess, "Failed to relaunch shipNode from a snapshot older than the SHiP head"

    # a block past the producer's current head cannot already be in the restored SHiP
    # data, so reaching it proves SHiP appended new blocks after the overlap
    assert shipNode.waitForBlock(prodNode.getHeadBlockNum() + 1), \
        "Head did not advance past the pre-restart SHiP head after the snapshot restart"

    shipNode.kill(signal.SIGTERM)

    # pre-existing SHiP data must be retained unmodified as a prefix of the grown files
    assert isFilePrefix(preSnapshotLog, stateHistoryLog), \
        "SHiP log data from before the snapshot restart was modified"
    assert isFilePrefix(preSnapshotIndex, stateHistoryIndex), \
        "SHiP index data from before the snapshot restart was modified"

    #-------- Restart from a snapshot newer than the SHiP head. Writing the first block
    #         after the snapshot would leave a gap in the logs, so SHiP must refuse to
    #         start rather than serve history with a hole in it.
    Print("Restart from snapshot newer than SHiP head test")

    # restore the state-history dir saved at the start of the test; its logs end far
    # below the snapshot head
    shutil.rmtree(shipDir)
    shutil.copytree(origStateHistoryDir, shipDir)

    shipNode.removeState()

    isRelaunchSuccess = shipNode.relaunch(rmArgs=f"--snapshot {snapshotPath}",
                                          chainArg=f"--snapshot {snapshotPath}")
    assert not isRelaunchSuccess, "shipNode should have failed to relaunch from a snapshot newer than the SHiP head"
    assert shipNode.findInLog("skips over block") is not None, \
        "expected SHiP 'skips over block' gap error in shipNode log"

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)
    if tmpDir is not None:
        shutil.rmtree(tmpDir, ignore_errors=True)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
