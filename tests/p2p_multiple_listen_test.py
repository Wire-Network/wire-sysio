#!/usr/bin/env python3

import signal

from TestHarness import Cluster, TestHelper, Utils, WalletMgr

###############################################################
# p2p_multiple_listen_test
#
# Test nodeop ability to listen on multiple ports for p2p
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit
advertisedP2pEndpoint0 = 'ext-ip0:20000'
advertisedP2pEndpoint1 = 'ext-ip1:20001'

args=TestHelper.parse_args({"-p","-n","-d","--keep-logs"
                            ,"--activate-if","--dump-error-details","-v"
                            ,"--leave-running","--unshared"})
pnodes=args.p
delay=args.d
debug=args.v
total_nodes=5
activateIF=args.activate_if
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True)

def getOpenPeerAddress(node):
    """Return the single open peer address advertised to the supplied node by node 00."""
    connections = node.processUrllibRequest('net', 'connections')
    openPeerAddresses = []
    for conn in connections['payload']:
        if conn['is_socket_open']:
            connectedAgent = conn['last_handshake']['agent']
            assert connectedAgent == 'node-00', f"Connected node identified as '{connectedAgent}' instead of node-00"
            openPeerAddresses.append(conn['last_handshake']['p2p_address'].split()[0])

    assert len(openPeerAddresses) == 1, f'Node {node.nodeId} is expected to have exactly one open socket'
    return openPeerAddresses[0]

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)

    Print(f'producing nodes: {pnodes}, delay between nodes launch: {delay} second{"s" if delay != 1 else ""}')

    Print("Stand up cluster")
    alternateListenEndpoint = f"0.0.0.0:{Utils.getPort(Utils.PortAlternateP2P, 3)}"
    alternatePeerEndpoint = f"localhost:{Utils.getPort(Utils.PortAlternateP2P, 3)}"
    specificArgs = {
        '0': f'--agent-name node-00 --p2p-listen-endpoint 0.0.0.0:{cluster.getNodeP2pPort(0)} '
             f'--p2p-listen-endpoint {alternateListenEndpoint} --p2p-server-address {advertisedP2pEndpoint0} '
             f'--p2p-server-address {advertisedP2pEndpoint1} --plugin sysio::net_api_plugin',
        '2': f'--agent-name node-02 --p2p-peer-address {alternatePeerEndpoint} --plugin sysio::net_api_plugin',
        '4': f'--agent-name node-04 --p2p-peer-address {cluster.getNodeP2pEndpoint(0)} --plugin sysio::net_api_plugin',
    }
    if cluster.launch(pnodes=pnodes, totalNodes=total_nodes, topo='line', delay=delay, activateIF=activateIF,
                      specificExtraNodeopArgs=specificArgs) is False:
        errorExit("Failed to stand up sys cluster.")

    # Be sure all nodes start out connected   (bios node omitted from diagram for brevity)
    #     node00              node01            node02            node03            node04
    #   localhost:9876 -> localhost:9877 -> localhost:9878 -> localhost:9879 -> localhost:9880
    # localhost:9779 ^                           |                                   |
    #       ^        +---------------------------+                                   |
    #       +------------------------------------------------------------------------+
    cluster.waitOnClusterSync(blockAdvancing=5)
    # Shut down bios node, which is connected to all other nodes in all topologies
    cluster.biosNode.kill(signal.SIGTERM)
    # Shut down second node, interrupting the default connections between it and nodes 00 and 02
    cluster.getNode(1).kill(signal.SIGTERM)
    # Shut down the fourth node, interrupting the default connections between it and nodes 02 and 04
    cluster.getNode(3).kill(signal.SIGTERM)
    # Be sure all remaining nodes continue to sync via the two listen ports on node 00
    #     node00            node01              node02            node03            node04
    #   localhost:9876     offline          localhost:9878       offline        localhost:9880
    # localhost:9779 ^                           |                                   |
    #       ^        +---------------------------+                                   |
    #       +------------------------------------------------------------------------+
    cluster.waitOnClusterSync(blockAdvancing=5)
    connections = cluster.nodes[0].processUrllibRequest('net', 'connections')
    open_socket_count = 0
    for conn in connections['payload']:
        if conn['is_socket_open']:
            open_socket_count += 1
            if conn['last_handshake']['agent'] == 'node-02':
                expectedEndpoint = cluster.getNodeP2pEndpoint(2)
                assert conn['last_handshake']['p2p_address'].split()[0] == expectedEndpoint, f"Connected node is listening on '{conn['last_handshake']['p2p_address'].split()[0]}' instead of {expectedEndpoint}"
            elif conn['last_handshake']['agent'] == 'node-04':
                expectedEndpoint = cluster.getNodeP2pEndpoint(4)
                assert conn['last_handshake']['p2p_address'].split()[0] == expectedEndpoint, f"Connected node is listening on '{conn['last_handshake']['p2p_address'].split()[0]}' instead of {expectedEndpoint}"
    assert open_socket_count == 2, 'Node 0 is expected to have exactly two open sockets'

    # Server addresses are paired positionally with listen endpoints:
    # default listen endpoint -> ext-ip0:20000, alternate listen endpoint -> ext-ip1:20001.
    node2AdvertisedAddress = getOpenPeerAddress(cluster.nodes[2])
    assert node2AdvertisedAddress == advertisedP2pEndpoint1, \
        f"Connected node is advertising '{node2AdvertisedAddress}' instead of {advertisedP2pEndpoint1}"

    node4AdvertisedAddress = getOpenPeerAddress(cluster.nodes[4])
    assert node4AdvertisedAddress == advertisedP2pEndpoint0, \
        f"Connected node is advertising '{node4AdvertisedAddress}' instead of {advertisedP2pEndpoint0}"

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
