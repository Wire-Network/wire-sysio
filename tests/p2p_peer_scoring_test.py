#!/usr/bin/env python3

import signal

from TestHarness import Cluster, TestHelper, Utils, WalletMgr

###############################################################
# p2p_peer_scoring_test
#
# Verify peer scoring in net_plugin:
# 1. peer_score is reported in /v1/net/connections API and within [0, 200]
# 2. Scores rise above baseline (100) on receiving nodes after block relay
# 3. Reconnected peers maintain non-negative scores
#
###############################################################

Print = Utils.Print
errorExit = Utils.errorExit

args = TestHelper.parse_args({"-d", "--keep-logs", "--dump-error-details",
                              "-v", "--leave-running", "--unshared"})
delay = args.d
debug = args.v
dumpErrorDetails = args.dump_error_details

Utils.Debug = debug
testSuccessful = False

cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True)

try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    pnodes = 1
    total_nodes = 3
    extraArgs = '--plugin sysio::net_api_plugin'

    Print("Launching cluster")
    if cluster.launch(pnodes=pnodes, totalNodes=total_nodes, delay=delay,
                      activateIF=True, extraNodeopArgs=extraArgs) is False:
        errorExit("Failed to stand up cluster.")

    cluster.waitOnClusterSync(blockAdvancing=5)

    # ---- Test 1: peer_score field is present and within valid range ----
    Print("Test 1: Verify peer_score is present and within [0, 200]")
    connections = cluster.nodes[0].processUrllibRequest('net', 'connections')

    open_conns = [c for c in connections['payload'] if c['is_socket_open']]
    assert len(open_conns) >= 2, f"Expected at least 2 open connections, got {len(open_conns)}"

    baseline = 100
    for conn in open_conns:
        assert 'peer_score' in conn, f"peer_score missing from connection {conn.get('connection_id')}"
        score = conn['peer_score']
        assert isinstance(score, int), f"peer_score is not an integer: {score}"
        assert 0 <= score <= 200, f"peer_score {score} outside valid range [0, 200] for connection {conn['connection_id']}"
        assert score >= baseline, f"peer_score {score} below baseline {baseline} at startup for connection {conn['connection_id']}"
        Print(f"  connection {conn['connection_id']}: peer_score={score}")

    # ---- Test 2: Scores increase above baseline after block relay ----
    # Scores are adjusted on the receiving side, so check non-producer nodes
    # (which receive blocks from the producer) rather than the producer itself.
    Print("Test 2: Verify scores increase on receiving nodes after block relay")
    node0 = cluster.getNode(0)
    start_block = node0.getHeadBlockNum()
    target_block = start_block + 20
    assert node0.waitForBlock(target_block, timeout=30), \
        f"Node 0 did not reach block {target_block}"

    # Check node 1's view — its connection to the producer (node 0) should have
    # an elevated score from receiving unique blocks.
    cluster.waitOnClusterSync(blockAdvancing=5)
    connections = cluster.nodes[1].processUrllibRequest('net', 'connections')
    open_conns = [c for c in connections['payload'] if c['is_socket_open']]

    above_baseline_count = 0
    for conn in open_conns:
        score = conn['peer_score']
        Print(f"  node1 connection {conn['connection_id']}: peer_score={score}")
        if score > baseline:
            above_baseline_count += 1

    assert above_baseline_count > 0, \
        f"Expected at least one peer above baseline on node 1 after receiving blocks"

    # ---- Test 3: Reconnected peers have non-negative scores ----
    # Kill nodes 1 and 2, verify disconnect is detected, then restart them.
    # After reconnection and resync, verify that scores remain non-negative.
    Print("Test 3: Verify peer scores for reconnected peers are non-negative")

    # Record scores before killing
    connections = cluster.nodes[0].processUrllibRequest('net', 'connections')
    pre_kill_scores = {}
    for conn in connections['payload']:
        if conn['is_socket_open']:
            pre_kill_scores[conn['connection_id']] = conn['peer_score']
            Print(f"  pre-kill connection {conn['connection_id']}: peer_score={conn['peer_score']}")

    # Kill non-producer nodes
    cluster.getNode(1).kill(signal.SIGTERM)
    cluster.getNode(2).kill(signal.SIGTERM)

    # Wait a bit for disconnect to be detected, then check scores
    node0.waitForBlock(node0.getHeadBlockNum() + 3, timeout=10)

    connections = cluster.nodes[0].processUrllibRequest('net', 'connections')
    closed_count = 0
    for conn in connections['payload']:
        if not conn['is_socket_open']:
            closed_count += 1
    Print(f"  closed connections after kill: {closed_count}")
    assert closed_count >= 2, f"Expected at least 2 closed connections, got {closed_count}"

    # Restart the nodes
    assert cluster.getNode(1).relaunch(), "Node 1 failed to relaunch"
    assert cluster.getNode(2).relaunch(), "Node 2 failed to relaunch"

    # Wait for resync
    cluster.waitOnClusterSync(blockAdvancing=5)

    # After reconnect, the outbound connections on node 0 are reused (score persists),
    # but the fresh inbound connections on node 1/2 start at baseline.
    connections = cluster.nodes[1].processUrllibRequest('net', 'connections')
    open_conns = [c for c in connections['payload'] if c['is_socket_open']]
    assert len(open_conns) >= 1, f"Node 1 expected open connections after relaunch"

    for conn in open_conns:
        score = conn['peer_score']
        Print(f"  node1 reconnected connection {conn['connection_id']}: peer_score={score}")
        # Fresh connections start at baseline; outbound connections that reconnected
        # may have decayed slightly but should still be reasonable
        assert score >= 0, f"Score {score} is negative, which should not happen"

    Print("All peer scoring assertions passed")
    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful,
                        dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
