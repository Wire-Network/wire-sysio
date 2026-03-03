#!/usr/bin/env python3

import signal

from TestHarness import Cluster, TestHelper, Utils, WalletMgr

###############################################################
# p2p_peer_auth_test
#
# Test P2P peer authentication using peer_auth_message.
#
# Topology:
#   node0 (producer) <-> node1 (authorized peer) — should sync
#   node0 (producer)  X  node2 (unauthorized peer) — should be rejected
#
# Flow:
#   1. Launch node0 as producer with default --allowed-connection any
#   2. Kill bios and node0, restart node0 with --allowed-connection specified
#   3. Start node1 (authorized) — verify it syncs from node0
#   4. Start node2 (unauthorized) — verify it cannot sync and is rejected
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit

args=TestHelper.parse_args({"--keep-logs"
                            ,"--dump-error-details","-v","--leave-running"
                            ,"--unshared"})
debug=args.v
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

# K1 keypairs for peer authentication (generated via: clio create key --k1 --to-console)
KEY0_PUB = "PUB_K1_7vWwkYDbJottP4hiT3mtbjYZ69nG5ZHaA9WKdJirKapX46pTir"
KEY0_PVT = "PVT_K1_2MsgQPmGnn1h6ymMotKZ4FAQZSawx8KbiLLFqNUkKJ9r8Yq8z8"

KEY1_PUB = "PUB_K1_83HCYssKBuwnT8KXTqKmz4nA9HoRHaKd6kJKYuWCXjg7T8eE9h"
KEY1_PVT = "PVT_K1_2bjvK9CXFtJn548osj9jKS8gVyZNHgUh5uHSttLMcpCkXzpqRt"

KEY2_PUB = "PUB_K1_5hxPfNud5RfR1sLVxSHnK4qPWkLh2wRZUJKsCa8rzCqkPHvgZR"
KEY2_PVT = "PVT_K1_Cj7pBcRkPie6xAd1Y6gFumu289XfzhVLas4ckXBX5GLgqZc7w"

def peer_key_json(pub):
    """Format a public key as a JSON string for --peer-key (needs double quotes for dejsonify)."""
    return f'"{pub}"'

def peer_private_key_json(pub, pvt):
    """Format a --peer-private-key JSON tuple."""
    return f'["{pub}","{pvt}"]'

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True)

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)

    pnodes=1
    totalNodes=3
    unstartedNodes=2

    # Configure unstarted nodes with auth args baked into their start commands.
    # JSON values must survive shlex.split in the launcher — protect with single quotes.
    # Node1 (authorized): trusts node0's key, provides its own identity
    specificExtraNodeopArgs = {}
    specificExtraNodeopArgs[0] = "--plugin sysio::net_api_plugin"
    specificExtraNodeopArgs[1] = (
        f"--allowed-connection specified"
        f" --peer-key '{peer_key_json(KEY0_PUB)}'"
        f" --peer-private-key '{peer_private_key_json(KEY1_PUB, KEY1_PVT)}'"
        f" --p2p-peer-address localhost:9876"
    )
    # Node2 (unauthorized): has its own identity but node0 does NOT trust KEY2
    specificExtraNodeopArgs[2] = (
        f"--allowed-connection specified"
        f" --peer-key '{peer_key_json(KEY2_PUB)}'"
        f" --peer-private-key '{peer_private_key_json(KEY2_PUB, KEY2_PVT)}'"
        f" --p2p-peer-address localhost:9876"
    )

    Print("Stand up cluster: 1 producer, 2 unstarted nodes")
    if cluster.launch(pnodes=pnodes, totalNodes=totalNodes, unstartedNodes=unstartedNodes,
                      topo='mesh', activateIF=True, specificExtraNodeopArgs=specificExtraNodeopArgs) is False:
        errorExit("Failed to stand up cluster.")

    node0 = cluster.getNode(0)
    node1 = cluster.unstartedNodes[0]
    node2 = cluster.unstartedNodes[1]

    Print("Wait for node0 to produce blocks")
    assert node0.waitForHeadToAdvance(blocksToAdvance=4), "node0 did not produce blocks"
    headBlock = node0.getHeadBlockNum()
    Print(f"node0 head block: {headBlock}")

    Print("Killing bios node")
    cluster.biosNode.kill(signal.SIGTERM)

    Print("Killing node0 to restart with peer authentication")
    node0.kill(signal.SIGTERM)
    assert not node0.verifyAlive(), "node0 did not shut down"

    Print("Restarting node0 with --allowed-connection specified (trusts only node1)")
    # chainArg is processed by shlex.split — single quotes protect JSON double quotes
    chainArg = (
        f"--allowed-connection specified"
        f" --peer-key '{peer_key_json(KEY1_PUB)}'"
        f" --peer-private-key '{peer_private_key_json(KEY0_PUB, KEY0_PVT)}'"
    )
    assert node0.relaunch(chainArg=chainArg), "node0 failed to relaunch with auth"

    Print("Launching node1 (authorized peer)")
    cluster.launchUnstarted(1)
    assert node1.verifyAlive(), "node1 did not launch"

    Print("Verify node1 syncs from node0 (positive case)")
    headBlock = node0.getHeadBlockNum()
    assert node1.waitForBlock(headBlock, timeout=30), \
        f"node1 did not sync to block {headBlock} — authorized peer should sync"
    Print(f"PASS: node1 synced to block {headBlock}")

    Print("Launching node2 (unauthorized peer)")
    cluster.launchUnstarted(1)
    assert node2.verifyAlive(), "node2 did not launch"

    Print("Verify node2 cannot sync from node0 (negative case)")
    assert not node2.waitForBlock(headBlock, timeout=10), \
        f"node2 synced to block {headBlock} — unauthorized peer should NOT sync"
    node2Head = node2.getHeadBlockNum()
    Print(f"PASS: node2 did not sync (head {node2Head} < node0 head {headBlock})")

    Print("Verify node0 logged rejection of unauthorized peer")
    assert node0.findInLog("Unauthorized peer key"), \
        "node0 did not log 'Unauthorized peer key' — expected rejection of node2"
    Print("PASS: node0 logged 'Unauthorized peer key'")

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
