#!/usr/bin/env python3

"""Verify sync throttling from sender metrics while both peers complete the same backlog."""

import re
import signal
import sys
import time

from TestHarness import Cluster, TestHelper, Utils, WalletMgr, CORE_SYMBOL, createAccountKeys, ReturnType
from TestHarness.TestHelper import AppArgs

###############################################################
# p2p_sync_throttle_test
#
# Test throttling of a peer during block syncing.
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

SYNC_RATE_CAP = '200KB/s'
SYNC_RATE_BYTES_PER_SECOND = 200_000
UNTHROTTLED_RATE_CAP = '1TB/s'
METRICS_INTERVAL_SECONDS = 1
TRANSACTION_GENERATION_SECONDS = 30


def peer_sync_metrics(node, peer_ports):
    """Read the sender's sync counters and throttle state for peers identified by their advertised ports."""
    connections = node.processUrllibRequest('net', 'connections', exitOnError=True)['payload']
    metrics = node.processUrllibRequest('prometheus', 'metrics', returnType=ReturnType.raw,
                                      method='GET', exitOnError=True).decode()
    result = {port: {'throttling': False, 'bytes_sent': 0} for port in peer_ports}
    for connection in connections:
        address = connection['last_handshake']['p2p_address'].split(' - ', 1)[0]
        port = address.rsplit(':', 1)[-1]
        if port not in result:
            continue
        connection_id = connection['connection_id']
        for metric, field in [('nodeop_p2p_block_sync_throttling', 'throttling'),
                              ('nodeop_p2p_block_sync_bytes_sent', 'bytes_sent')]:
            pattern = rf'^{metric}\{{[^\n]*connection_id="{connection_id}"[^\n]*\}}\s+(\S+)$'
            match = re.search(pattern, metrics, re.MULTILINE)
            if match:
                result[port][field] = max(result[port][field], float(match[1]))
    return result


appArgs = AppArgs()
appArgs.add(flag='--plugin',action='append',type=str,help='Run nodes with additional plugins')
appArgs.add(flag='--connection-cleanup-period',type=int,help='Interval in whole seconds to run the connection reaper and metric collection')

args=TestHelper.parse_args({"-d","--keep-logs","--activate-if"
                            ,"--dump-error-details","-v","--leave-running"
                            ,"--unshared"},
                            applicationSpecificArgs=appArgs)
