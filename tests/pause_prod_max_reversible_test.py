#!/usr/bin/env python3

import random
import signal

from TestHarness import Cluster, TestHelper, Utils, WalletMgr

###############################################################
# pause_prod_max_reversible_test
#
# Verify production is paused when --max-reversible-blocks is exceeded
# and can be resumed when restarted.
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

args=TestHelper.parse_args({"--keep-logs","--dump-error-details","-v","--leave-running","--unshared"})
pnodes=3
debug=args.v
total_nodes = 3
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True, keepRunning=args.leave_running, keepLogs=args.keep_logs)

try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    Print("Stand up cluster")
    specificExtraNodeopArgs={
        1:"--max-reversible-blocks 30"}

    if cluster.launch(pnodes=pnodes, totalNodes=total_nodes, prodCount=1, totalProducers=pnodes, specificExtraNodeopArgs=specificExtraNodeopArgs, onlySetProds=True) is False:
        errorExit("Failed to stand up sys cluster.")

    Print("Wait for Cluster stabilization")
    # wait for cluster to start producing blocks
    if not cluster.waitOnClusterBlockNumSync(3):
        errorExit("Cluster never stabilized")

    Print("Kill bios node")
    cluster.biosNode.kill(signal.SIGTERM)

    Print("Kill a production node 0 and wait for production to pause")
    node00 = cluster.getNode(0)
    node00.kill(signal.SIGTERM)
    assert not node00.verifyAlive() # resets pid so reluanch works

    node01 = cluster.getNode(1)
    assert node01.waitForLibNotToAdvance(), "Production should have paused"
    assert node01.findInLog("Not producing block because max-reversible-blocks"), "Should have found not producing log"

    Print("Restart node 0, but node 1 will not resume production due to low max-reversible-blocks")
    node00.relaunch()
    assert node01.waitForLibNotToAdvance(), "Production should still be paused"

    Print("Restart node 1 and wait for production to resume")
    node01.kill(signal.SIGTERM)
    assert not node01.verifyAlive() # resets pid so reluanch works
    node01.relaunch(addSwapFlags={"--max-reversible-blocks": "3600"})
    assert node01.waitForLibToAdvance(timeout=60), "Production should have resumed"

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
