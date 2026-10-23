"""Raw-socket P2P sessions with a standalone nodeop, for tests that need exact control of what crosses the wire.

A RawPeer speaks just enough of the net_plugin protocol to hold a session open: framing, the handshake, time
messages, block nacks and peer_auth_message. It records the node's handshakes, pings and go-away. The module also
launches a single nodeop on loopback, outside any Cluster, and waits on what that node logs and reports.
"""

import hashlib
import itertools
import json
import secrets
import select
import signal
import socket
import struct
import time
from enum import IntEnum
from pathlib import Path

from .Node import Node
from .launcher import cluster_generator
from .testUtils import Utils


LOOPBACK = "127.0.0.1"
AGENT_NAME = "raw-p2p-test"
# The launched node's keepalive, short enough that heartbeats tick within a test.
KEEPALIVE_MS = 2_000
MILLISECONDS_PER_SECOND = 1_000
KEEPALIVE_SECONDS = KEEPALIVE_MS / MILLISECONDS_PER_SECOND
PING_INTERVAL_SECONDS = KEEPALIVE_SECONDS / 2
CONNECTION_CLEANUP_PERIOD_SECONDS = 1
STARTUP_TIMEOUT_SECONDS = 30
RESPONSE_TIMEOUT_SECONDS = 5
POLL_SECONDS = 0.1
RECV_BYTES = 64 * 1024
MAX_FRAME_BYTES = 1024 * 1024
PRODUCER_NAME = "sysio"
# The native genesis timestamp format keeps milliseconds and drops the remaining microsecond digits.
SUB_MILLISECOND_DIGITS = 3
# Each launched nodeop takes the next index for its HTTP and P2P ports.
NODE_INDEXES = itertools.count()

FRAME_LENGTH = struct.Struct("<I")
TIME_FIELDS = struct.Struct("<qqqq")
GO_AWAY_REASON = struct.Struct("<i")
# handshake_message fields ahead of its two strings: network_version, chain_id, node_id,
# fork_db_root_id, fork_db_head_id, chain_head_id, generation.
HANDSHAKE_FIELDS = struct.Struct("<H32s32s32s32s32sh")
NETWORK_VERSION = 1
INITIAL_GENERATION = 1
NODE_ID_BYTES = 32
BLOCK_ID_BYTES = 32
ZERO_BLOCK_ID = bytes(BLOCK_ID_BYTES)
VARINT_CONTINUATION_BIT = 0x80
# fc::crypto key and signature variants pack their index first; K1 is index 0.
K1_VARIANT_INDEX = 0


class MessageTag(IntEnum):
    """net_message variant indices a RawPeer sends or reads (net_plugin protocol.hpp)."""
    handshake = 0
    go_away = 1
    time = 2
    block_nack = 9
    peer_auth = 14


class GoAwayReason(IntEnum):
    """sysio::go_away_reason, which fc::raw packs as a little-endian int32."""
    no_reason = 0
    self = 1
    duplicate = 2
    wrong_chain = 3
    wrong_version = 4
    forked = 5
    validation = 6
    benign_other = 7
    fatal_other = 8
    authentication = 9


def frame(tag, body=b""):
    """Serialize a net_message: little-endian length, then its variant index and fields."""
    payload = bytes([tag]) + body
    return FRAME_LENGTH.pack(len(payload)) + payload


def pack_string(text):
    """Pack a short string as fc::raw does: one varint length byte, then the bytes."""
    data = text.encode()
    assert len(data) < VARINT_CONTINUATION_BIT, f"String needs a multi-byte length: {text}"
    return bytes([len(data)]) + data


