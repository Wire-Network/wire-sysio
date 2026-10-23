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
* ``handshake_rejections``: with auto bp peering, a session that opens with a handshake other than generation 1, or
  on another fork, is refused, and nothing it sent after the rejected handshake is processed. Newcomers rejected for a
  wrong chain id hold no slot, so a valid newcomer handshaking with them still gets the free one.

A raw peer lowers its own score by nacking blocks the node has made irreversible: each nack of the block after the
one it last nacked costs it the peer_scoring::block_nack penalty.

Every port the test binds or configures, the source ports of its raw sessions included, comes from the test's
bind-registry lease through Utils.getPort.
"""

import json
import secrets
import shutil
import socket
import time
import urllib.request
from pathlib import Path

import TestHarness
from TestHarness import TestHelper, Utils
from TestHarness.TestHelper import AppArgs
from TestHarness.raw_p2p import (BLOCK_ID_BYTES, KEEPALIVE_SECONDS, LOOPBACK, RESPONSE_TIMEOUT_SECONDS, GoAwayReason,
                                 connect, launch_nodeop, node_log, producer_args, run_peers_until, stop_node,
                                 wait_for_block, wait_for_chain_id, wait_for_log, write_genesis)


MAX_CLIENTS = 2
# Every client dials from loopback, a single subnet, so the per-subnet limit must not be what refuses one.
MAX_NODES_PER_SUBNET = 100
# The configured block producer dials from this loopback address, and every client from LOOPBACK.
BP_ADDRESS = "127.0.0.2"
BP_ACCOUNT = "prodbp"
# The bp's source port, from an alternate-P2P slot clear of the per-case ones.
BP_SOURCE_PORT_INDEX = 40
# peer_scoring::eviction_threshold.
EVICTION_THRESHOLD = 50
# Consecutive nacks, sent in one burst, take a baseline score of 100 to 40. A keepalive tick within 2 s of a nack gives
# a point back, so a slow node working through the burst can return a few; after that a raw peer, which sends no blocks,
# gets none. Waiting for the margin below the threshold leaves the peer evictable for the rest of the test.
NACKED_BLOCKS = 30
SCORE_MARGIN = 5
LOWERED_SCORE_MAX = EVICTION_THRESHOLD - SCORE_MARGIN
PRODUCTION_TIMEOUT_SECONDS = 60
# Long enough for the node to act on a session it was going to close, at its next keepalive tick at the latest.
SETTLE_SECONDS = KEEPALIVE_SECONDS + 1
# Newcomers of a burst, and net threads to process their handshakes at the same time.
BURST_NEWCOMERS = 8
BURST_NET_THREADS = 8
BURST_ROUNDS = 3
# Newcomers rejected for a wrong chain id in each burst, beside one valid newcomer.
REJECTED_PER_BURST = 7
REJECTION_ROUNDS = 5
# A first handshake that skips generation 1.
SKIPPED_GENERATION = 2
# The node reads one message per completion, so whether it would go on to process the frames sent after a rejected
# handshake depends on the order its strand runs the go-away's write and the next reads; each of these sessions is
# another chance for a node without the fix to do so.
COALESCED_SESSIONS = 32
CHAIN_ID_BYTES = 32
# A block id starts with its block number, big-endian; block 1 is the genesis block every node has.
BLOCK_NUM_BYTES = 4
GENESIS_BLOCK_NUM = 1
# A block the node has not made, which a turned-away session asks for after its rejected handshake.
UNKNOWN_BLOCK_NUM = 4_242_424
# Blocks the node makes before the rejections, so that a session it went on to admit would have blocks to sync.
PRODUCED_BLOCKS = 3
# What the node logs, at debug level, when it negotiates sync for a session it has admitted.
SYNC_NEGOTIATION_LOG = "] handshake msg.froot"
# The logger whose level the net_plugin loggers take when the logging config names none of them.
P2P_PARENT_LOGGER = "net_plugin_impl"
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


def write_debug_p2p_logging(directory):
    """Write the harness logging config, with the net_plugin loggers at debug level, into directory; return its path."""
    config = json.loads((Path(TestHarness.__file__).parent / "logging-template.json").read_text())
    for logger in config["loggers"]:
        if logger["name"] == P2P_PARENT_LOGGER:
            logger["level"] = "debug"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "logging.json"
    path.write_text(json.dumps(config, indent=2))
    return path


def negotiated_sync(node, peer):
    """Whether the node negotiated sync for peer's session, which it does only for a session it admitted."""
    session = f' - {peer.short_node_id}" - '
    return any(session in line and SYNC_NEGOTIATION_LOG in line for line in node_log(node).splitlines())


