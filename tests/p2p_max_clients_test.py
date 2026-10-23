#!/usr/bin/env python3
"""Exercise max-clients on raw sockets against a producing nodeop, at accept time and at the first handshake.

Without auto bp peering the limit is enforced when a connection is accepted. With it, the limit waits for a
connection's first handshake, when the node can tell a block producer from a client. Either way a node over the
limit admits a new client only in place of a misbehaving one, a client whose peer score is below the eviction
threshold, and otherwise refuses it, leaving every other connection alone.

* ``accept_limit``: without auto bp peering. Looking up a host through the net API finds an outbound connection but
  never an inbound one, not even by an empty host. A new client takes the slot of a low-score client, and is refused
  when every client is in good standing.
* ``handshake_limit``: with auto bp peering, the same at the handshake. A block producer recognized by its source
  address, the host of a p2p-auto-bp-peer entry, is neither counted against the limit nor evicted, however low its
  score.
* ``handshake_burst``: with auto bp peering, newcomers whose first handshakes are processed at the same time while one
  client is evictable: exactly one of them is admitted, in that client's place.

A raw peer lowers its own score by nacking blocks the node has made irreversible: each nack of the block after the
one it last nacked costs it the peer_scoring::block_nack penalty.
"""

import json
import shutil
import socket
import time
import urllib.request
from pathlib import Path

from TestHarness import TestHelper, Utils
from TestHarness.TestHelper import AppArgs
from TestHarness.raw_p2p import (KEEPALIVE_SECONDS, LOOPBACK, RESPONSE_TIMEOUT_SECONDS, connect, launch_nodeop,
                                 producer_args, run_peers_until, stop_node, wait_for_block, wait_for_chain_id,
                                 wait_for_log, write_genesis)


MAX_CLIENTS = 2
# Every client dials from loopback, a single subnet, so the per-subnet limit must not be what refuses one.
MAX_NODES_PER_SUBNET = 100
# The configured block producer dials from this loopback address, and every client from LOOPBACK.
BP_ADDRESS = "127.0.0.2"
BP_ACCOUNT = "prodbp"
# peer_scoring::baseline, peer_scoring::eviction_threshold and the peer_scoring::block_nack penalty.
BASELINE_SCORE = 100
EVICTION_THRESHOLD = 50
BLOCK_NACK_PENALTY = 2
NACKED_BLOCKS = 30
# The score the nacks leave. A keepalive tick within 2 s of a nack gives a point back, one or two ticks in all; after
# that a raw peer, which sends no blocks, gets none, so the score stays well below the threshold.
LOWERED_SCORE_MAX = BASELINE_SCORE - BLOCK_NACK_PENALTY * NACKED_BLOCKS + 2
PRODUCTION_TIMEOUT_SECONDS = 60
# Long enough for the node to act on a session it was going to close, at its next keepalive tick at the latest.
SETTLE_SECONDS = KEEPALIVE_SECONDS + 1
# Newcomers of a burst, and net threads to process their handshakes at the same time.
BURST_NEWCOMERS = 8
BURST_NET_THREADS = 8
BURST_ROUNDS = 3
# Time for the node to accept every newcomer of a burst before any of them handshakes.
ACCEPT_SECONDS = 0.5

ACCEPT_EVICTION_LOG = "Evicting low-score peer"
HANDSHAKE_EVICTION_LOG = "exceeded, evicting low-score peer"
CONNECTED = "added connection"
DISCONNECTED = "connection removed"
STATUS_NOT_FOUND = "connection not found: "
DISCONNECT_NOT_FOUND = "no known connection for host"


def launch_producer(name, extra_args):
    """Start a producing node with the net API and a max-clients limit; return it with its P2P port."""
    genesis_path, bios_key = write_genesis(Path(Utils.DataPath) / name)
    return launch_nodeop(f"{name}/node",
                         [*producer_args(bios_key), "--plugin", "sysio::net_api_plugin",
                          "--max-clients", str(MAX_CLIENTS), "--p2p-max-nodes-per-host", str(MAX_NODES_PER_SUBNET),
                          *extra_args],
                         genesis_path)


