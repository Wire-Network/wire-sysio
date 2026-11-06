#!/usr/bin/env python3
"""Exercise the configured heartbeat timeout on real incoming and outgoing sockets.

Peers exchange valid time messages under ``allowed-connection any``, then stay
silent. No handshake or authentication failure can close these open-policy
connections: time messages are allowed before session establishment, and the
node confirms receiving each message with a matching time response.
"""

import json
import select
import shutil
import signal
import socket
import struct
import time
from pathlib import Path

from TestHarness import Node, TestHelper, Utils
from TestHarness.TestHelper import AppArgs
from TestHarness.launcher import cluster_generator


LOOPBACK = "127.0.0.1"
DEFAULT_KEEPALIVE_MS = 10_000
CUSTOM_KEEPALIVE_MS = 1_000
MILLISECONDS_PER_SECOND = 1_000
HEARTBEAT_TIMEOUT_INTERVALS = 2
TICK_ALIGNMENT_FRACTION = 0.5
DEFAULT_SILENCE_SECONDS = 16
STARTUP_TIMEOUT_SECONDS = 30
RESPONSE_TIMEOUT_SECONDS = 5
SCHEDULING_ALLOWANCE_SECONDS = 2
POLL_SECONDS = 0.1
RECV_BYTES = 64 * 1024
MAX_FRAME_BYTES = 1024 * 1024
FRAME_LENGTH = struct.Struct("<I")
TIME_FIELDS = struct.Struct("<qqqq")
HANDSHAKE_MESSAGE_TAG = 0
GO_AWAY_MESSAGE_TAG = 1
TIME_MESSAGE_TAG = 2


class SilentPeer:
    """Track framed node messages while withholding automatic heartbeat replies."""

    def __init__(self, connection, direction):
        """Own a connected socket representing the specified node connection direction."""
        self.connection = connection
        self.direction = direction
        self.buffer = bytearray()
        self.closed = False
        self.handshakes = 0
        self.expected_reply = None
        self.last_reply = None

    def ping(self):
        """Send a unique valid time message whose response proves node receipt."""
        self.expected_reply = time.time_ns()
        payload = bytes([TIME_MESSAGE_TAG]) + TIME_FIELDS.pack(0, 0, self.expected_reply, 0)
        self.connection.sendall(FRAME_LENGTH.pack(len(payload)) + payload)

    def receive(self):
        """Drain available bytes and distinguish heartbeat closure from a protocol rejection."""
        data = self.connection.recv(RECV_BYTES)
        if not data:
            self.closed = True
            return
        self.buffer.extend(data)
        while len(self.buffer) >= FRAME_LENGTH.size:
            length = FRAME_LENGTH.unpack_from(self.buffer)[0]
            assert 0 < length <= MAX_FRAME_BYTES, f"Invalid P2P frame length: {length}"
            frame_end = FRAME_LENGTH.size + length
            if len(self.buffer) < frame_end:
                return
            payload = self.buffer[FRAME_LENGTH.size:frame_end]
            del self.buffer[:frame_end]
            message_tag = payload[0]
            assert message_tag != GO_AWAY_MESSAGE_TAG, f"{self.direction} peer received a go-away message"
            if message_tag == HANDSHAKE_MESSAGE_TAG:
                self.handshakes += 1
            elif message_tag == TIME_MESSAGE_TAG:
                origin = TIME_FIELDS.unpack(payload[1:])[0]
                if origin == self.expected_reply:
                    self.last_reply = origin


def poll_until(peers, predicate, deadline):
    """Read both sockets until a condition, EOF on both peers, or a monotonic deadline."""
    while not predicate():
        remaining = deadline - time.monotonic()
        live = {peer.connection: peer for peer in peers if not peer.closed}
        if remaining <= 0 or not live:
            return False
        readable, _, _ = select.select(list(live), [], [], min(POLL_SECONDS, remaining))
        for connection in readable:
            live[connection].receive()
    return True


def assert_open(peers):
    """Require both connection directions to survive the protected inactivity interval."""
    for peer in peers:
        assert not peer.closed, f"{peer.direction} connection closed before its configured heartbeat timeout"


def ping_and_confirm(peers):
    """Reset inactivity through actual message processing, then wait for both acknowledgements."""
    for peer in peers:
        peer.ping()
    assert poll_until(
        peers,
        lambda: all(peer.last_reply == peer.expected_reply for peer in peers),
        time.monotonic() + RESPONSE_TIMEOUT_SECONDS,
    ), "Node did not acknowledge both peers' time messages"
    assert_open(peers)


def write_genesis(node_dir):
    """Reuse the launcher's valid genesis configuration and BIOS finalizer key."""
    genesis_path = node_dir / "genesis.json"
    launcher = cluster_generator(["--nodes", "1", "--pnodes", "1", "--producers", "1",
                                  "--genesis", str(genesis_path)])
    launcher.define_network()
    launcher.bind_nodes()
    genesis_path.write_text(json.dumps(launcher.init_genesis(), indent=2))
    return genesis_path


def accept_outgoing(listener, node):
    """Wait for node connection while reporting an exited node immediately."""
    deadline = time.monotonic() + STARTUP_TIMEOUT_SECONDS
    listener.settimeout(POLL_SECONDS)
    while time.monotonic() < deadline:
        assert node.verifyAlive(silent=True), f"Node exited during startup; see {node.data_dir / 'stderr.txt'}"
        try:
            connection, _ = listener.accept()
            return connection
        except socket.timeout:
            continue
    raise AssertionError(f"Node did not connect to its configured peer; see {node.data_dir / 'stderr.txt'}")