pnodes=1
delay=args.d
debug=args.v
prod_count = 2
total_nodes=5
activateIF=args.activate_if
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True)

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)

    Print(f'producing nodes: {pnodes}, delay between nodes launch: {delay} second{"s" if delay != 1 else ""}')

    Print("Stand up cluster")
    # Custom topology:
    #    prodNode <-> nonProdNode
    #                             <-> throttlingNode <-> throttledNode  (200KB/s)
    #                                                <-> unThrottledNode (1TB/s)
    #
    # Observe the capped connection's throttle state while both peers sync the same backlog.
    if cluster.launch(pnodes=pnodes, unstartedNodes=3, totalNodes=total_nodes, prodCount=prod_count,
                      extraNodeopArgs="--sync-fetch-span 5",
                      specificExtraNodeopArgs={
                          2: f'--plugin sysio::net_api_plugin --plugin sysio::prometheus_plugin '
                             f'--connection-cleanup-period {METRICS_INTERVAL_SECONDS}',
                          3: '--plugin sysio::net_api_plugin',
                          4: '--plugin sysio::net_api_plugin'},
                      topo='./tests/p2p_sync_throttle_test_shape.json', delay=delay, activateIF=activateIF) is False:
        errorExit("Failed to stand up sys cluster.")

    prodNode = cluster.getNode(0)
    nonProdNode = cluster.getNode(1)

    accounts=createAccountKeys(2)
    if accounts is None:
        Utils.errorExit("FAILURE - create keys")

    accounts[0].name="tester111111"
    accounts[1].name="tester222222"

    account1PrivKey = accounts[0].activePrivateKey
    account2PrivKey = accounts[1].activePrivateKey

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

    beginLargeBlocksHeadBlock = nonProdNode.getHeadBlockNum()

    Print("Configure and launch txn generators")
    targetTpsPerGenerator = 500
    testTrxGenDurationSec=TRANSACTION_GENERATION_SECONDS
    trxGeneratorCnt=1
    cluster.launchTrxGenerators(contractOwnerAcctName=cluster.sysioAccount.name, acctNamesList=[accounts[0].name,accounts[1].name],
                                acctPrivKeysList=[account1PrivKey,account2PrivKey], nodeId=prodNode.nodeId, tpsPerGenerator=targetTpsPerGenerator,
                                numGenerators=trxGeneratorCnt, durationSec=testTrxGenDurationSec, waitToComplete=True)

    endLargeBlocksHeadBlock = nonProdNode.getHeadBlockNum()

    throttlingNode = cluster.unstartedNodes[0]
    i = throttlingNode.cmd.index('--p2p-listen-endpoint')
    throttleListenAddr = throttlingNode.cmd[i+1]
    # Keep large bootstrap blocks within the peer's five-second sync-response timeout. This is the approximate
    # effective rate of the old 20 KB/s setting, whose rate calculation understated bytes per second by ten.
    throttlingNode.cmd[i+1] = f'{throttleListenAddr}:{SYNC_RATE_CAP}'
    throttleListenIP, throttleListenPort = throttleListenAddr.split(':')
    throttlingNode.cmd.append('--p2p-listen-endpoint')
    unThrottleListenAddr = f'{throttleListenIP}:{Utils.getPort(Utils.PortAlternateP2P, 0)}'
    throttlingNode.cmd.append(f'{unThrottleListenAddr}:{UNTHROTTLED_RATE_CAP}')

    cluster.biosNode.kill(signal.SIGTERM)

    Print("Launch throttling node")
    cluster.launchUnstarted(1)
    assert throttlingNode.verifyAlive(), "throttling node did not launch"

    # Throttling node was offline during block generation and once online receives blocks as fast as possible
    assert throttlingNode.waitForBlock(endLargeBlocksHeadBlock, timeout=endLargeBlocksHeadBlock/2), f'wait for block {endLargeBlocksHeadBlock}  on throttled node timed out'

    Print("Launch throttled and un-throttled nodes")
    cluster.launchUnstarted(2)

    throttledNode = cluster.getNode(3)
    unThrottledNode = cluster.getNode(4)
    assert throttledNode.verifyAlive(), "throttled node did not launch"
    assert unThrottledNode.verifyAlive(), "un-throttled node did not launch"

    peers = {'throttled': throttledNode, 'unthrottled': unThrottledNode}
    peer_ports = {name: node.cmd[node.cmd.index('--p2p-listen-endpoint') + 1].rsplit(':', 1)[-1]
                  for name, node in peers.items()}
    observations = {name: {'throttling': False, 'bytes_sent': 0, 'completed_at': None} for name in peers}

    # Connect after both HTTP endpoints are ready, so startup time is excluded from the diagnostic timings.
    clusterStart = time.monotonic()
    throttledNode.processUrllibRequest('net', 'connect', payload=throttleListenAddr, exitOnError=True)
    unThrottledNode.processUrllibRequest('net', 'connect', payload=unThrottleListenAddr, exitOnError=True)

    def both_peers_synced():
        """Poll both peers together, retaining throttle observations and each peer's first completion time."""
        metrics = peer_sync_metrics(throttlingNode, peer_ports.values())
        for name, node in peers.items():
            observation = observations[name]
            sample = metrics[peer_ports[name]]
            observation['throttling'] |= bool(sample['throttling'])
            observation['bytes_sent'] = max(observation['bytes_sent'], sample['bytes_sent'])
            if observation['completed_at'] is None and node.getHeadBlockNum() >= endLargeBlocksHeadBlock:
                observation['completed_at'] = time.monotonic() - clusterStart
        return all(value['completed_at'] is not None and value['bytes_sent'] >= SYNC_RATE_BYTES_PER_SECOND
                   for value in observations.values())

    assert Utils.waitForBool(both_peers_synced, timeout=endLargeBlocksHeadBlock*2), \
        f'Peers did not sync block {endLargeBlocksHeadBlock} and report sync bytes: {observations}'
    Print(f'Sync observations: {observations}')
    assert observations['throttled']['throttling'], 'The capped sync connection never reported throttling'
    assert not observations['unthrottled']['throttling'], 'The 1 TB/s sync connection unexpectedly throttled'

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
