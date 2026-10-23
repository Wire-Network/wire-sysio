#!/usr/bin/env python3
"""Exercise the peer-authentication grace window on raw sockets against one nodeop.

Under ``allowed-connection specified`` a peer has one heartbeat timeout from the start of its
session to send a valid peer_auth_message. An honest dialer can sign only after the acceptor's
handshake reaches it, so a keepalive tick inside that round trip must not close it.

* ``late_auth``: an incoming peer that authenticates after the node's first keepalive tick stays
  connected.
* ``auth_timeout``: an outgoing peer that never authenticates, whether silent or handshaking, is closed
  after the grace window with a retryable reason and redialed; a key rejected on receipt is never
  redialed.
* ``rejected_auth``: an unauthorized key, a mismatched key and an unrecoverable signature are each
  rejected as soon as they arrive.
* ``redial_sync``: a node whose grace window closed its outgoing session redials, authenticates a
  producing peer, and syncs span after span from it.

The peer signs with a stdlib-only secp256k1 implementation, since the CI image has no Python crypto
package.
"""

import json
import secrets
import select
import shutil
import socket
import threading
import time
from pathlib import Path

from TestHarness import TestHelper, Utils
from TestHarness.TestHelper import AppArgs
from TestHarness.raw_p2p import (CONNECTION_CLEANUP_PERIOD_SECONDS, INITIAL_GENERATION, KEEPALIVE_SECONDS, LOOPBACK,
                                 POLL_SECONDS, RECV_BYTES, RESPONSE_TIMEOUT_SECONDS, STARTUP_TIMEOUT_SECONDS,
                                 GoAwayReason, RawPeer, accept_connection, answer_handshake, connect, launch_nodeop,
                                 node_log, open_incoming, producer_args, stop_node, wait_for_block,
                                 wait_for_chain_id, wait_for_log, write_genesis)


# An outgoing connection's heartbeat timeout, and so its authentication grace, is two keepalive intervals.
OUTGOING_HEARTBEAT_TIMEOUT_SECONDS = 2 * KEEPALIVE_SECONDS
# A retryable close is redialed on the first connection-cleanup pass a full period after it, so within two
# periods; a rejected key is shown not to be redialed by waiting three.
REDIAL_CLEANUP_PERIODS = 2
NO_REDIAL_CLEANUP_PERIODS = 3
POST_AUTH_TICKS = 3
SCHEDULING_ALLOWANCE_SECONDS = 2
# nodeop starts an outgoing session when its connect completes, slightly before accept() returns here.
ACCEPT_ALLOWANCE_SECONDS = 1
# The producing node makes this many sync spans irreversible before the sync node starts.
SYNC_FETCH_SPAN = 5
SYNC_SPANS = 3
PRODUCTION_TIMEOUT_SECONDS = 60
SYNC_TIMEOUT_SECONDS = 60

AUTHENTICATED_LOG = "Peer authenticated successfully"
GRACE_EXPIRED_LOG = "Peer did not authenticate in time"
UNAUTHORIZED_KEY_LOG = "Unauthorized peer key"
KEY_MISMATCH_LOG = "Auth key mismatch"
INVALID_SIGNATURE_LOG = "Invalid auth signature"
SYNC_REQUEST_LOG = "requesting range"
SYNC_PEER_DROPPED_LOG = "Unable to continue syncing at this time"

# K1 key pairs (clio create key --k1 --to-console), the same ones p2p_peer_auth_test.py uses.
NODE_KEY_TEXT = ("PUB_K1_7vWwkYDbJottP4hiT3mtbjYZ69nG5ZHaA9WKdJirKapX46pTir",
                 "PVT_K1_2MsgQPmGnn1h6ymMotKZ4FAQZSawx8KbiLLFqNUkKJ9r8Yq8z8")
TRUSTED_KEY_TEXT = ("PUB_K1_83HCYssKBuwnT8KXTqKmz4nA9HoRHaKd6kJKYuWCXjg7T8eE9h",
                    "PVT_K1_2bjvK9CXFtJn548osj9jKS8gVyZNHgUh5uHSttLMcpCkXzpqRt")
UNTRUSTED_KEY_TEXT = ("PUB_K1_5hxPfNud5RfR1sLVxSHnK4qPWkLh2wRZUJKsCa8rzCqkPHvgZR",
                      "PVT_K1_Cj7pBcRkPie6xAd1Y6gFumu289XfzhVLas4ckXBX5GLgqZc7w")