def run_case(case_index, keepalive_ms, args):
    """Check default or custom timeout behavior on both actual connection constructors."""
    interval = keepalive_ms / MILLISECONDS_PER_SECOND
    case_name = "default" if keepalive_ms == DEFAULT_KEEPALIVE_MS else "custom"
    node_dir = Path(Utils.DataPath) / case_name
    node_dir.mkdir(parents=True, exist_ok=True)
    genesis_path = write_genesis(node_dir)
    http_port = Utils.getPort(Utils.PortNodeHttp, case_index)
    p2p_port = Utils.getPort(Utils.PortP2P, case_index)
    peer_port = Utils.getPort(Utils.PortAlternateP2P, case_index)
    node = None
    peers = []
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((LOOPBACK, peer_port))
        listener.listen()
        command = [
            Utils.SysServerPath,
            "--config-dir", str(node_dir),
            "--data-dir", str(node_dir),
            "--genesis-json", str(genesis_path),
            "--plugin", "sysio::chain_api_plugin",
            "--plugin", "sysio::net_plugin",
            "--http-server-address", f"{LOOPBACK}:{http_port}",
            "--p2p-listen-endpoint", f"{LOOPBACK}:{p2p_port}",
            "--p2p-peer-address", f"{LOOPBACK}:{peer_port}",
            "--allowed-connection", "any",
        ]
        if keepalive_ms != DEFAULT_KEEPALIVE_MS:
            command.extend(["--p2p-keepalive-interval-ms", str(keepalive_ms)])
        try:
            node = Node(LOOPBACK, http_port, case_index, node_dir, node_dir, command)
            outgoing = accept_outgoing(listener, node)
            peers.append(SilentPeer(outgoing, "outgoing"))
            incoming = socket.create_connection((LOOPBACK, p2p_port), timeout=STARTUP_TIMEOUT_SECONDS)
            peers.append(SilentPeer(incoming, "incoming"))
            ping_and_confirm(peers)

            if keepalive_ms == DEFAULT_KEEPALIVE_MS:
                # Observe a real ticker invocation, then place the last message
                # halfway between ticks. The following ticks see 5s and 15s of
                # inactivity, exposing the old incoming 10s timeout reliably.
                handshakes = [peer.handshakes for peer in peers]
                assert poll_until(
                    peers,
                    lambda: all(peer.handshakes > previous for peer, previous in zip(peers, handshakes)),
                    time.monotonic() + interval + SCHEDULING_ALLOWANCE_SECONDS,
                ), "Did not observe the node's periodic heartbeat checks"
                assert_open(peers)
                poll_until(peers, lambda: False, time.monotonic() + interval * TICK_ALIGNMENT_FRACTION)
                assert_open(peers)
                ping_and_confirm(peers)

            silent_since = time.monotonic()
            protected_interval = DEFAULT_SILENCE_SECONDS if case_name == "default" else interval / 2
            poll_until(peers, lambda: False, silent_since + protected_interval)
            assert_open(peers)

            # One ticker interval beyond the configured timeout accommodates its
            # phase. A custom 1s keepalive must close well before the old 10s
            # incoming default, so a hard-coded 20s fix also fails this case.
            close_deadline = silent_since + interval * (HEARTBEAT_TIMEOUT_INTERVALS + 1)
            close_deadline += SCHEDULING_ALLOWANCE_SECONDS
            assert poll_until(peers, lambda: all(peer.closed for peer in peers), close_deadline), (
                f"{case_name}: peers exceeded the configured timeout: "
                f"{[peer.direction for peer in peers if not peer.closed]}"
            )
            assert node.verifyAlive(), "Node exited instead of timing out silent peers"
            log = (node_dir / "stderr.txt").read_text()
            timeouts = [line for line in log.splitlines()
                        if "heartbeat timed out" in line and "half heartbeat" not in line]
            assert len(timeouts) >= len(peers), "Expected heartbeat timeout diagnostics"
            assert "Peer did not authenticate in time" not in log, "Authentication confounded heartbeat test"
            Utils.Print(f"PASS: {case_name} incoming/outgoing heartbeat timeout ({keepalive_ms}ms keepalive)")
        finally:
            for peer in peers:
                peer.connection.close()
            if node is not None and not args.leave_running:
                node.kill(signal.SIGTERM)


def main():
    """Run both configuration cases and preserve node logs on any failure."""
    application_args = AppArgs()
    application_args.add("--case", str, "Heartbeat configuration to exercise", default="all",
                         choices=["all", "default", "custom"])
    args = TestHelper.parse_args({"--keep-logs", "--leave-running", "-v"}, application_args)
    Utils.Debug = args.v
    successful = False
    try:
        for case_index, (case_name, keepalive_ms) in enumerate(
            [("default", DEFAULT_KEEPALIVE_MS), ("custom", CUSTOM_KEEPALIVE_MS)]
        ):
            if args.case in ("all", case_name):
                run_case(case_index, keepalive_ms, args)
        successful = True
    finally:
        if successful and not (args.keep_logs or args.leave_running):
            shutil.rmtree(Utils.DataPath, ignore_errors=True)
        Utils.Print(f"Heartbeat regression {'passed' if successful else 'failed'}; logs: {Utils.DataPath}")


if __name__ == "__main__":
    main()