class RawPeer:
    """One P2P session with nodeop over a raw socket, recording what the node sends."""

    def __init__(self, connection, label):
        """Own a connected socket and pick this session's random node id."""
        self.connection = connection
        self.label = label
        # (host, port) of our end, which the node reports as the session's remote address; kept past a close.
        self.local_address = connection.getsockname()[:2]
        self.node_id = secrets.token_bytes(NODE_ID_BYTES)
        self.buffer = bytearray()
        self.closed = False
        self.last_ping = None
        self.chain_id = None
        self.remote_node_id = None
        self.handshake_generations = []
        self.node_pings = 0
        self.go_away = None
        self.go_away_time = None

    def send(self, data):
        """Write to the node; a reset socket counts as closed."""
        try:
            self.connection.sendall(data)
        except OSError:
            self.closed = True

    def send_handshake(self, chain_id, generation=INITIAL_GENERATION):
        """Send a handshake for an empty chain; a first one of generation 1 is what the node accepts."""
        fields = HANDSHAKE_FIELDS.pack(NETWORK_VERSION, chain_id, self.node_id, ZERO_BLOCK_ID, ZERO_BLOCK_ID,
                                       ZERO_BLOCK_ID, generation)
        address = f"{LOOPBACK}:{self.connection.getsockname()[1]}"
        self.send(frame(MessageTag.handshake, fields + pack_string(address) + pack_string(AGENT_NAME)))

    def auth_digest(self):
        """Return the digest the node verifies: sha256(node id, our node id, chain id), see peer_auth.hpp."""
        return hashlib.sha256(self.remote_node_id + self.node_id + self.chain_id).digest()

    def send_peer_auth(self, public_key, signature):
        """Send a peer_auth_message claiming a K1 public key with a K1 signature."""
        body = bytes([K1_VARIANT_INDEX]) + public_key + bytes([K1_VARIANT_INDEX]) + signature
        self.send(frame(MessageTag.peer_auth, body))

    def send_block_nack(self, block_id):
        """Send a block_nack_message: tell the node we already have the block with this 32-byte id."""
        assert len(block_id) == BLOCK_ID_BYTES, f"Block id must be {BLOCK_ID_BYTES} bytes, got {len(block_id)}"
        self.send(frame(MessageTag.block_nack, block_id))

    def ping(self):
        """Send a time message, keeping the node's heartbeat timeout away from the session."""
        self.last_ping = time.monotonic()
        self.send(frame(MessageTag.time, TIME_FIELDS.pack(0, 0, time.time_ns(), 0)))

    def receive(self):
        """Drain available bytes and record the node's handshakes, pings and go-away."""
        try:
            data = self.connection.recv(RECV_BYTES)
        except ConnectionResetError:
            data = b""
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
            payload = bytes(self.buffer[FRAME_LENGTH.size:frame_end])
            del self.buffer[:frame_end]
            tag = payload[0]
            if tag == MessageTag.handshake:
                _, self.chain_id, self.remote_node_id, _, _, _, generation = HANDSHAKE_FIELDS.unpack_from(payload, 1)
                self.handshake_generations.append(generation)
            elif tag == MessageTag.time:
                # An origin of zero marks a ping the node started, not a reply to ours.
                if TIME_FIELDS.unpack_from(payload, 1)[0] == 0:
                    self.node_pings += 1
            elif tag == MessageTag.go_away and self.go_away is None:
                self.go_away = GoAwayReason(GO_AWAY_REASON.unpack_from(payload, 1)[0])
                self.go_away_time = time.monotonic()

    def run_until(self, predicate, deadline, keepalive=True):
        """Read, pinging unless keepalive is off, until predicate() holds, the node closes, or the deadline.

        Returns predicate().
        """
        while not predicate():
            now = time.monotonic()
            if self.closed:
                return predicate()
            if now >= deadline:
                # Read what already arrived first: a stalled host can wake this loop late.
                if select.select([self.connection], [], [], 0)[0]:
                    self.receive()
                return predicate()
            if keepalive and (self.last_ping is None or now - self.last_ping >= PING_INTERVAL_SECONDS):
                self.ping()
            readable, _, _ = select.select([self.connection], [], [], min(POLL_SECONDS, deadline - now))
            if readable:
                self.receive()
        return True

    def describe_go_away(self):
        """Name the node's go-away reason for assertion messages."""
        return "none" if self.go_away is None else self.go_away.name


def run_peers_until(peers, predicate, deadline):
    """Serve several sessions at once, reading and pinging each open one, until predicate() holds, the deadline,
    or every session is closed.

    Returns predicate().
    """
    while not predicate():
        now = time.monotonic()
        open_peers = [peer for peer in peers if not peer.closed]
        if not open_peers:
            return predicate()
        # At the deadline, read only what already arrived: a stalled host can wake this loop late.
        at_deadline = now >= deadline
        if not at_deadline:
            for peer in open_peers:
                if peer.last_ping is None or now - peer.last_ping >= PING_INTERVAL_SECONDS:
                    peer.ping()
        readable, _, _ = select.select([peer.connection for peer in open_peers], [], [],
                                       0 if at_deadline else min(POLL_SECONDS, deadline - now))
        for peer in open_peers:
            if peer.connection in readable:
                peer.receive()
        if at_deadline:
            return predicate()
    return True


def write_genesis(directory):
    """Write one UTC-millisecond genesis into directory; return its path and the BIOS key pair it names."""
    directory.mkdir(parents=True, exist_ok=True)
    genesis_path = directory / "genesis.json"
    launcher = cluster_generator(["--nodes", "1", "--pnodes", "1", "--producers", "1",
                                  "--genesis", str(genesis_path)])
    launcher.define_network()
    launcher.bind_nodes()
    genesis = launcher.init_genesis()
    genesis["initial_timestamp"] = Utils.timestamp()[:-SUB_MILLISECOND_DIGITS]
    genesis_path.write_text(json.dumps(genesis, indent=2))
    return genesis_path, launcher.network.nodes["bios"].keys[0]


def producer_args(bios_key):
    """Options that produce and finalize alone as the genesis producer, as the launcher configures BIOS."""
    return ["--plugin", "sysio::producer_plugin", "--enable-stale-production", "--producer-name", PRODUCER_NAME,
            "--signature-provider", f"wire-{bios_key.pubkey},wire,wire,{bios_key.pubkey},KEY:{bios_key.privkey}",
            "--signature-provider",
            f"wire-bls-{bios_key.blspubkey},wire,wire_bls,{bios_key.blspubkey},KEY:{bios_key.blsprivkey}"]