K1_PUBLIC_PREFIX = "PUB_K1_"
K1_PRIVATE_PREFIX = "PVT_K1_"
BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
KEY_CHECKSUM_BYTES = 4
SCALAR_BYTES = 32
COMPRESSED_PUBLIC_KEY_BYTES = 33
COMPRESSED_EVEN_Y_PREFIX = 2
# fc compact signature header for a compressed public key: 27 + 4 + recovery id.
COMPACT_SIGNATURE_HEADER = 27 + 4
# Recovery id bits: the parity of the nonce point's y, and whether its x overflowed the group order.
RECOVERY_ID_Y_ODD = 1
RECOVERY_ID_X_OVERFLOW = 2
# A header of 0 is outside fc's accepted 27..34 range, so recovering the signer throws.
UNRECOVERABLE_SIGNATURE = bytes(1 + 2 * SCALAR_BYTES)

SECP256K1_P = 2**256 - 2**32 - 977
SECP256K1_N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
SECP256K1_G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
               0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def decode_key_text(text, prefix, size):
    """Return the key bytes of an fc key string, dropping its base58 checksum."""
    assert text.startswith(prefix), f"Expected a {prefix} key, got {text}"
    value = 0
    for character in text[len(prefix):]:
        value = value * len(BASE58_ALPHABET) + BASE58_ALPHABET.index(character)
    return value.to_bytes(size + KEY_CHECKSUM_BYTES, "big")[:size]


def point_add(left, right):
    """Add two affine secp256k1 points; None is the point at infinity."""
    if left is None:
        return right
    if right is None:
        return left
    if left[0] == right[0] and (left[1] + right[1]) % SECP256K1_P == 0:
        return None
    if left == right:
        slope = 3 * left[0] * left[0] * pow(2 * left[1], -1, SECP256K1_P) % SECP256K1_P
    else:
        slope = (right[1] - left[1]) * pow(right[0] - left[0], -1, SECP256K1_P) % SECP256K1_P
    x = (slope * slope - left[0] - right[0]) % SECP256K1_P
    return x, (slope * (left[0] - x) - left[1]) % SECP256K1_P


def point_multiply(scalar, point):
    """Multiply an affine secp256k1 point by a scalar, double and add."""
    result = None
    while scalar:
        if scalar & 1:
            result = point_add(result, point)
        point = point_add(point, point)
        scalar >>= 1
    return result


class K1Key:
    """K1 key pair from its fc key strings, producing signatures fc recovers."""

    def __init__(self, key_text):
        """Derive the compressed public key and check it matches the PUB_K1 string."""
        self.public_text, private_text = key_text
        self.secret = int.from_bytes(decode_key_text(private_text, K1_PRIVATE_PREFIX, SCALAR_BYTES), "big")
        x, y = point_multiply(self.secret, SECP256K1_G)
        self.public_key = bytes([COMPRESSED_EVEN_Y_PREFIX | (y & 1)]) + x.to_bytes(SCALAR_BYTES, "big")
        expected = decode_key_text(self.public_text, K1_PUBLIC_PREFIX, COMPRESSED_PUBLIC_KEY_BYTES)
        assert self.public_key == expected, f"{self.public_text} does not belong to its private key"

    def sign(self, digest):
        """Return an fc compact recoverable signature over a 32-byte digest."""
        message = int.from_bytes(digest, "big")
        while True:
            nonce = secrets.randbelow(SECP256K1_N - 1) + 1
            nonce_x, nonce_y = point_multiply(nonce, SECP256K1_G)
            r = nonce_x % SECP256K1_N
            s = pow(nonce, -1, SECP256K1_N) * (message + r * self.secret) % SECP256K1_N
            if r and s:
                recovery_id = ((RECOVERY_ID_Y_ODD if nonce_y & 1 else 0) |
                               (RECOVERY_ID_X_OVERFLOW if nonce_x >= SECP256K1_N else 0))
                return (bytes([COMPACT_SIGNATURE_HEADER + recovery_id]) +
                        r.to_bytes(SCALAR_BYTES, "big") + s.to_bytes(SCALAR_BYTES, "big"))


TRUSTED_KEY = K1Key(TRUSTED_KEY_TEXT)
UNTRUSTED_KEY = K1Key(UNTRUSTED_KEY_TEXT)


