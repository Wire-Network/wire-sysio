#!/usr/bin/env python3

import signal
import json
import os
import filecmp

from TestHarness import Account, Cluster, Node, TestHelper, Utils, WalletMgr

###############################################################
# nodeop_chainbase_allocation_test
#
# Test snapshot creation and restarting from snapshot
#
###############################################################

# Parse command line arguments
args = TestHelper.parse_args({"-v","--activate-if","--dump-error-details","--leave-running","--keep-logs","--unshared"})
Utils.Debug = args.v
dumpErrorDetails=args.dump_error_details
activateIF=args.activate_if

walletMgr=WalletMgr(True)
cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
cluster.setWalletMgr(walletMgr)

testSuccessful = False
try:
    TestHelper.printSystemInfo("BEGIN")

    # The following is the list of chainbase objects that need to be verified:
    # - account_object (bootstrap)
    # - code_object (bootstrap)
    # - global_property_object
    # - key_value_object (bootstrap)
    # - protocol_state_object (bootstrap)
    # - permission_object (bootstrap)
    # The bootstrap process has created account_object and code_object (by uploading the bios contract),
    # key_value_object (token creation), protocol_state_object (preactivation feature), and permission_object
    # (automatically taken care by the automatically generated sysio account)
    assert cluster.launch(
        pnodes=1,
        prodCount=1,
        totalProducers=1,
        totalNodes=3,
        activateIF=activateIF,
        loadSystemContract=False,
        specificExtraNodeopArgs={
            1:"--read-mode irreversible --plugin sysio::producer_api_plugin"})

    producerNodeId = 0
    irrNodeId = 1
    nonProdNodeId = 2
    producerNode = cluster.getNode(producerNodeId)
    irrNode = cluster.getNode(irrNodeId)
    nonProdNode = cluster.getNode(nonProdNodeId)

    # Schedule a new producer to trigger new producer schedule for "global_property_object"
    newProducerAcc = Account("newprod")
    newProducerAcc.ownerPublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    newProducerAcc.activePublicKey = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
    nonProdNode.createAccount(newProducerAcc, cluster.sysioAccount, waitForTransBlock=True)

    setProdsStr = '{"schedule": ['
    setProdsStr += '{"producer_name":' + newProducerAcc.name + ',"authority": ["block_signing_authority_v0", {"threshold":1, "keys":[{"key":' + newProducerAcc.activePublicKey + ', "weight":1}]}]}'
    setProdsStr += ']}'
    cmd="push action -j sysio setprods '{}' -p sysio".format(setProdsStr)
    trans = producerNode.processClioCmd(cmd, cmd, silentErrors=False)
    assert trans
    setProdsBlockNum = int(trans["processed"]["block_num"])

    # Wait until the block where set prods is executed become irreversible so the producer schedule
    def isSetProdsBlockNumIrr():
            return producerNode.getIrreversibleBlockNum() >= setProdsBlockNum
    Utils.waitForBool(isSetProdsBlockNumIrr, timeout=30, sleepTime=0.1)
    # Once it is irreversible, immediately pause the producer so the promoted producer schedule is not cleared
    producerNode.processUrllibRequest("producer", "pause")

    producerNode.kill(signal.SIGTERM)

    # Wait for irrNode to catch up to the promoted producer schedule. The producer is already at or
    # past setProdsBlockNum, and the blocks it emitted before dying carry irrNode to the same point,
    # so the snapshot below covers the updated global_property_object.
    def isSetProdsBlockNumIrrOnIrrNode():
        """Report whether irrNode has advanced its LIB to the set-prods block."""
        return irrNode.getIrreversibleBlockNum() >= setProdsBlockNum
    assert Utils.waitForBool(isSetProdsBlockNumIrrOnIrrNode, timeout=30, sleepTime=0.1), \
        f"irrNode did not reach irreversible block {setProdsBlockNum}"

    # Take down every remaining source of blocks. irrNode runs in irreversible read mode, so its head
    # is its LIB, and any block delivered after the first snapshot advances that LIB -- leaving the two
    # snapshots describing different chain states and making them legitimately unequal. With no peer
    # left to serve blocks, the restart below replays the same persisted state and lands on the same head.
    nonProdNode.kill(signal.SIGTERM)
    cluster.biosNode.kill(signal.SIGTERM)

    # Create the snapshot and rename it, because the post-restart snapshot is written under the same
    # head-block-derived name and createSnapshot refuses to overwrite an existing file.
    res = irrNode.createSnapshot()
    beforeShutdownBlockNum = res["payload"]["head_block_num"]
    snapshotPathWithoutExt, snapshotExt = os.path.splitext(res["payload"]["snapshot_name"])
    beforeShutdownSnapshotPath = snapshotPathWithoutExt + "_before_shutdown" + snapshotExt
    os.rename(res["payload"]["snapshot_name"], beforeShutdownSnapshotPath)

    # Restart irr node and ensure the snapshot is still identical
    irrNode.kill(signal.SIGTERM)
    isRelaunchSuccess = irrNode.relaunch(timeout=15)
    assert isRelaunchSuccess, "Fail to relaunch"
    res = irrNode.createSnapshot()
    afterShutdownSnapshotPath = res["payload"]["snapshot_name"]
    afterShutdownBlockNum = res["payload"]["head_block_num"]
    assert afterShutdownBlockNum == beforeShutdownBlockNum, \
        f"snapshots cover different blocks: {beforeShutdownBlockNum} before shutdown, {afterShutdownBlockNum} after"
    assert filecmp.cmp(beforeShutdownSnapshotPath, afterShutdownSnapshotPath, shallow=False), "snapshot is not identical"

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