def block_id(node, block_num):
    """Return the id of the node's block block_num."""
    block = node.processUrllibRequest("chain", "get_block", {"block_num_or_id": block_num})["payload"]
    return bytes.fromhex(block["id"])


def lower_score(node, peer, peers):
    """Nack the node's first NACKED_BLOCKS blocks from peer in one burst, then wait until the score is well below the
    eviction threshold."""
    nacked_ids = [block_id(node, block_num) for block_num in range(1, NACKED_BLOCKS + 1)]
    for nacked_id in nacked_ids:
        peer.send_block_nack(nacked_id)

    def lowered():
        record = connection_of(node, peer)
        return record is not None and record["peer_score"] <= LOWERED_SCORE_MAX

    assert run_peers_until(peers, lowered, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        f"{peer.label}: node did not score the peer {LOWERED_SCORE_MAX} or less: {connection_of(node, peer)}"


def close_sessions(node, closing, peers):
    """Close the sessions in closing, serving the other peers until the node no longer lists any of them as open."""
    for peer in closing:
        peer.close()
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


def assert_turned_away(peer, data, reason, peers):
    """Send data in one write; check that the node answers with go_away reason and closes, never sending a handshake."""
    peer.send(data)
    assert run_peers_until(peers, lambda: peer.closed, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        f"{peer.label}: node kept the session"
    assert peer.go_away is reason, f"{peer.label}: node sent go_away {peer.describe_go_away()}, not {reason.name}"
    assert not peer.handshake_generations, f"{peer.label}: node answered a handshake of a session it turned away"


def assert_admitted_for(newcomer, victim, peers):
    """Check that the node evicts victim and answers newcomer's handshake."""
    assert run_peers_until(peers, lambda: victim.closed and newcomer.handshake_generations,
                           time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        (f"Node did not admit {newcomer.label} in place of {victim.label} (newcomer answered: "
         f"{bool(newcomer.handshake_generations)}, closed: {newcomer.closed}; {victim.label} closed: {victim.closed})")


def can_bind(source_address):
    """Whether a socket can bind source_address; macOS configures only 127.0.0.1 on its loopback interface."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(source_address)
        return True
    except OSError:
        return False


def stop_and_close(node, peers, args):
    """Stop the node, then close every session."""
    stop_node(node, args)
    for peer in peers:
        peer.close()


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
        stop_and_close(node, peers, args)


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

        bp_source = (BP_ADDRESS, Utils.getPort(Utils.PortAlternateP2P, BP_SOURCE_PORT_INDEX))
        if not can_bind(bp_source):
            Utils.Print(f"SKIP: handshake_limit configured bp, cannot bind {BP_ADDRESS} on this host")
            Utils.Print("PASS: handshake_limit, a client over max-clients takes only a low-score client's slot")
            return
        # With a client slot free, the bp, recognized by its source address, does not take it.
        close_sessions(node, [newcomer], peers)
        bp = admit(p2p_port, chain_id, "configured bp", peers, source_address=bp_source)
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
        stop_and_close(node, peers, args)


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
        stop_and_close(node, peers, args)


def run_handshake_rejections(case_index, args):
    """With auto bp peering, a rejected handshake neither holds a client slot nor keeps a valid newcomer out of one."""
    bp_port = Utils.getPort(Utils.PortAlternateP2P, case_index)
    node = None
    peers = []
    try:
        logging_config = write_debug_p2p_logging(Path(Utils.DataPath) / "handshake_rejections")
        node, p2p_port = launch_producer("handshake_rejections",
                                         ["--p2p-auto-bp-peer", f"{BP_ACCOUNT},{BP_ADDRESS}:{bp_port}",
                                          "--net-threads", str(BURST_NET_THREADS), "--logconf", str(logging_config)])
        chain_id = wait_for_chain_id(node)
        assert wait_for_block(node, "head_block_num", PRODUCED_BLOCKS, time.monotonic() + PRODUCTION_TIMEOUT_SECONDS), \
            f"Node did not produce block {PRODUCED_BLOCKS}; see {node.data_dir / 'stderr.txt'}"
        keeper = admit(p2p_port, chain_id, "keeper", peers)

        # A session that opens past generation 1, or on another fork, is refused, and a queued go-away ends the session:
        # a valid handshake and a block request sent with the rejected handshake in the same write are not processed,
        # so the node never negotiates sync for it, as it does for the admitted keeper.
        assert run_peers_until(peers, lambda: negotiated_sync(node, keeper),
                               time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
            f"Node logged no '{SYNC_NEGOTIATION_LOG}' for the keeper; see {node.data_dir / 'stderr.txt'}"
        genesis_id = block_id(node, GENESIS_BLOCK_NUM)
        unknown_head = UNKNOWN_BLOCK_NUM.to_bytes(BLOCK_NUM_BYTES, "big") + secrets.token_bytes(
            BLOCK_ID_BYTES - BLOCK_NUM_BYTES)
        # The genesis block number with an id the node does not have: the node sees the peer on another fork.
        forked_root = GENESIS_BLOCK_NUM.to_bytes(BLOCK_NUM_BYTES, "big") + secrets.token_bytes(
            BLOCK_ID_BYTES - BLOCK_NUM_BYTES)
        skipper = connect(p2p_port, "generation skipper")
        peers.append(skipper)
        assert_turned_away(skipper, skipper.handshake_frame(chain_id, generation=SKIPPED_GENERATION),
                           GoAwayReason.fatal_other, peers)
        coalesced_skippers = []
        for index in range(COALESCED_SESSIONS):
            coalesced = connect(p2p_port, f"coalesced skipper {index}")
            peers.append(coalesced)
            coalesced_skippers.append(coalesced)
            assert_turned_away(coalesced, (coalesced.handshake_frame(chain_id, generation=SKIPPED_GENERATION) +
                                           coalesced.handshake_frame(chain_id, fork_db_root_id=genesis_id) +
                                           coalesced.block_request_frame(unknown_head)),
                               GoAwayReason.fatal_other, peers)
        forked = connect(p2p_port, "forked")
        peers.append(forked)
        assert_turned_away(forked, (forked.handshake_frame(chain_id, fork_db_root_id=forked_root) +
                                    forked.block_request_frame(unknown_head)),
                           GoAwayReason.forked, peers)
        turned_away_processed = [peer.label for peer in (skipper, *coalesced_skippers, forked)
                                 if negotiated_sync(node, peer)]
        assert not turned_away_processed, f"Node negotiated sync for sessions it turned away: {turned_away_processed}"

        # Newcomers rejected for their chain id hold no slot, so the valid newcomer among them gets the one the keeper
        # leaves free.
        for burst_round in range(REJECTION_ROUNDS):
            rejected = [connect(p2p_port, f"wrong chain {burst_round}.{index}") for index in range(REJECTED_PER_BURST)]
            valid = connect(p2p_port, f"valid newcomer {burst_round}")
            peers.extend([*rejected, valid])
            run_peers_until(peers, lambda: False, time.monotonic() + ACCEPT_SECONDS)
            for peer in rejected:
                peer.send_handshake(secrets.token_bytes(CHAIN_ID_BYTES))
            valid.send_handshake(chain_id)

            def decided():
                return valid.closed or valid.handshake_generations

            assert run_peers_until(peers, decided, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
                f"round {burst_round}: node did not decide the valid newcomer"
            assert valid.handshake_generations and not valid.closed, \
                (f"round {burst_round}: node refused the valid newcomer while rejecting "
                 f"{REJECTED_PER_BURST} newcomers for a wrong chain id")
            assert run_peers_until(peers, lambda: all(peer.closed for peer in rejected),
                                   time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
                f"round {burst_round}: node kept a newcomer with a wrong chain id"
            wrong = [peer.label for peer in rejected if peer.go_away is not GoAwayReason.wrong_chain]
            assert not wrong, f"round {burst_round}: {wrong} were not sent go_away wrong_chain"
            assert not keeper.closed, f"round {burst_round}: node closed the keeper"
            close_sessions(node, [valid], peers)
        Utils.Print(f"PASS: handshake_rejections, sessions opening past generation 1 or on another fork are refused "
                    f"with nothing after the rejected handshake processed, and {REJECTION_ROUNDS} valid newcomers each "
                    f"got the free slot beside {REJECTED_PER_BURST} rejected ones")
    finally:
        stop_and_close(node, peers, args)


CASES = {
    "accept_limit": run_accept_limit,
    "handshake_limit": run_handshake_limit,
    "handshake_burst": run_handshake_burst,
    "handshake_rejections": run_handshake_rejections,
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
