#!/usr/bin/env python3

import signal
import platform
import subprocess
import time
import re

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from TestHarness.testUtils import WaitSpec
from os import getpid
from pathlib import Path

###############################################################
# p2p connection in high latency network for one producer and one syning node cluster.
#
#   This test simulates p2p connections in high latency network. The test case is such that there are one producer
#   and one syncing node and a latency of 1100ms is introduced to their p2p connection.
#   The expected behavior is that producer recognize the net latency and do not send lib catchup to syncing node.
#   As syncing node is always behind, therefore sending lib catchup is useless as producer/peer node gets caught into infinite
#   loop of sending lib catch up to syncing node.
###############################################################

def readlogs(node_num, net_latency, nodeopLogPath):
    filename = nodeopLogPath
    f = subprocess.Popen(['tail','-F',filename], \
                         stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    latRegex = re.compile(r'\d+ms')
    t_end = time.time() + 80 # cluster runs for 80 seconds and and logs are being processed
    while time.time() <= t_end:
        line = f.stdout.readline().decode("utf-8")
        print(line)
        if 'info' in line and 'Catching up with chain, our last req is ' in line:
            Utils.Print("Syncing node is catching up with chain, however it should not due to net latency")
            return False
        if 'debug' in line and 'Network latency' in line and float(latRegex.search(line).group()[:-2]) < 0.8 * net_latency:
            Utils.Print("Network latency is lower than expected.")
            return False

    return True
def exec(cmd):
    process = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, err = process.communicate()
    process.wait()
    process.stdout.close()
    process.stderr.close()
    return err, process.returncode

Print=Utils.Print

args = TestHelper.parse_args({"--dump-error-details","--keep-logs","-v","--leave-running","--unshared"})
Utils.Debug=args.v

producers=1
syncingNodes=1
totalNodes=producers+syncingNodes
cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
dumpErrorDetails=args.dump_error_details

testSuccessful=False

specificExtraNodeopArgs={}
producerNodeId=0
syncingNodeId=1

specificExtraNodeopArgs[producerNodeId]=" --p2p-listen-endpoint 0.0.0.0:{}".format(9876+producerNodeId)
specificExtraNodeopArgs[syncingNodeId]="--p2p-peer-address 0.0.0.0:{}".format(9876+producerNodeId)

try:
    TestHelper.printSystemInfo("BEGIN")
    traceNodeopArgs=" --plugin sysio::producer_plugin --produce-block-offset-ms 0 --producer-threads 1 --plugin sysio::net_plugin --net-threads 1"
    if cluster.launch(pnodes=1, totalNodes=totalNodes, totalProducers=1, specificExtraNodeopArgs=specificExtraNodeopArgs, extraNodeopArgs=traceNodeopArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up sys cluster.")

    cluster.waitOnClusterSync(blockAdvancing=5)
    Utils.Print("Cluster in Sync")
    cluster.biosNode.kill(signal.SIGTERM)
    Utils.Print("Bios node killed")
    latency = 1100 # 1100 millisecond
    # adding latency to all inbound and outbound traffic
    Utils.Print( "adding {}ms latency to network.".format(latency) )
    if platform.system() == 'Darwin':
        cmd = 'sudo dnctl pipe 1 config delay {} && \
            echo "dummynet out proto tcp from any to any pipe 1" | sudo pfctl -f - && \
            sudo pfctl -e'.format(latency)
    else:
        cmd = 'tc qdisc add dev lo root netem delay {}ms'.format(latency)
    err, ReturnCode = exec(cmd)
    if ReturnCode != 0:
        print(err.decode("utf-8")) # print error details of network slowdown initialization commands
        Utils.errorExit("failed to initialize network latency, exited with error code {}".format(ReturnCode))
        # processing logs to make sure syncing node doesn't get into lib catch up mode.
    testSuccessful=readlogs(syncingNodeId, latency, cluster.nodeopLogPath)
    if platform.system() == 'Darwin':
        cmd = 'sudo pfctl -f /etc/pf.conf && \
            sudo dnctl -q flush && sudo pfctl -d'
    else:
        cmd = 'tc qdisc del dev lo root netem'
    err, ReturnCode = exec(cmd)
    if ReturnCode != 0:
        print(err.decode("utf-8")) # print error details of network slowdown termination commands
        Utils.errorExit("failed to remove network latency, exited with error code {}".format(ReturnCode))
finally:
    TestHelper.shutdown(cluster, None, testSuccessful, dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