def wait_for_nackable_blocks(node):
    """Return the node's chain id once it has made NACKED_BLOCKS blocks irreversible."""
    chain_id = wait_for_chain_id(node)
    assert wait_for_block(node, "last_irreversible_block_num", NACKED_BLOCKS,
                          time.monotonic() + PRODUCTION_TIMEOUT_SECONDS), \
        f"Node did not make block {NACKED_BLOCKS} irreversible; see {node.data_dir / 'stderr.txt'}"
    return chain_id


def dial(p2p_port, chain_id, label, peers, source_address=None):
    """Dial the node and handshake, without waiting for the reply a refused client never gets; return the peer.

    The peer joins peers, the sessions the test keeps serving.
    """
    peer = connect(p2p_port, label, source_address)
    peers.append(peer)
    peer.send_handshake(chain_id)
    return peer


def admit(p2p_port, chain_id, label, peers, source_address=None):
    """Dial the node and handshake, serving every peer until the node answers; return the new peer."""
    peer = dial(p2p_port, chain_id, label, peers, source_address)
    assert run_peers_until(peers, lambda: peer.handshake_generations, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        f"{label}: node did not answer the handshake (closed: {peer.closed}, go_away {peer.describe_go_away()})"
    return peer


def connection_of(node, peer):
    """Return the node's open /v1/net/connections record of peer's session, or None."""
    host, port = peer.local_address
    for record in node.processUrllibRequest("net", "connections")["payload"]:
        if record["is_socket_open"] and record["remote_ip"] == host and record["remote_port"] == str(port):
            return record
    return None


def post_net(node, command, value):
    """POST value as a JSON body to /v1/net/<command>; return the decoded reply.

    Node.processUrllibRequest sends no body for an empty string, which the API rejects as missing.
    """
    request = urllib.request.Request(f"{node.endpointHttp}/v1/net/{command}", data=json.dumps(value).encode(),
                                     method="POST", headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=RESPONSE_TIMEOUT_SECONDS) as response:
        return json.load(response)


def lower_score(node, peer, peers):
    """Nack the node's first NACKED_BLOCKS blocks from peer, then wait until the node has applied every nack."""
    for block_num in range(1, NACKED_BLOCKS + 1):
        block = node.processUrllibRequest("chain", "get_block", {"block_num_or_id": block_num})["payload"]
        peer.send_block_nack(bytes.fromhex(block["id"]))

    def lowered():
        record = connection_of(node, peer)
        return record is not None and record["peer_score"] <= LOWERED_SCORE_MAX

    assert run_peers_until(peers, lowered, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        f"{peer.label}: node did not score the peer {LOWERED_SCORE_MAX} or less: {connection_of(node, peer)}"


def close_sessions(node, closing, peers):
    """Close the sessions in closing, serving the other peers until the node no longer lists any of them as open."""
    for peer in closing:
        peer.connection.close()
        peer.closed = True
    others = [peer for peer in peers if not peer.closed]
    assert run_peers_until(others, lambda: all(connection_of(node, peer) is None for peer in closing),
                           time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        f"Node still lists {[peer.label for peer in closing if connection_of(node, peer)]} as open"


def assert_open(peers):
    """Keep serving peers for SETTLE_SECONDS, then check that the node closed none of them."""
    run_peers_until(peers, lambda: False, time.monotonic() + SETTLE_SECONDS)
    closed = [peer.label for peer in peers if peer.closed]
    assert not closed, f"Node closed {closed}"


def assert_refused(peer, peers):
    """Check that the node closes peer's session without a handshake or a go-away."""
    assert run_peers_until(peers, lambda: peer.closed, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        f"{peer.label}: node did not refuse a client over max-clients"
    assert not peer.handshake_generations, f"{peer.label}: node answered the handshake before refusing it"
    assert peer.go_away is None, f"{peer.label}: node refused the client with go_away {peer.describe_go_away()}"


def assert_admitted_for(newcomer, victim, peers):
    """Check that the node evicts victim and answers newcomer's handshake."""
    assert run_peers_until(peers, lambda: victim.closed and newcomer.handshake_generations,
                           time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        (f"Node did not admit {newcomer.label} in place of {victim.label} (newcomer answered: "
         f"{bool(newcomer.handshake_generations)}, closed: {newcomer.closed}; {victim.label} closed: {victim.closed})")


def can_bind(address):
    """Whether a socket can bind address; macOS configures only 127.0.0.1 on its loopback interface."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind((address, 0))
        return True
    except OSError:
        return False


def run_accept_limit(case_index, args):
    """Without auto bp peering, max-clients is enforced as a connection is accepted."""
    node = None
    peers = []
    try:
        node, p2p_port = launch_producer("accept_limit", [])
        chain_id = wait_for_nackable_blocks(node)
        first = admit(p2p_port, chain_id, "first client", peers)
        second = admit(p2p_port, chain_id, "second client", peers)

        # A host lookup finds an outbound connection, but no inbound one, not even by an empty host.
        outbound = f"{LOOPBACK}:{Utils.getPort(Utils.PortAlternateP2P, case_index)}" # nothing listens there
        assert post_net(node, "connect", outbound) == CONNECTED, f"Node did not add {outbound}"
        status = post_net(node, "status", outbound)
        assert isinstance(status, dict) and status["peer"] == outbound, f"status of {outbound}: {status}"
        assert post_net(node, "status", "") == STATUS_NOT_FOUND, "status of an empty host found a connection"
        assert post_net(node, "disconnect", "") == DISCONNECT_NOT_FOUND, \
            "disconnect of an empty host found a connection"
        assert post_net(node, "disconnect", outbound) == DISCONNECTED, f"disconnect of {outbound} found nothing"
        assert_open(peers)

        lower_score(node, first, peers)
        newcomer = dial(p2p_port, chain_id, "newcomer", peers)
        assert_admitted_for(newcomer, first, peers)
        wait_for_log(node, ACCEPT_EVICTION_LOG)

        assert_refused(dial(p2p_port, chain_id, "refused client", peers), peers)
        assert_open([second, newcomer])
        Utils.Print("PASS: accept_limit, host lookups find no inbound connection and a client over max-clients takes "
                    "only a low-score client's slot")
    finally:
        for peer in peers:
            peer.connection.close()
        stop_node(node, args)


def run_handshake_limit(case_index, args):
    """With auto bp peering, max-clients is enforced at a connection's first handshake."""
    bp_port = Utils.getPort(Utils.PortAlternateP2P, case_index)
    node = None
    peers = []
    try:
        node, p2p_port = launch_producer("handshake_limit",
                                         ["--p2p-auto-bp-peer", f"{BP_ACCOUNT},{BP_ADDRESS}:{bp_port}"])
        chain_id = wait_for_nackable_blocks(node)
        first = admit(p2p_port, chain_id, "first client", peers)
        second = admit(p2p_port, chain_id, "second client", peers)

        # Over the limit with every client in good standing, the newcomer is refused and no one else is touched.
        assert_refused(dial(p2p_port, chain_id, "refused client", peers), peers)
        assert_open([first, second])

        lower_score(node, first, peers)
        newcomer = dial(p2p_port, chain_id, "newcomer", peers)
        assert_admitted_for(newcomer, first, peers)
        wait_for_log(node, HANDSHAKE_EVICTION_LOG)
        assert_open([second, newcomer])

        if not can_bind(BP_ADDRESS):
            Utils.Print(f"SKIP: handshake_limit configured bp, cannot bind {BP_ADDRESS} on this host")
            Utils.Print("PASS: handshake_limit, a client over max-clients takes only a low-score client's slot")
            return
        # With a client slot free, the bp, recognized by its source address, does not take it.
        close_sessions(node, [newcomer], peers)
        bp = admit(p2p_port, chain_id, "configured bp", peers, source_address=(BP_ADDRESS, 0))
        record = connection_of(node, bp)
        assert record is not None and record["is_bp_peer"], f"Node did not recognize the configured bp: {record}"
        third = admit(p2p_port, chain_id, "third client", peers)

        # However low its score, the bp keeps its slot, so a further client is refused.
        lower_score(node, bp, peers)
        assert_refused(dial(p2p_port, chain_id, "late client", peers), peers)
        record = connection_of(node, bp)
        assert record is not None and record["peer_score"] < EVICTION_THRESHOLD, \
            f"The bp's score rose to the threshold before the late client was refused: {record}"
        assert_open([second, third, bp])
        Utils.Print("PASS: handshake_limit, a client over max-clients takes only a low-score client's slot and a "
                    "configured bp is never counted or evicted")
    finally:
        for peer in peers:
            peer.connection.close()
        stop_node(node, args)


def run_handshake_burst(case_index, args):
    """With auto bp peering, newcomers handshaking at once while one client is evictable: exactly one is admitted."""
    bp_port = Utils.getPort(Utils.PortAlternateP2P, case_index)
    node = None
    peers = []
    try:
        node, p2p_port = launch_producer("handshake_burst",
                                         ["--p2p-auto-bp-peer", f"{BP_ACCOUNT},{BP_ADDRESS}:{bp_port}",
                                          "--net-threads", str(BURST_NET_THREADS)])
        chain_id = wait_for_nackable_blocks(node)
        keeper = admit(p2p_port, chain_id, "keeper", peers)
        for burst_round in range(BURST_ROUNDS):
            victim = admit(p2p_port, chain_id, f"victim {burst_round}", peers)
            lower_score(node, victim, peers)
            # The node accepts every newcomer first, so that their handshakes then arrive back to back.
            newcomers = [connect(p2p_port, f"newcomer {burst_round}.{index}") for index in range(BURST_NEWCOMERS)]
            peers.extend(newcomers)
            run_peers_until(peers, lambda: False, time.monotonic() + ACCEPT_SECONDS)
            for newcomer in newcomers:
                newcomer.send_handshake(chain_id)

            def decided():
                return victim.closed and all(peer.closed or peer.handshake_generations for peer in newcomers)

            assert run_peers_until(peers, decided, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
                f"round {burst_round}: node did not decide every newcomer (victim closed: {victim.closed})"
            run_peers_until(peers, lambda: False, time.monotonic() + SETTLE_SECONDS)
            admitted = [peer for peer in newcomers if peer.handshake_generations and not peer.closed]
            assert len(admitted) == 1, \
                f"round {burst_round}: node admitted {[peer.label for peer in admitted]} in place of one victim"
            assert not keeper.closed, f"round {burst_round}: node closed the keeper"
            close_sessions(node, admitted, peers)
        Utils.Print(f"PASS: handshake_burst, {BURST_ROUNDS} bursts of {BURST_NEWCOMERS} newcomers each admitted "
                    "exactly one, in place of the low-score client")
    finally:
        for peer in peers:
            peer.connection.close()
        stop_node(node, args)


CASES = {
    "accept_limit": run_accept_limit,
    "handshake_limit": run_handshake_limit,
    "handshake_burst": run_handshake_burst,
}


def main():
    """Run the selected cases and keep node logs on any failure."""
    application_args = AppArgs()
    application_args.add("--case", str, "max-clients case to exercise", default="all", choices=["all", *CASES])
    args = TestHelper.parse_args({"--keep-logs", "--leave-running", "-v"}, application_args)
    Utils.Debug = args.v
    successful = False
    try:
        for case_index, (case_name, run_case) in enumerate(CASES.items()):
            if args.case in ("all", case_name):
                run_case(case_index, args)
        successful = True
    finally:
        if successful and not (args.keep_logs or args.leave_running):
            shutil.rmtree(Utils.DataPath, ignore_errors=True)
        Utils.Print(f"Max clients test {'passed' if successful else 'failed'}; logs: {Utils.DataPath}")


if __name__ == "__main__":
    main()