class Relay(threading.Thread):
    """Copy bytes both ways between two connected sockets until either closes or stop() is called."""

    def __init__(self, first, second):
        """Pair the two sockets; start() begins forwarding."""
        super().__init__(daemon=True)
        self.forward_to = {first: second, second: first}
        self.stopped = threading.Event()

    def run(self):
        """Forward whatever either side sends, then close both sockets."""
        try:
            while not self.stopped.is_set():
                readable, _, _ = select.select(list(self.forward_to), [], [], POLL_SECONDS)
                for connection in readable:
                    data = connection.recv(RECV_BYTES)
                    if not data:
                        return
                    self.forward_to[connection].sendall(data)
        except OSError:
            return
        finally:
            for connection in self.forward_to:
                connection.close()

    def stop(self):
        """Stop forwarding and wait for both sockets to close."""
        self.stopped.set()
        self.join()



def launch_node(name, extra_args, genesis_path=None, identity=NODE_KEY_TEXT, trusted=TRUSTED_KEY_TEXT):
    """Start nodeop requiring peer authentication; return it with its P2P port.

    The node authenticates as `identity` and accepts `trusted`. It gets a genesis of its own unless given
    the genesis_path of a chain it shares.
    """
    auth_args = ["--allowed-connection", "specified",
                 "--peer-key", json.dumps(trusted[0]),
                 "--peer-private-key", json.dumps(list(identity))]
    return launch_nodeop(name, [*auth_args, *extra_args], genesis_path)


def run_late_auth(case_index, args):
    """A dialer that authenticates after the acceptor's first keepalive tick must stay connected."""
    node, p2p_port = launch_node("late_auth", [])
    peer = None
    try:
        chain_id = wait_for_chain_id(node)
        peer = open_incoming(p2p_port, chain_id, "late dialer")
        # Hold the peer_auth_message until the node's next keepalive tick has checked the session: a
        # dialer whose handshake round trip spans a tick. The tick re-sends the node's handshake (or
        # pings) when it leaves the session open.
        tick_deadline = time.monotonic() + KEEPALIVE_SECONDS + SCHEDULING_ALLOWANCE_SECONDS
        assert peer.run_until(lambda: (peer.handshake_generations[-1] > INITIAL_GENERATION or peer.node_pings
                                       or peer.go_away is not None), tick_deadline), \
            "Did not observe the node's keepalive tick"
        assert peer.go_away is None and not peer.closed, (
            f"Node closed the dialer at its first keepalive tick, before it could authenticate "
            f"(go_away {peer.describe_go_away()})")

        peer.send_peer_auth(TRUSTED_KEY.public_key, TRUSTED_KEY.sign(peer.auth_digest()))
        wait_for_log(node, AUTHENTICATED_LOG)
        peer.run_until(lambda: False, time.monotonic() + POST_AUTH_TICKS * KEEPALIVE_SECONDS)
        assert peer.go_away is None and not peer.closed, \
            f"Node closed the authenticated dialer (go_away {peer.describe_go_away()})"
        assert GRACE_EXPIRED_LOG not in node_log(node), "Node logged an expired grace window"
        Utils.Print("PASS: late_auth, a dialer authenticating after a keepalive tick stays connected")
    finally:
        if peer is not None:
            peer.close()
        stop_node(node, args)