def launch_nodeop(name, extra_args, genesis_path=None):
    """Start nodeop on loopback with the chain API and a P2P listener; return it with its P2P port.

    The node gets a genesis of its own unless given the genesis_path of a chain it shares.
    """
    node_index = next(NODE_INDEXES)
    node_dir = Path(Utils.DataPath) / name
    node_dir.mkdir(parents=True, exist_ok=True)
    if genesis_path is None:
        genesis_path, _ = write_genesis(node_dir)
    http_port = Utils.getPort(Utils.PortNodeHttp, node_index)
    p2p_port = Utils.getPort(Utils.PortP2P, node_index)
    command = [
        Utils.SysServerPath,
        "--config-dir", str(node_dir),
        "--data-dir", str(node_dir),
        "--genesis-json", str(genesis_path),
        "--plugin", "sysio::chain_api_plugin",
        "--plugin", "sysio::net_plugin",
        "--http-server-address", f"{LOOPBACK}:{http_port}",
        "--p2p-listen-endpoint", f"{LOOPBACK}:{p2p_port}",
        "--p2p-keepalive-interval-ms", str(KEEPALIVE_MS),
        "--connection-cleanup-period", str(CONNECTION_CLEANUP_PERIOD_SECONDS),
        *extra_args,
    ]
    return Node(LOOPBACK, http_port, node_index, node_dir, node_dir, command), p2p_port


def stop_node(node, args):
    """Stop the node unless the run asked to leave it running."""
    if node is not None and not args.leave_running:
        node.kill(signal.SIGTERM)


def node_log(node):
    """Return the node's stderr log."""
    return (node.data_dir / "stderr.txt").read_text()


def wait_for_log(node, text, count=1):
    """Wait for the node to log text at least count times."""
    deadline = time.monotonic() + RESPONSE_TIMEOUT_SECONDS
    while node_log(node).count(text) < count:
        assert time.monotonic() < deadline, \
            f"Node did not log '{text}' {count} time(s); see {node.data_dir / 'stderr.txt'}"
        time.sleep(POLL_SECONDS)


def wait_for_block(node, field, block_num, deadline):
    """Poll get_info until its field (a block number) reaches block_num; return False at the deadline."""
    while True:
        assert node.verifyAlive(silent=True), f"Node exited; see {node.data_dir / 'stderr.txt'}"
        info = node.getInfo(silentErrors=True)
        if info is not None and info[field] >= block_num:
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(POLL_SECONDS)


def wait_for_chain_id(node):
    """Return the node's chain id once its HTTP API answers."""
    deadline = time.monotonic() + STARTUP_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        assert node.verifyAlive(silent=True), f"Node exited during startup; see {node.data_dir / 'stderr.txt'}"
        info = node.getInfo(silentErrors=True)
        if info is not None:
            return bytes.fromhex(info["chain_id"])
        time.sleep(POLL_SECONDS)
    raise AssertionError(f"Node HTTP API did not start; see {node.data_dir / 'stderr.txt'}")


def connect(p2p_port, label, source_address=None):
    """Open an incoming session to the node's P2P listener, retrying until it listens.

    source_address is an optional (host, port) to bind the local end of the session to, with SO_REUSEADDR so a
    rerun can bind it again while an earlier session's TIME_WAIT lasts. Nagle is off, so each message reaches the
    node when it is sent.
    """
    deadline = time.monotonic() + STARTUP_TIMEOUT_SECONDS
    while True:
        connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            connection.settimeout(STARTUP_TIMEOUT_SECONDS)
            if source_address is not None:
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                connection.bind(source_address)
            connection.connect((LOOPBACK, p2p_port))
        except ConnectionRefusedError:
            connection.close()
            assert time.monotonic() < deadline, f"Node is not listening on P2P port {p2p_port}"
            time.sleep(POLL_SECONDS)
            continue
        except OSError:
            connection.close()
            raise
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return RawPeer(connection, label)


def accept_connection(listener, node, deadline):
    """Wait for the node to dial the listener; return the socket, or None if none arrived by the deadline."""
    listener.settimeout(POLL_SECONDS)
    while True:
        assert node.verifyAlive(silent=True), f"Node exited; see {node.data_dir / 'stderr.txt'}"
        try:
            connection, _ = listener.accept()
            return connection
        except socket.timeout:
            if time.monotonic() >= deadline:
                return None


def answer_handshake(peer):
    """Wait for the node's handshake on its outgoing session, then answer with ours."""
    assert peer.run_until(lambda: peer.handshake_generations, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        f"{peer.label}: node sent no handshake"
    peer.send_handshake(peer.chain_id)


def open_incoming(p2p_port, chain_id, label):
    """Dial the node, handshake, and wait for its handshake in reply."""
    peer = connect(p2p_port, label)
    peer.send_handshake(chain_id)
    assert peer.run_until(lambda: peer.handshake_generations, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
        f"{label}: node did not answer the handshake (go_away {peer.describe_go_away()})"
    return peer