def run_auth_timeout(case_index, args):
    """Unauthenticated peers are closed after the grace window and redialed, with or without frames.

    A key rejected on receipt is not redialed.
    """
    peer_port = Utils.getPort(Utils.PortAlternateP2P, case_index)
    node = None
    peers = []
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((LOOPBACK, peer_port))
        listener.listen()
        try:
            node, _ = launch_node("auth_timeout", ["--p2p-peer-address", f"{LOOPBACK}:{peer_port}"])
            connection = accept_connection(listener, node, time.monotonic() + STARTUP_TIMEOUT_SECONDS)
            assert connection is not None, f"Node did not dial its peer; see {node.data_dir / 'stderr.txt'}"
            # A silent peer never sends a frame; a handshaking peer handshakes and pings but never authenticates.
            unauthenticated_peers = (("silent acceptor", False), ("handshaking acceptor", True))
            for closes, (label, handshakes) in enumerate(unauthenticated_peers, start=1):
                accepted = time.monotonic()
                peer = RawPeer(connection, label)
                peers.append(peer)
                if handshakes:
                    answer_handshake(peer)
                close_deadline = (accepted + OUTGOING_HEARTBEAT_TIMEOUT_SECONDS + KEEPALIVE_SECONDS +
                                  SCHEDULING_ALLOWANCE_SECONDS)
                assert peer.run_until(lambda: peer.go_away is not None, close_deadline, keepalive=handshakes), \
                    f"Node sent the {label} no go_away by the end of its grace window (socket closed: {peer.closed})"
                elapsed = peer.go_away_time - accepted
                assert peer.go_away is GoAwayReason.benign_other, \
                    f"{label}: grace-window close sent go_away {peer.describe_go_away()}, which is never redialed"
                assert elapsed >= OUTGOING_HEARTBEAT_TIMEOUT_SECONDS - ACCEPT_ALLOWANCE_SECONDS, \
                    f"Node closed the {label} {elapsed:.2f}s into a {OUTGOING_HEARTBEAT_TIMEOUT_SECONDS}s grace window"
                wait_for_log(node, GRACE_EXPIRED_LOG, closes)
                redial_deadline = (peer.go_away_time + REDIAL_CLEANUP_PERIODS * CONNECTION_CLEANUP_PERIOD_SECONDS +
                                   SCHEDULING_ALLOWANCE_SECONDS)
                connection = accept_connection(listener, node, redial_deadline)
                assert connection is not None, f"Node did not redial after closing the {label}"

            peers.append(RawPeer(connection, "rejected acceptor"))
            rejected = peers[-1]
            answer_handshake(rejected)
            rejected.send_peer_auth(UNTRUSTED_KEY.public_key, UNTRUSTED_KEY.sign(rejected.auth_digest()))
            assert rejected.run_until(lambda: rejected.go_away is not None,
                                      time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
                "Node did not reject an unauthorized key"
            assert rejected.go_away is GoAwayReason.authentication, \
                f"Unauthorized key drew go_away {rejected.describe_go_away()}"
            wait_for_log(node, UNAUTHORIZED_KEY_LOG)
            no_redial_deadline = (time.monotonic() + NO_REDIAL_CLEANUP_PERIODS * CONNECTION_CLEANUP_PERIOD_SECONDS +
                                  SCHEDULING_ALLOWANCE_SECONDS)
            assert accept_connection(listener, node, no_redial_deadline) is None, \
                "Node redialed a peer whose key it rejected"
            Utils.Print("PASS: auth_timeout, a grace-window close is redialed and a rejected key is not")
        finally:
            for peer in peers:
                peer.close()
            stop_node(node, args)


REJECTIONS = (
    ("unauthorized key", UNAUTHORIZED_KEY_LOG,
     lambda digest: (UNTRUSTED_KEY.public_key, UNTRUSTED_KEY.sign(digest))),
    ("key mismatch", KEY_MISMATCH_LOG,
     lambda digest: (TRUSTED_KEY.public_key, UNTRUSTED_KEY.sign(digest))),
    ("invalid signature", INVALID_SIGNATURE_LOG,
     lambda digest: (TRUSTED_KEY.public_key, UNRECOVERABLE_SIGNATURE)),
)


def run_rejected_auth(case_index, args):
    """Credentials that fail verification draw go_away(authentication) on receipt."""
    node, p2p_port = launch_node("rejected_auth", [])
    try:
        chain_id = wait_for_chain_id(node)
        for name, log_text, credentials in REJECTIONS:
            peer = open_incoming(p2p_port, chain_id, name)
            try:
                peer.send_peer_auth(*credentials(peer.auth_digest()))
                assert peer.run_until(lambda: peer.go_away is not None, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
                    f"{name}: node did not reject the peer_auth_message"
                assert peer.go_away is GoAwayReason.authentication, \
                    f"{name}: node sent go_away {peer.describe_go_away()}"
                wait_for_log(node, log_text)
                # The node admits one session per host, so the next one waits for this close.
                assert peer.run_until(lambda: peer.closed, time.monotonic() + RESPONSE_TIMEOUT_SECONDS), \
                    f"{name}: node did not close the session"
            finally:
                peer.close()
        Utils.Print("PASS: rejected_auth, bad credentials are rejected on receipt")
    finally:
        stop_node(node, args)


def run_redial_sync(case_index, args):
    """A session redialed after a grace-window close authenticates and syncs every span from its peer.

    The sync node's first session goes to a test acceptor that never authenticates, so its own grace window
    closes the session with benign_other. Its redial reaches a producing node through a relay. Once that
    session authenticates, the sync node must keep requesting spans from it, not drop its only peer from sync.
    """
    peer_port = Utils.getPort(Utils.PortAlternateP2P, case_index)
    genesis_path, bios_key = write_genesis(Path(Utils.DataPath) / "redial_sync")
    source = sync_node = relay = None
    peers = []
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((LOOPBACK, peer_port))
        listener.listen()
        try:
            source, source_p2p_port = launch_node("redial_sync/source", producer_args(bios_key), genesis_path)
            irreversible_target = SYNC_SPANS * SYNC_FETCH_SPAN
            assert wait_for_block(source, "last_irreversible_block_num", irreversible_target,
                                  time.monotonic() + PRODUCTION_TIMEOUT_SECONDS), \
                f"Producing node did not reach irreversible block {irreversible_target}"
            sync_node, _ = launch_node("redial_sync/sync",
                                       ["--p2p-peer-address", f"{LOOPBACK}:{peer_port}",
                                        "--sync-fetch-span", str(SYNC_FETCH_SPAN)],
                                       genesis_path, identity=TRUSTED_KEY_TEXT, trusted=NODE_KEY_TEXT)

            connection = accept_connection(listener, sync_node, time.monotonic() + STARTUP_TIMEOUT_SECONDS)
            assert connection is not None, f"Sync node did not dial; see {sync_node.data_dir / 'stderr.txt'}"
            accepted = time.monotonic()
            peers.append(RawPeer(connection, "unauthenticated acceptor"))
            first = peers[-1]
            answer_handshake(first)
            close_deadline = (accepted + OUTGOING_HEARTBEAT_TIMEOUT_SECONDS + KEEPALIVE_SECONDS +
                              SCHEDULING_ALLOWANCE_SECONDS)
            assert first.run_until(lambda: first.go_away is not None, close_deadline), \
                "Sync node kept a peer that never authenticated past its grace window"
            assert first.go_away is GoAwayReason.benign_other, \
                f"Grace-window close sent go_away {first.describe_go_away()}"

            redial_deadline = (first.go_away_time + REDIAL_CLEANUP_PERIODS * CONNECTION_CLEANUP_PERIOD_SECONDS +
                               SCHEDULING_ALLOWANCE_SECONDS)
            connection = accept_connection(listener, sync_node, redial_deadline)
            assert connection is not None, "Sync node did not redial after its grace window closed the session"
            upstream = connect(source_p2p_port, "relay to the producing node")
            peers.append(upstream)
            relay = Relay(connection, upstream.connection)
            relay.start()
            source_info = source.getInfo(silentErrors=True)
            assert source_info is not None, f"Producing node stopped answering; see {source.data_dir / 'stderr.txt'}"
            sync_target = source_info["last_irreversible_block_num"]
            synced = wait_for_block(sync_node, "head_block_num", sync_target, time.monotonic() + SYNC_TIMEOUT_SECONDS)
            log = node_log(sync_node)
            assert SYNC_PEER_DROPPED_LOG not in log, \
                "Sync node dropped its only, authenticated peer from sync after a span"
            assert synced, f"Sync node did not sync to block {sync_target}"
            assert log.count(SYNC_REQUEST_LOG) > 1, "Sync node synced without requesting more than one span"
            Utils.Print(f"PASS: redial_sync, synced to block {sync_target} in "
                        f"{log.count(SYNC_REQUEST_LOG)} spans after a grace-window redial")
        finally:
            if relay is not None:
                relay.stop()
            for peer in peers:
                peer.close()
            stop_node(sync_node, args)
            stop_node(source, args)


CASES = {
    "late_auth": run_late_auth,
    "auth_timeout": run_auth_timeout,
    "rejected_auth": run_rejected_auth,
    "redial_sync": run_redial_sync,
}


def main():
    """Run the selected cases and keep node logs on any failure."""
    application_args = AppArgs()
    application_args.add("--case", str, "Authentication case to exercise", default="all",
                         choices=["all", *CASES])
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
        Utils.Print(f"Peer auth grace test {'passed' if successful else 'failed'}; logs: {Utils.DataPath}")


if __name__ == "__main__":
    main()
