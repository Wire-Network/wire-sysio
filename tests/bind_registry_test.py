#!/usr/bin/env python3
"""Self-tests for the bind-registry client and test wrapper (TestHarness/bind_registry.py).

Registry behaviour is tested against private registries and temp directories
(WIRE_BIND_REGISTRY_PATH, TMPDIR). Leases there never probe real sockets: the
bind check is faked, in-process and in every child this suite starts, because a
probe of a port another test on the host leased could make that test's own
bind fail. The few tests that bind real sockets use only HOST_PORTS, a block
this module leases from the host registry for its lifetime. CTest can therefore
run this suite in parallel with any other test.
"""
import contextlib
import ctypes
import errno
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import unittest.mock
from concurrent.futures import ThreadPoolExecutor

import bind_registry
from bind_registry import (BindRegistryError, LEASE_PORT_COUNT, PORTS_ENV, REGISTRATION_SUFFIX,
                           REGISTRY_PATH_ENV, TEMPORARY_SUFFIX, WRAPPER_FAILURE_STATUS)

WRAPPER = str(Path(__file__).with_name("bind_registry.py"))
TESTS_DIR = str(Path(__file__).resolve().parent)
SMALL_LEASE = 4
# Generous: a lease bind-checks hundreds of sockets, which is slow on a loaded WSL2 host.
CHILD_TIMEOUT_SECONDS = 600
SHORT_LOCK_WAIT_SECONDS = 0.5
LOCK_RELEASE_DELAY_SECONDS = 0.3
STALE_LOCK_AGE_SECONDS = bind_registry.LOCK_STALE_SECONDS * 2
SHORT_GROUP_SECONDS = 0.5
GHOST_LEASE_DEADLINE_SECONDS = 10
# Longer than every drain window the wrapper tests pass, so a process only goes away if the wrapper removes it.
# tearDown kills whatever is left in the sessions the tests' wrapped commands ran in (and nothing else).
OUTLIVES_EVERY_WINDOW_SECONDS = 3600
SIGKILL_WINDOW_SECONDS = 60
HAS_PROCFS = os.path.isdir("/proc/self")
procfs_only = unittest.skipUnless(HAS_PROCFS, "needs /proc (argv of other processes)")

# Children that lease from a private registry fake the bind check, as this process does (see the module docstring).
NO_PROBE_PRELUDE = (f"import sys; sys.path.insert(0, {TESTS_DIR!r}); import bind_registry; "
                    "bind_registry.port_binds = lambda port: True\n")
WRAPPER_COMMAND = [sys.executable, "-c", NO_PROBE_PRELUDE + "sys.exit(bind_registry.main(sys.argv[1:]))"]

# A block leased from the host registry; the only ports this suite binds real sockets on.
HOST_PORTS = []
HOST_REGISTRATION = None


def setUpModule():
    global HOST_REGISTRATION
    ports, HOST_REGISTRATION = bind_registry.lease(SMALL_LEASE)
    HOST_PORTS.extend(ports)


def tearDownModule():
    if HOST_REGISTRATION is not None:
        bind_registry.release(HOST_REGISTRATION)


PR_SET_CHILD_SUBREAPER = 36  # linux/prctl.h
# The no-procfs code paths run on Linux too, with /proc/self hidden from the client (see hidden_procfs).
linux_only = unittest.skipUnless(sys.platform.startswith("linux"), "Linux-only stand-in for the no-procfs path")


@contextlib.contextmanager
def hidden_procfs():
    """Make the client see no procfs, so it takes its macOS paths (ps and getsid)."""
    real_isdir = os.path.isdir
    with unittest.mock.patch.object(bind_registry.os.path, "isdir",
                                    lambda path: False if path == "/proc/self" else real_isdir(path)):
        yield


@contextlib.contextmanager
def reaping_orphan_from(pid_file):
    """Make this process the subreaper of orphans (Linux) and reap the one whose pid ``pid_file`` names.

    Elsewhere (macOS) launchd reaps orphans, so only the pid is waited for, if it is ours.
    """
    libc = ctypes.CDLL(None, use_errno=True) if sys.platform.startswith("linux") else None
    subreaper = libc is not None and libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0
    stop = threading.Event()

    def reap():
        while not stop.is_set():
            try:
                pid = int(pid_file.read_text())
                if os.waitpid(pid, os.WNOHANG)[0] == pid:
                    return
            except (FileNotFoundError, ValueError, ChildProcessError):
                pass  # Not written yet, or not (yet) reparented to us.
            stop.wait(bind_registry.GROUP_POLL_SECONDS)

    reaper = threading.Thread(target=reap, daemon=True)
    reaper.start()
    try:
        yield
    finally:
        stop.set()
        reaper.join()
        if subreaper:
            libc.prctl(PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0)


def fake_binds(port):
    """A bind check that touches no socket: every port binds."""
    return True


def spawn_sleeper():
    """Start a live process whose argv[0] basename is "sleep"."""
    return subprocess.Popen(["sleep", "600"])


def dead_pid():
    """Return the pid of a process that has exited and been reaped."""
    child = subprocess.Popen(["true"])
    child.wait()
    return child.pid


class RegistryCase(unittest.TestCase):
    """Base: a private temp dir and registry, and a clean per-process write record."""

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="bind-registry-test-")
        self.root = Path(self.scratch.name)
        self.registry = self.root / "registry"
        self.environ = {"TMPDIR": str(self.root), REGISTRY_PATH_ENV: str(self.registry)}
        self.written_before = set(bind_registry._written_registrations)
        bind_registry._written_registrations.clear()
        self.children = []
        no_probe = unittest.mock.patch.object(bind_registry, "port_binds", fake_binds)
        no_probe.start()
        self.addCleanup(no_probe.stop)
        # Sessions of commands this test ran under the wrapper, recorded so tearDown can clean up after a regression.
        self.sessions = []
        original_drain = bind_registry.drain_session

        def recording_drain(sid, *args, **kwargs):
            self.sessions.append(sid)
            return original_drain(sid, *args, **kwargs)

        recording = unittest.mock.patch.object(bind_registry, "drain_session", recording_drain)
        recording.start()
        self.addCleanup(recording.stop)

    def tearDown(self):
        for sid in self.sessions:  # Only the sessions this test's wrapped commands ran in.
            bind_registry._signal_session(sid, signal.SIGKILL)
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=CHILD_TIMEOUT_SECONDS)
        bind_registry._written_registrations.clear()
        bind_registry._written_registrations.update(self.written_before)
        self.scratch.cleanup()

    def child_env(self, **extra):
        env = {key: value for key, value in os.environ.items() if key != PORTS_ENV}
        env.update(self.environ)
        env.update(extra)
        return env

    def write(self, name, content):
        self.registry.mkdir(parents=True, exist_ok=True)
        path = self.registry / name
        path.write_text(content if isinstance(content, str) else json.dumps(content))
        return path

    def exclusions(self, own_pid=None):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            excluded = bind_registry.read_exclusions(str(self.registry), own_pid or os.getpid())
        return excluded, stderr.getvalue()


class TempDirectory(unittest.TestCase):
    def test_follows_node_order_not_tempfile(self):
        # tempfile.gettempdir() would pick TEMP here; Node picks TMP.
        self.assertEqual(bind_registry.node_tmpdir({"TMP": "/from-tmp", "TEMP": "/from-temp"}), "/from-tmp")
        self.assertEqual(bind_registry.node_tmpdir({"TMPDIR": "", "TMP": "", "TEMP": "/from-temp"}), "/from-temp")
        self.assertEqual(bind_registry.node_tmpdir({"TMPDIR": "/a/", "TMP": "/b"}), "/a")
        self.assertEqual(bind_registry.node_tmpdir({"TMPDIR": "/"}), "/")
        self.assertEqual(bind_registry.node_tmpdir({}), "/tmp")

    def test_registry_dir_override_and_empty_override(self):
        with tempfile.TemporaryDirectory() as scratch:
            private = os.path.join(scratch, "private")
            self.assertEqual(bind_registry.registry_dir({REGISTRY_PATH_ENV: private, "TMPDIR": scratch}), private)
            self.assertEqual(bind_registry.registry_dir({REGISTRY_PATH_ENV: "", "TMPDIR": scratch}),
                             os.path.join(scratch, bind_registry.REGISTRY_DIRNAME))
            self.assertTrue(os.path.isdir(private))

    def test_lock_path_matches_proper_lockfile_key(self):
        import hashlib
        registry = "/some/registry"
        key = hashlib.sha256(registry.encode()).hexdigest()[:16]
        self.assertEqual(bind_registry.lock_path(registry, {"TMPDIR": "/t"}),
                         f"/t/wire-cluster-ports-{key}.lock.lock")


class Lock(RegistryCase):
    def lock(self, wait=SHORT_LOCK_WAIT_SECONDS):
        return bind_registry.RegistryLock(str(self.root / "test.lock.lock"), wait_seconds=wait)

    def test_contention_fails_loudly_after_bounded_wait(self):
        held = self.lock()
        with held:
            started = time.monotonic()
            with self.assertRaisesRegex(BindRegistryError, "still held"):
                with self.lock():
                    pass
            self.assertGreaterEqual(time.monotonic() - started, SHORT_LOCK_WAIT_SECONDS)
        self.assertFalse(os.path.exists(held.path))

    def test_waits_for_holder_to_release(self):
        holder = self.lock()
        holder.__enter__()
        releaser = threading.Timer(LOCK_RELEASE_DELAY_SECONDS, holder.__exit__, (None, None, None))
        releaser.start()
        with self.lock(wait=CHILD_TIMEOUT_SECONDS) as waiter:
            self.assertTrue(os.path.isdir(waiter.path))
        releaser.join()

    def test_breaks_stale_lock(self):
        stale = self.lock()
        os.mkdir(stale.path)
        old = time.time() - STALE_LOCK_AGE_SECONDS
        os.utime(stale.path, (old, old))
        with self.lock():
            self.assertGreater(os.stat(stale.path).st_mtime, old)
        self.assertFalse(os.path.exists(stale.path))


class LockIdentity(RegistryCase):
    def test_release_leaves_a_lock_another_process_took_over(self):
        lock = bind_registry.RegistryLock(str(self.root / "takeover.lock.lock"))
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with lock:
                os.rmdir(lock.path)  # Broken as stale by another process, which now holds its own.
                os.mkdir(lock.path)
                old = time.time() - 1
                os.utime(lock.path, (old, old))
        self.assertTrue(os.path.isdir(lock.path))
        self.assertIn("taken over", stderr.getvalue())


class Reading(RegistryCase):
    def test_live_registration_is_excluded(self):
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        self.write(f"{sleeper.pid}{REGISTRATION_SUFFIX}", {"executable": "sleep", "ports": [20001, 20002]})
        excluded, _ = self.exclusions()
        self.assertEqual(excluded, {20001, 20002})

    def test_dead_owner_is_removed(self):
        path = self.write(f"{dead_pid()}{REGISTRATION_SUFFIX}", {"executable": "python3", "ports": [20003]})
        excluded, _ = self.exclusions()
        self.assertEqual(excluded, set())
        self.assertFalse(path.exists())

    @procfs_only
    def test_recycled_pid_is_removed(self):
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        path = self.write(f"{sleeper.pid}{REGISTRATION_SUFFIX}", {"executable": "python3", "ports": [20004]})
        excluded, log = self.exclusions()
        self.assertEqual(excluded, set())
        self.assertFalse(path.exists())
        self.assertIn("now runs 'sleep'", log)

    def test_malformed_registrations_are_removed_and_logged(self):
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        pid = sleeper.pid
        bad = [self.write(f"{pid}{REGISTRATION_SUFFIX}", "{not json")]
        for name in ("0", "007", "123abc", "abc", "99999999999999999999999", "2147483648"):
            bad.append(self.write(f"{name}{REGISTRATION_SUFFIX}", {"executable": "sleep", "ports": [1]}))
        excluded, log = self.exclusions()
        self.assertEqual(excluded, set())
        for path in bad:
            self.assertFalse(path.exists(), path)
            self.assertIn(str(path), log)

    def test_wrong_member_types_are_removed(self):
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        for document in ({"executable": 1, "ports": []}, {"executable": "sleep", "ports": ["1"]},
                         {"executable": "sleep", "ports": [True]}, {"executable": "sleep"}, [],
                         {"executable": "sleep", "ports": [0]}, {"executable": "sleep", "ports": [65536]},
                         {"executable": "sleep", "ports": [20000, 20000]}):
            path = self.write(f"{sleeper.pid}{REGISTRATION_SUFFIX}", document)
            excluded, log = self.exclusions()
            self.assertEqual(excluded, set())
            self.assertFalse(path.exists(), document)
            self.assertIn("not a registration", log)

    def test_extra_members_are_ignored(self):
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        self.write(f"{sleeper.pid}{REGISTRATION_SUFFIX}", {"executable": "sleep", "ports": [20005], "x": {}})
        self.assertEqual(self.exclusions()[0], {20005})

    def test_dead_temporary_files_are_removed(self):
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        dead = self.write(f"{dead_pid()}{REGISTRATION_SUFFIX}{TEMPORARY_SUFFIX}", "{")
        live = self.write(f"{sleeper.pid}{REGISTRATION_SUFFIX}{TEMPORARY_SUFFIX}", "{")
        self.exclusions()
        self.assertFalse(dead.exists())
        self.assertTrue(live.exists())

    @procfs_only
    def test_zombie_owner_is_kept(self):
        # A zombie's /proc/<pid>/cmdline reads back empty: the owner cannot be checked, so keep it (as Node does).
        zombie = subprocess.Popen(["true"])
        self.children.append(zombie)
        deadline = time.monotonic() + CHILD_TIMEOUT_SECONDS
        while bind_registry.process_basename(zombie.pid) != "":
            self.assertLess(time.monotonic(), deadline)
            time.sleep(bind_registry.GROUP_POLL_SECONDS)
        path = self.write(f"{zombie.pid}{REGISTRATION_SUFFIX}", {"executable": "true", "ports": [20007]})
        self.assertEqual(self.exclusions()[0], {20007})
        self.assertTrue(path.exists())

    def test_undeletable_stale_file_is_reported_not_fatal(self):
        path = self.write(f"{dead_pid()}{REGISTRATION_SUFFIX}", {"executable": "python3", "ports": [20008]})
        with unittest.mock.patch.object(bind_registry.os, "unlink", side_effect=PermissionError(errno.EPERM, "denied")):
            excluded, log = self.exclusions()
        self.assertEqual(excluded, set())
        self.assertIn(f"cannot remove {path}", log)

    def test_own_registration_is_skipped_until_written_then_excluded(self):
        own = self.write(f"{os.getpid()}{REGISTRATION_SUFFIX}", {"executable": "anything", "ports": [20006]})
        excluded, _ = self.exclusions()
        self.assertEqual(excluded, set())  # Not written by this process: a recycled pid's leftover.
        self.assertTrue(own.exists())
        bind_registry._written_registrations.add(str(own))
        excluded, _ = self.exclusions()
        self.assertEqual(excluded, {20006})
        self.assertTrue(own.exists())


class Leasing(RegistryCase):
    def lease(self, count=SMALL_LEASE):
        return bind_registry.lease(count, environ=self.environ)

    def registered(self, path):
        return json.loads(Path(path).read_text())

    def test_lease_is_consecutive_registered_and_outside_reserved_ranges(self):
        ports, path = self.lease(LEASE_PORT_COUNT)
        self.assertEqual(ports, list(range(ports[0], ports[0] + LEASE_PORT_COUNT)))
        first, last = bind_registry.ephemeral_port_range()
        low, high = bind_registry.RESERVED_PORT_BAND
        self.assertFalse(any(low <= port <= high or first <= port <= last for port in ports))
        self.assertEqual(self.registered(path), {"executable": bind_registry.own_executable(), "ports": ports})
        self.assertEqual(Path(path).name, f"{os.getpid()}{REGISTRATION_SUFFIX}")
        self.assertFalse(any(name.endswith(TEMPORARY_SUFFIX) for name in os.listdir(self.registry)))
        self.assertFalse(os.path.exists(bind_registry.lock_path(str(self.registry), self.environ)))

    def test_lease_avoids_live_registrations(self):
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        first, _ = self.lease()
        bind_registry._written_registrations.clear()
        os.rename(self.registry / f"{os.getpid()}{REGISTRATION_SUFFIX}",
                  self.registry / f"{sleeper.pid}{REGISTRATION_SUFFIX}")
        document = self.registered(self.registry / f"{sleeper.pid}{REGISTRATION_SUFFIX}")
        document["executable"] = "sleep"
        self.write(f"{sleeper.pid}{REGISTRATION_SUFFIX}", document)
        second, _ = self.lease()
        self.assertFalse(set(first) & set(second))

    def test_second_lease_in_one_process_never_reissues_its_own_ports(self):
        first, path = self.lease()
        second, same_path = self.lease()
        self.assertEqual(path, same_path)
        self.assertFalse(set(first) & set(second))
        self.assertEqual(sorted(self.registered(path)["ports"]), sorted(first + second))

    def test_first_write_replaces_a_recycled_pids_file(self):
        stale_ports = list(range(20010, 20010 + SMALL_LEASE))
        self.write(f"{os.getpid()}{REGISTRATION_SUFFIX}", {"executable": "python3", "ports": stale_ports})
        ports, path = self.lease()
        self.assertEqual(self.registered(path)["ports"], ports)

    def test_every_probed_port_is_registered_to_this_process_first(self):
        # A probe socket holds its port for a moment; if the port belonged to another process's lease,
        # that owner's bind could fail EADDRINUSE (TCP and UDP, with or without SO_REUSEADDR). So a lease
        # only ever probes ports it has already claimed in its own registration.
        own = self.registry / f"{os.getpid()}{REGISTRATION_SUFFIX}"
        probed, unclaimed = [], []

        def checking_binds(port):
            probed.append(port)
            if not (own.exists() and port in json.loads(own.read_text())["ports"]):
                unclaimed.append(port)
            return True

        with unittest.mock.patch.object(bind_registry, "port_binds", checking_binds):
            ports, _ = self.lease()
        self.assertEqual(sorted(set(probed)), ports)
        self.assertEqual(unclaimed, [])
        self.assertEqual(self.registered(own)["ports"], ports)

    def test_block_with_one_taken_port_is_released_and_another_claimed(self):
        own = self.registry / f"{os.getpid()}{REGISTRATION_SUFFIX}"
        first = []

        def middle_of_first_block_taken(port):
            if not first:  # The first probe: remember the block this lease claimed for it.
                first.extend(json.loads(own.read_text())["ports"])
            return port != first[SMALL_LEASE // 2]  # Not the block's first port: the whole block must be checked.

        with unittest.mock.patch.object(bind_registry, "port_binds", middle_of_first_block_taken):
            ports, path = self.lease()
        self.assertEqual(len(set(first)), SMALL_LEASE)
        self.assertFalse(set(first) & set(ports))
        self.assertEqual(self.registered(path)["ports"], ports)

    def test_prefers_blocks_above_the_reserved_band(self):
        for _ in range(20):
            start = bind_registry.ordered_blocks(SMALL_LEASE, set(), (60000, 60001))[0]
            self.assertGreaterEqual(start, bind_registry.PREFERRED_LOWEST_PORT)
        high = set(range(bind_registry.PREFERRED_LOWEST_PORT, bind_registry.HIGHEST_PORT + 1))
        start = bind_registry.ordered_blocks(SMALL_LEASE, high, (60000, 60001))[0]
        self.assertLess(start + SMALL_LEASE - 1, bind_registry.RESERVED_PORT_BAND[0])

    def test_lease_reports_exhaustion_and_leaves_no_file(self):
        only_two_blocks = (bind_registry.LOWEST_LEASED_PORT, bind_registry.HIGHEST_PORT - 2 * SMALL_LEASE)
        with unittest.mock.patch.object(bind_registry, "ephemeral_port_range", return_value=only_two_blocks), \
                unittest.mock.patch.object(bind_registry, "port_binds", lambda port: False):
            with self.assertRaisesRegex(BindRegistryError, r"no block .*\(2 blocks probed and taken\)"):
                self.lease()
        self.assertEqual(os.listdir(self.registry), [])

    def test_probe_error_releases_the_claimed_block(self):
        held, path = self.lease()
        error = OSError(errno.EMFILE, os.strerror(errno.EMFILE))
        with unittest.mock.patch.object(bind_registry, "port_binds", side_effect=error):
            with self.assertRaises(OSError) as raised:
                self.lease()
        self.assertEqual(raised.exception.errno, errno.EMFILE)
        self.assertEqual(self.registered(path)["ports"], held)
        bind_registry.release(path)
        bind_registry._written_registrations.clear()
        with unittest.mock.patch.object(bind_registry, "port_binds", side_effect=error):
            with self.assertRaises(OSError):
                self.lease()
        self.assertEqual(os.listdir(self.registry), [])

    def test_concurrent_leases_in_one_process_keep_each_others_ports(self):
        def slow_binds(port):
            time.sleep(0.01)  # Keeps both leases' claims open at the same time.
            return True

        results = []
        with unittest.mock.patch.object(bind_registry, "port_binds", slow_binds):
            threads = [threading.Thread(target=lambda: results.append(self.lease())) for _ in range(2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=CHILD_TIMEOUT_SECONDS)
        (first, path), (second, _) = results
        self.assertFalse(set(first) & set(second))
        self.assertEqual(sorted(self.registered(path)["ports"]), sorted(first + second))

    def test_block_is_released_only_after_every_probe_returned(self):
        own = self.registry / f"{os.getpid()}{REGISTRATION_SUFFIX}"
        first, finished, released = [], [], []
        original_remove = bind_registry._remove_ports_locked

        def first_port_taken_rest_slow(port):
            if not first:
                first.extend(json.loads(own.read_text())["ports"])
            if port == first[0]:
                return False  # Answers at once; a lease that stops at the first False releases too early.
            if port in first:
                time.sleep(0.3)
                finished.append(time.monotonic())
            return True

        def recording_remove(path, ports):
            released.append(time.monotonic())
            original_remove(path, ports)

        with unittest.mock.patch.object(bind_registry, "port_binds", first_port_taken_rest_slow), \
                unittest.mock.patch.object(bind_registry, "_remove_ports_locked", recording_remove):
            self.lease()
        self.assertEqual(len(released), 1)
        self.assertEqual(len(finished), SMALL_LEASE - 1)
        self.assertGreaterEqual(released[0], max(finished))

    def test_rejected_block_is_released_in_the_next_claims_critical_section(self):
        own = self.registry / f"{os.getpid()}{REGISTRATION_SUFFIX}"
        first, sections = [], []
        original_enter = bind_registry.RegistryLock.__enter__

        def counting_enter(lock):
            sections.append(lock.path)
            return original_enter(lock)

        def first_block_taken(port):
            if not first:
                first.extend(json.loads(own.read_text())["ports"])
            return port not in first

        with unittest.mock.patch.object(bind_registry, "port_binds", first_block_taken), \
                unittest.mock.patch.object(bind_registry.RegistryLock, "__enter__", counting_enter):
            ports, path = self.lease()
        self.assertEqual(len(sections), 2)  # Claim, then release-and-claim: no separate round trip for the release.
        self.assertEqual(self.registered(path)["ports"], ports)
        self.assertFalse(set(first) & set(ports))

    def test_claim_skips_a_snapshot_candidate_claimed_meanwhile(self):
        # The candidate order comes from a snapshot outside the lock; a block registered after it is not taken.
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        original_order = bind_registry.ordered_blocks
        stolen = []

        def order_then_lose_the_first(count, excluded, ephemeral, rng=None):
            starts = original_order(count, excluded, ephemeral, rng)
            if not stolen and starts:
                stolen.extend(range(starts[0], starts[0] + count))
                self.write(f"{sleeper.pid}{REGISTRATION_SUFFIX}", {"executable": "sleep", "ports": stolen})
            return starts

        with unittest.mock.patch.object(bind_registry, "ordered_blocks", order_then_lose_the_first):
            ports, _ = self.lease()
        self.assertTrue(stolen)
        self.assertFalse(set(stolen) & set(ports))

    def test_lease_does_not_spin_on_a_ghost_registration(self):
        # The unlocked snapshot counts every parseable file; the locked read skips this pid's own file before its first
        # write (a recycled pid) and a dead pid's file this user may not delete. With the only free blocks named in such
        # a file, the lease must still claim one of them rather than spin.
        only_two_blocks = (bind_registry.LOWEST_LEASED_PORT, bind_registry.HIGHEST_PORT - 2 * SMALL_LEASE)
        free = list(range(bind_registry.HIGHEST_PORT - 2 * SMALL_LEASE + 1, bind_registry.HIGHEST_PORT + 1))
        for mode in ("recycled own pid", "undeletable dead pid"):
            with self.subTest(mode):
                for leftover in self.registry.glob("*") if self.registry.exists() else []:
                    leftover.unlink()
                bind_registry._written_registrations.clear()
                owner = os.getpid() if mode == "recycled own pid" else dead_pid()
                self.write(f"{owner}{REGISTRATION_SUFFIX}", {"executable": "python3", "ports": free})
                patches = [unittest.mock.patch.object(bind_registry, "ephemeral_port_range",
                                                      return_value=only_two_blocks)]
                if mode == "undeletable dead pid":
                    patches.append(unittest.mock.patch.object(bind_registry, "_remove", lambda path: None))
                result = {}

                def lease_in_thread():
                    try:
                        result["ports"] = self.lease()[0]
                    except BaseException as error:  # noqa: BLE001  Reported through the assertions below.
                        result["error"] = error

                with contextlib.ExitStack() as stack:
                    for patch in patches:
                        stack.enter_context(patch)
                    thread = threading.Thread(target=lease_in_thread, daemon=True)
                    thread.start()
                    thread.join(timeout=GHOST_LEASE_DEADLINE_SECONDS)
                    spinning = thread.is_alive()
                    if spinning:
                        stack.enter_context(unittest.mock.patch.object(  # Make the stuck lease fail its next round.
                            bind_registry, "ordered_blocks", side_effect=BindRegistryError("stopped by the test")))
                        thread.join(timeout=CHILD_TIMEOUT_SECONDS)
                self.assertFalse(spinning, f"lease() still running after {GHOST_LEASE_DEADLINE_SECONDS} s")
                self.assertNotIn("error", result)
                self.assertTrue(set(result["ports"]) <= set(free))

    def test_lock_timeout_does_not_wait_for_the_lock_again(self):
        # A rejected block is pending when the next claim's lock wait runs out: the lease must not wait a second time.
        own = self.registry / f"{os.getpid()}{REGISTRATION_SUFFIX}"
        entries = []
        original_enter = bind_registry.RegistryLock.__enter__

        def enter_then_time_out(lock):
            entries.append(lock.path)
            if len(entries) > 1:
                raise bind_registry.LockTimeoutError("bind registry lock is still held")
            return original_enter(lock)

        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), \
                unittest.mock.patch.object(bind_registry, "port_binds", lambda port: False), \
                unittest.mock.patch.object(bind_registry.RegistryLock, "__enter__", enter_then_time_out):
            with self.assertRaises(bind_registry.LockTimeoutError):
                self.lease()
        self.assertEqual(len(entries), 2)
        self.assertIn("leaving ports", stderr.getvalue())
        self.assertTrue(own.exists())  # Left for the next reader to reclaim once this process has exited.

    def test_release_failure_keeps_the_original_error(self):
        emfile = OSError(errno.EMFILE, os.strerror(errno.EMFILE))
        enospc = OSError(errno.ENOSPC, os.strerror(errno.ENOSPC))
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), \
                unittest.mock.patch.object(bind_registry, "port_binds", side_effect=emfile), \
                unittest.mock.patch.object(bind_registry, "_remove_ports_locked", side_effect=enospc):
            with self.assertRaises(OSError) as raised:
                self.lease()
        self.assertEqual(raised.exception.errno, errno.EMFILE)
        self.assertIn("cannot release ports", stderr.getvalue())

    def test_interrupt_right_after_the_claim_releases_the_block(self):
        # KeyboardInterrupt between leaving the claim's critical section and probing must not leak the claimed block.
        original_exit = bind_registry.RegistryLock.__exit__
        interrupted = []

        def exit_then_interrupt(lock, *exc_info):
            original_exit(lock, *exc_info)
            if not interrupted:
                interrupted.append(True)
                raise KeyboardInterrupt

        with unittest.mock.patch.object(bind_registry.RegistryLock, "__exit__", exit_then_interrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.lease()
        self.assertEqual(os.listdir(self.registry), [])

    def test_invalid_requested_ports_rejects_bools(self):
        self.assertIsNotNone(bind_registry.invalid_requested_ports([True]))
        self.assertIsNotNone(bind_registry.invalid_requested_ports([20120, False]))
        self.assertIsNone(bind_registry.invalid_requested_ports([20120, 20121]))

    def test_register_ports_rejects_repeats_reserved_band_and_out_of_range(self):
        low = bind_registry.RESERVED_PORT_BAND[0]
        for ports in ([20101, 20101], [low], [20102, low + 1], [0], [bind_registry.HIGHEST_PORT + 1], []):
            with self.assertRaises(BindRegistryError, msg=ports):
                bind_registry.register_ports(ports, environ=self.environ)
        self.assertFalse(os.path.exists(self.registry / f"{os.getpid()}{REGISTRATION_SUFFIX}"))

    def test_register_ports_appends_after_the_first_registration(self):
        path, _ = bind_registry.register_ports([20111, 20112], environ=self.environ)
        bind_registry.register_ports([20113], environ=self.environ)
        self.assertEqual(self.registered(path)["ports"], [20111, 20112, 20113])

    def test_concurrent_leases_that_both_fail_leave_no_file(self):
        only_two_blocks = (bind_registry.LOWEST_LEASED_PORT, bind_registry.HIGHEST_PORT - 2 * SMALL_LEASE)

        def slow_refusal(port):
            time.sleep(0.01)  # Keeps both leases' claims open at the same time.
            return False

        errors = []

        def lease_expecting_failure():
            try:
                self.lease()
            except BindRegistryError as error:
                errors.append(error)

        with unittest.mock.patch.object(bind_registry, "ephemeral_port_range", return_value=only_two_blocks), \
                unittest.mock.patch.object(bind_registry, "port_binds", slow_refusal):
            threads = [threading.Thread(target=lease_expecting_failure) for _ in range(2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=CHILD_TIMEOUT_SECONDS)
        self.assertEqual(len(errors), 2)
        self.assertEqual(os.listdir(self.registry), [])

    def test_probe_error_is_raised_only_after_every_probe_returned(self):
        finished = []

        def first_raises_rest_slow(port):
            if port == 0:
                raise OSError(errno.EMFILE, os.strerror(errno.EMFILE))
            time.sleep(0.3)
            finished.append(time.monotonic())
            return True

        with ThreadPoolExecutor(max_workers=bind_registry.BIND_CHECK_THREADS) as pool:
            with self.assertRaises(OSError):
                bind_registry.block_binds(list(range(SMALL_LEASE)), pool, first_raises_rest_slow)
            raised_at = time.monotonic()
        self.assertEqual(len(finished), SMALL_LEASE - 1)
        self.assertGreaterEqual(raised_at, max(finished))

    def test_failed_submit_waits_for_the_probes_already_submitted(self):
        finished = []

        def slow(port):
            time.sleep(0.3)
            finished.append(port)
            return True

        class FailingPool:
            """Starts two probes, then fails to start the third, like a pool out of threads."""

            def __init__(self, pool):
                self.pool, self.submitted = pool, 0

            def submit(self, *args):
                self.submitted += 1
                if self.submitted == 3:
                    raise RuntimeError("can't start new thread")
                return self.pool.submit(*args)

        with ThreadPoolExecutor(max_workers=bind_registry.BIND_CHECK_THREADS) as pool:
            with self.assertRaisesRegex(RuntimeError, "new thread"):
                bind_registry.block_binds(list(range(SMALL_LEASE)), FailingPool(pool), slow)
            self.assertEqual(sorted(finished), [0, 1])  # Both submitted probes ended before the error surfaced.

    def test_parse_lease(self):
        good = list(range(20000, 20000 + LEASE_PORT_COUNT))
        self.assertEqual(bind_registry.parse_lease(bind_registry.format_lease(good)), good)
        for bad in ("", "a,b", bind_registry.format_lease(good[:-1]), bind_registry.format_lease(good + [good[-1] + 1]),
                    bind_registry.format_lease([good[1], good[0]] + good[2:]),
                    bind_registry.format_lease(range(0, LEASE_PORT_COUNT)),
                    bind_registry.format_lease(range(65500, 65500 + LEASE_PORT_COUNT))):
            with self.assertRaises(BindRegistryError):
                bind_registry.parse_lease(bad)

    def test_in_process_lease_is_taken_once_across_threads(self):
        script = (NO_PROBE_PRELUDE + "import os, threading\n"
                  "results = []\n"
                  "threads = [threading.Thread(target=lambda: results.append(bind_registry.ensure_ports())) for _ in range(8)]\n"
                  "[t.start() for t in threads]; [t.join() for t in threads]\n"
                  "assert len(results) == 8 and all(r == results[0] for r in results)\n"
                  "files = os.listdir(os.environ['WIRE_BIND_REGISTRY_PATH'])\n"
                  "assert len(files) == 1, files\n"
                  "assert len(__import__('json').load(open(os.path.join(os.environ['WIRE_BIND_REGISTRY_PATH'], files[0])))['ports']) == bind_registry.LEASE_PORT_COUNT\n")
        result = subprocess.run([sys.executable, "-c", script], cwd=TESTS_DIR, env=self.child_env(),
                                capture_output=True, text=True, timeout=CHILD_TIMEOUT_SECONDS)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_in_process_lease_on_first_use(self):
        script = (NO_PROBE_PRELUDE + "import os\n"
                  "a = bind_registry.ensure_ports(); b = bind_registry.ensure_ports()\n"
                  "assert a == b and os.environ['SYSIO_TEST_PORTS'] == bind_registry.format_lease(a)\n"
                  "assert os.path.exists(os.path.join(os.environ['WIRE_BIND_REGISTRY_PATH'], f'{os.getpid()}.bind-config.json'))\n"
                  "print('ok')\n")
        result = subprocess.run([sys.executable, "-c", script], cwd=TESTS_DIR, env=self.child_env(),
                                capture_output=True, text=True, timeout=CHILD_TIMEOUT_SECONDS)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(os.listdir(self.registry), [])  # Removed at exit.


class PortBinds(unittest.TestCase):
    """port_binds with failures injected; IPv4 binds for real, on HOST_PORTS only."""

    @staticmethod
    def factory(ipv6_errno=None, create_errno=None):
        def make(family, kind):
            if family == socket.AF_INET6:
                if create_errno is not None:
                    raise OSError(create_errno, os.strerror(create_errno))
                fake = unittest.mock.MagicMock()
                fake.__enter__.return_value = fake
                if ipv6_errno is not None:
                    fake.bind.side_effect = OSError(ipv6_errno, os.strerror(ipv6_errno))
                return fake
            return socket.socket(family, kind)
        return make

    def free_port(self):
        """A port this suite holds in the host registry (see the module docstring)."""
        return HOST_PORTS[0]

    def test_ipv6_unavailable_counts_as_free(self):
        port = self.free_port()
        for unavailable in (errno.EADDRNOTAVAIL, errno.EAFNOSUPPORT):
            self.assertTrue(bind_registry.port_binds(port, self.factory(ipv6_errno=unavailable)))
        self.assertTrue(bind_registry.port_binds(port, self.factory(create_errno=errno.EAFNOSUPPORT)))

    def test_ipv6_in_use_counts_as_taken(self):
        port = self.free_port()
        self.assertFalse(bind_registry.port_binds(port, self.factory(ipv6_errno=errno.EADDRINUSE)))
        self.assertFalse(bind_registry.port_binds(port, self.factory(ipv6_errno=errno.EACCES)))

    def test_errors_unrelated_to_the_port_are_raised(self):
        port = self.free_port()

        def ipv4_fails(create_errno=None, bind_errno=None):
            def make(family, kind):
                if create_errno is not None:
                    raise OSError(create_errno, os.strerror(create_errno))
                fake = unittest.mock.MagicMock()
                fake.__enter__.return_value = fake
                fake.bind.side_effect = OSError(bind_errno, os.strerror(bind_errno))
                return fake
            return make

        for kwargs in ({"create_errno": errno.EMFILE}, {"bind_errno": errno.ENOBUFS},
                       {"bind_errno": errno.ENOMEM}, {"bind_errno": errno.EPERM},
                       {"bind_errno": errno.EADDRNOTAVAIL}):
            with self.assertRaises(OSError, msg=kwargs) as raised:
                bind_registry.port_binds(port, ipv4_fails(**kwargs))
            self.assertEqual(raised.exception.errno, next(iter(kwargs.values())))
        with self.assertRaises(OSError):
            bind_registry.port_binds(port, self.factory(ipv6_errno=errno.ENOBUFS))
        self.assertFalse(bind_registry.port_binds(port, ipv4_fails(bind_errno=errno.EADDRINUSE)))

    def test_ipv4_in_use_counts_as_taken(self):
        port = self.free_port()
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as held:
            held.bind(("0.0.0.0", port))
            held.listen(1)
            self.assertFalse(bind_registry.port_binds(port, self.factory()))


class StressDescriptorLimit(unittest.TestCase):
    """bind_registry_stress_test.raise_descriptor_limit, with getrlimit/setrlimit faked."""

    class FakeResource:
        RLIMIT_NOFILE = 7
        RLIM_INFINITY = -1

        def __init__(self, soft, hard, honoured=True):
            self.limits, self.honoured = (soft, hard), honoured

        def getrlimit(self, which):
            return self.limits

        def setrlimit(self, which, limits):
            if self.honoured:
                self.limits = limits

    def setUp(self):
        import bind_registry_stress_test
        self.raise_limit = bind_registry_stress_test.raise_descriptor_limit

    def test_raises_the_soft_limit_to_the_hard_one(self):
        fake = self.FakeResource(256, 10240)
        self.raise_limit(448, fake)
        self.assertEqual(fake.limits, (10240, 10240))
        fake = self.FakeResource(256, self.FakeResource.RLIM_INFINITY)
        self.raise_limit(448, fake)
        self.assertEqual(fake.limits, (448, self.FakeResource.RLIM_INFINITY))

    def test_fails_clearly_when_the_limit_stays_too_low(self):
        for fake in (self.FakeResource(256, 300), self.FakeResource(256, 10240, honoured=False)):
            with self.assertRaisesRegex(SystemExit, "needs 448 descriptors"):
                self.raise_limit(448, fake)


class Wrapper(RegistryCase):
    def wrap(self, *command, **kwargs):
        return subprocess.run([*WRAPPER_COMMAND, *command], env=self.child_env(), capture_output=True,
                              text=True, timeout=CHILD_TIMEOUT_SECONDS, **kwargs)

    def test_passes_lease_and_cleans_up(self):
        result = self.wrap(sys.executable, "-c", "import os, json, sys; "
                           "ports = os.environ['SYSIO_TEST_PORTS']; "
                           "files = os.listdir(os.environ['WIRE_BIND_REGISTRY_PATH']); "
                           "doc = json.load(open(os.path.join(os.environ['WIRE_BIND_REGISTRY_PATH'], files[0]))); "
                           "assert ','.join(map(str, doc['ports'])) == ports; "
                           "assert files == [f'{os.getppid()}.bind-config.json'], files; "
                           "assert os.getpgid(0) == os.getpid(); print(len(doc['ports']))")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(LEASE_PORT_COUNT))
        self.assertEqual(os.listdir(self.registry), [])

    def test_exit_status_propagates(self):
        self.assertEqual(self.wrap("sh", "-c", "exit 7").returncode, 7)

    def test_signal_death_is_128_plus_signal(self):
        result = self.wrap(sys.executable, "-c", "import os, signal; os.kill(os.getpid(), signal.SIGKILL)")
        self.assertEqual(result.returncode, 128 + signal.SIGKILL)
        result = self.wrap(sys.executable, "-c", "import os, signal; os.kill(os.getpid(), signal.SIGSEGV)")
        self.assertEqual(result.returncode, 128 + signal.SIGSEGV)
        self.assertEqual(os.listdir(self.registry), [])

    def test_forwards_signals_to_the_whole_session(self):
        ready = self.root / "ready"
        received = self.root / "received"
        # The background child records the SIGTERM it is sent and exits. If only the leader were signalled, the
        # child would get its SIGTERM from the drain instead, which the wrapper reports on stderr.
        child = ("import os, signal, sys, time, pathlib\n"
                 "def term(*_):\n"
                 "    pathlib.Path(sys.argv[2]).write_text('SIGTERM')\n"
                 "    os._exit(0)\n"
                 "signal.signal(signal.SIGTERM, term)\n"
                 "pathlib.Path(sys.argv[1]).write_text(str(os.getsid(0)))\n"
                 f"time.sleep({OUTLIVES_EVERY_WINDOW_SECONDS})\n")
        wrapper = subprocess.Popen(
            [*WRAPPER_COMMAND, "sh", "-c", f'"$0" -c "$1" "$2" "$3" & wait', sys.executable, child, str(ready),
             str(received)], env=self.child_env(), stderr=subprocess.PIPE, text=True)
        self.children.append(wrapper)
        deadline = time.monotonic() + CHILD_TIMEOUT_SECONDS
        while not (ready.exists() and ready.read_text()):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(bind_registry.GROUP_POLL_SECONDS)
        self.sessions.append(int(ready.read_text()))
        wrapper.send_signal(signal.SIGTERM)
        _, stderr = wrapper.communicate(timeout=CHILD_TIMEOUT_SECONDS)
        self.assertEqual(wrapper.returncode, 128 + signal.SIGTERM, stderr)
        self.assertEqual(received.read_text(), "SIGTERM")
        self.assertNotIn("left processes", stderr)
        self.assertEqual(os.listdir(self.registry), [])

    def test_forwarded_signal_is_delivered_once(self):
        ready = self.root / "ready"
        received = self.root / "received"
        script = ("import signal, sys, time, pathlib\n"
                  "log = pathlib.Path(sys.argv[2])\n"
                  "signal.signal(signal.SIGINT, lambda *_: log.open('a').write('SIGINT\\n'))\n"
                  "pathlib.Path(sys.argv[1]).touch()\n"
                  "deadline = time.monotonic() + 60\n"
                  "while not log.exists() and time.monotonic() < deadline: time.sleep(0.05)\n"
                  "time.sleep(1)\n")
        wrapper = subprocess.Popen([*WRAPPER_COMMAND, sys.executable, "-c", script, str(ready), str(received)],
                                   env=self.child_env())
        self.children.append(wrapper)
        deadline = time.monotonic() + CHILD_TIMEOUT_SECONDS
        while not ready.exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(bind_registry.GROUP_POLL_SECONDS)
        wrapper.send_signal(signal.SIGINT)
        self.assertEqual(wrapper.wait(timeout=CHILD_TIMEOUT_SECONDS), 0)
        self.assertEqual(received.read_text().splitlines(), ["SIGINT"])

    def test_signal_skips_a_pid_no_longer_in_the_session(self):
        sleeper = spawn_sleeper()  # In this test process's session, not the one named below.
        self.children.append(sleeper)
        other_session = os.getsid(0) + 1
        bind_registry._signal_member(sleeper.pid, other_session, signal.SIGTERM)
        time.sleep(SHORT_GROUP_SECONDS)
        self.assertIsNone(sleeper.poll())
        bind_registry._signal_member(sleeper.pid, os.getsid(0), signal.SIGTERM)
        self.assertEqual(sleeper.wait(timeout=CHILD_TIMEOUT_SECONDS), -signal.SIGTERM)

    def test_stragglers_are_terminated_before_release(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), unittest.mock.patch.dict(os.environ, self.environ):
            status = bind_registry.run(
                [sys.executable, "-c", "import subprocess; subprocess.Popen(['sh', '-c', "
                 f"'trap \"\" TERM; sleep {OUTLIVES_EVERY_WINDOW_SECONDS}'])"],
                drain=SHORT_GROUP_SECONDS, term=SHORT_GROUP_SECONDS, kill=SIGKILL_WINDOW_SECONDS)
        # The straggler ignores SIGTERM and outlives every window: only the SIGKILL step can end it in time.
        self.assertEqual(status, 0, stderr.getvalue())
        self.assertIn("ignored SIGTERM; sent SIGKILL", stderr.getvalue())
        self.assertEqual(os.listdir(self.registry), [])

    def test_descendants_that_change_process_group_are_drained(self):
        marker = self.root / "escaped.pid"
        received = self.root / "escaped.received"
        escaped = ("import os, signal, sys, time, pathlib\n"
                   "def term(*_):\n"
                   "    pathlib.Path(sys.argv[1]).write_text('SIGTERM')\n"
                   "    os._exit(0)\n"
                   "signal.signal(signal.SIGTERM, term)\n"
                   f"time.sleep({OUTLIVES_EVERY_WINDOW_SECONDS})\n")
        script = ("import os, subprocess, sys\n"
                  "p = subprocess.Popen([sys.executable, '-c', sys.argv[3], sys.argv[2]], preexec_fn=os.setpgrp)\n"
                  "open(sys.argv[1], 'w').write(str(p.pid))\n")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), unittest.mock.patch.dict(os.environ, self.environ):
            status = bind_registry.run([sys.executable, "-c", script, str(marker), str(received), escaped],
                                       drain=SHORT_GROUP_SECONDS, term=CHILD_TIMEOUT_SECONDS, kill=SHORT_GROUP_SECONDS)
        # It left the test's process group but not its session; only the SIGTERM step records the marker.
        self.assertEqual(status, 0, stderr.getvalue())
        self.assertIn("sent SIGTERM", stderr.getvalue())
        self.assertEqual(received.read_text(), "SIGTERM")
        self.assertEqual(os.listdir(self.registry), [])

    def test_escalation_survives_a_broken_stderr_and_eperm(self):
        # ctest gone (stderr broken) and an EPERM from one member must not stop the SIGTERM/SIGKILL escalation.
        broken = unittest.mock.MagicMock()
        broken.write.side_effect = BrokenPipeError(errno.EPIPE, "broken pipe")
        original_member = bind_registry._signal_member
        calls = []

        def eperm_first(pid, sid, signum):
            calls.append(pid)
            if len(calls) == 1:
                raise PermissionError(errno.EPERM, "not permitted")
            original_member(pid, sid, signum)

        with unittest.mock.patch.object(sys, "stderr", broken), \
                unittest.mock.patch.object(bind_registry, "_signal_member", eperm_first), \
                unittest.mock.patch.dict(os.environ, self.environ):
            status = bind_registry.run(
                [sys.executable, "-c", f"import subprocess; subprocess.Popen(['sleep', '{OUTLIVES_EVERY_WINDOW_SECONDS}'])"],
                drain=SHORT_GROUP_SECONDS, term=SIGKILL_WINDOW_SECONDS, kill=SIGKILL_WINDOW_SECONDS)
        self.assertEqual(status, 0)
        self.assertEqual(os.listdir(self.registry), [])

    def test_drain_without_a_process_list_still_reaps_and_releases(self):
        # No procfs and a failing ps: the drain falls back to the leader's process group, which it can only test for
        # existence (killpg 0), and a zombie still counts. The straggler is orphaned when the test's leader exits; under
        # a PID 1 that never reaps (a CI job container) it would stay a zombie in the group forever. Real macOS (the
        # no-procfs platform) reaps orphans through launchd, so this process stands in for it: it becomes the orphan's
        # subreaper and reaps exactly that pid.
        marker = self.root / "straggler.pid"
        real_run = subprocess.run

        def failing_ps(command, *args, **kwargs):
            """Fail only ps; every other command (macOS's sysctl read of the ephemeral range) runs for real."""
            if command[0] == "ps":
                raise subprocess.CalledProcessError(1, command)
            return real_run(command, *args, **kwargs)

        stderr = io.StringIO()
        with reaping_orphan_from(marker), contextlib.redirect_stderr(stderr), \
                unittest.mock.patch.dict(os.environ, self.environ), hidden_procfs(), \
                unittest.mock.patch.object(bind_registry.subprocess, "run", failing_ps):
            status = bind_registry.run(
                [sys.executable, "-c",
                 f"import subprocess, sys; p = subprocess.Popen(['sleep', '{OUTLIVES_EVERY_WINDOW_SECONDS}']); "
                 "open(sys.argv[1], 'w').write(str(p.pid))", str(marker)],
                drain=SHORT_GROUP_SECONDS, term=SIGKILL_WINDOW_SECONDS, kill=SIGKILL_WINDOW_SECONDS)
        self.assertEqual(status, 0, stderr.getvalue())
        self.assertEqual(stderr.getvalue().count("cannot list processes"), 1)
        self.assertIn("sent SIGTERM", stderr.getvalue())  # The fallback escalated; it did not just return.
        self.assertNotIn("None", stderr.getvalue())
        self.assertFalse(bind_registry._pid_alive(int(marker.read_text())))  # Gone (and reaped) when run() returned.
        self.assertEqual(os.listdir(self.registry), [])

    @linux_only
    def test_drain_without_procfs_finds_the_session_through_ps_and_getsid(self):
        # The no-procfs path (macOS in production) run on Linux with procfs hidden and the real `ps -A -o pid=,stat=`.
        # The straggler leaves the leader's process group, so only the ps + getsid session scan can find it, and only
        # the SIGTERM it then sends writes the marker.
        marker = self.root / "escaped.pid"
        received = self.root / "escaped.received"
        escaped = ("import os, signal, sys, time, pathlib\n"
                   "def term(*_):\n"
                   "    pathlib.Path(sys.argv[1]).write_text('SIGTERM')\n"
                   "    os._exit(0)\n"
                   "signal.signal(signal.SIGTERM, term)\n"
                   f"time.sleep({OUTLIVES_EVERY_WINDOW_SECONDS})\n")
        script = ("import os, subprocess, sys\n"
                  "p = subprocess.Popen([sys.executable, '-c', sys.argv[3], sys.argv[2]], preexec_fn=os.setpgrp)\n"
                  "open(sys.argv[1], 'w').write(str(p.pid))\n")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), unittest.mock.patch.dict(os.environ, self.environ), hidden_procfs():
            status = bind_registry.run([sys.executable, "-c", script, str(marker), str(received), escaped],
                                       drain=SHORT_GROUP_SECONDS, term=CHILD_TIMEOUT_SECONDS, kill=SHORT_GROUP_SECONDS)
        self.assertEqual(status, 0, stderr.getvalue())
        self.assertNotIn("cannot list processes", stderr.getvalue())
        self.assertEqual(received.read_text(), "SIGTERM")
        self.assertEqual(os.listdir(self.registry), [])

    @linux_only
    def test_no_procfs_scan_and_signal_stay_inside_the_session(self):
        # Without procfs, session_members filters ps's list by getsid, and _signal_member re-checks getsid before
        # os.kill. A process in another session must be neither listed nor signalled.
        outsider = spawn_sleeper()  # In this test process's session.
        self.children.append(outsider)
        member = subprocess.Popen(["sleep", str(OUTLIVES_EVERY_WINDOW_SECONDS)], start_new_session=True)
        self.children.append(member)
        with hidden_procfs():
            self.assertEqual(bind_registry.session_members(member.pid), [member.pid])
            bind_registry._signal_member(outsider.pid, member.pid, signal.SIGTERM)
            time.sleep(SHORT_GROUP_SECONDS)
            self.assertIsNone(outsider.poll())
            bind_registry._signal_member(member.pid, member.pid, signal.SIGTERM)
        self.assertEqual(member.wait(timeout=CHILD_TIMEOUT_SECONDS), -signal.SIGTERM)

    def test_group_that_outlives_the_bounded_wait_fails_loudly(self):
        # A group that survives SIGKILL cannot be produced on demand, so suppress
        # the signals and let the bounded wait run out.
        original_signal_session = bind_registry._signal_session
        bind_registry._signal_session = lambda sid, signum: bind_registry.session_members(sid)
        stderr = io.StringIO()
        leftover = None
        try:
            with contextlib.redirect_stderr(stderr), unittest.mock.patch.dict(os.environ, self.environ):
                status = bind_registry.run(
                    [sys.executable, "-c",
                     f"import subprocess; subprocess.Popen(['sleep', '{OUTLIVES_EVERY_WINDOW_SECONDS}'])"],
                    drain=SHORT_GROUP_SECONDS, term=SHORT_GROUP_SECONDS, kill=SHORT_GROUP_SECONDS)
            leftover = [int(pid) for pid in stderr.getvalue().split("processes [")[-1].split("]")[0].split(",")]
        finally:
            bind_registry._signal_session = original_signal_session
            for pid in leftover or []:
                os.kill(pid, signal.SIGKILL)
        self.assertEqual(status, WRAPPER_FAILURE_STATUS)
        self.assertIn("survived SIGKILL", stderr.getvalue())
        self.assertEqual(os.listdir(self.registry), [f"{os.getpid()}{REGISTRATION_SUFFIX}"])

    def test_lease_failure_is_reported(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), \
                unittest.mock.patch.object(bind_registry, "lease", side_effect=BindRegistryError("lock held")):
            status = bind_registry.run(["true"])
        self.assertEqual(status, WRAPPER_FAILURE_STATUS)
        self.assertIn("cannot lease", stderr.getvalue())

    def test_usage(self):
        self.assertEqual(self.wrap().returncode, bind_registry.USAGE_STATUS)


class Register(RegistryCase):
    """The ``--register`` command: wire-tools-ts' interop client contract."""

    def start(self, *ports):
        child = subprocess.Popen([sys.executable, WRAPPER, bind_registry.REGISTER_COMMAND, *map(str, ports)],
                                 env=self.child_env(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        self.children.append(child)
        return child

    def test_registers_reports_and_releases(self):
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        self.write(f"{sleeper.pid}{REGISTRATION_SUFFIX}", {"executable": "sleep", "ports": [20021]})
        child = self.start(20031, 20032)
        report = json.loads(child.stdout.readline())
        self.assertEqual(report["pid"], child.pid)
        self.assertEqual(report["registeredPorts"], [20021])
        self.assertEqual(report["lockDirectory"], bind_registry.lock_path(str(self.registry), self.environ))
        self.assertEqual(json.loads(Path(report["registrationFile"]).read_text()),
                         {"executable": report["executable"], "ports": [20031, 20032]})
        child.stdin.close()
        self.assertEqual(child.wait(timeout=CHILD_TIMEOUT_SECONDS), 0)
        self.assertFalse(Path(report["registrationFile"]).exists())

    @procfs_only
    def test_executable_form_owns_its_registration_in_request_order(self):
        script = str(Path(__file__).with_name("bind_registry_register.py"))
        child = subprocess.Popen([script, "20072", "20071"], env=self.child_env(), stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(child)
        report = json.loads(child.stdout.readline())
        self.assertEqual(report["pid"], child.pid)
        self.assertEqual(report["executable"], bind_registry.process_basename(child.pid))
        self.assertEqual(json.loads(Path(report["registrationFile"]).read_text())["ports"], [20072, 20071])
        child.stdin.close()
        self.assertEqual(child.wait(timeout=CHILD_TIMEOUT_SECONDS), 0)
        self.assertFalse(Path(report["registrationFile"]).exists())

    def test_refuses_held_ports(self):
        sleeper = spawn_sleeper()
        self.children.append(sleeper)
        self.write(f"{sleeper.pid}{REGISTRATION_SUFFIX}", {"executable": "sleep", "ports": [20041]})
        child = self.start(20041)
        self.assertEqual(child.wait(timeout=CHILD_TIMEOUT_SECONDS), bind_registry.REGISTER_PORT_HELD_STATUS)

    def test_usage(self):
        for ports in ((), ("x",), (0,), (65536,), (20051, 20051)):
            child = self.start(*ports)
            self.assertEqual(child.wait(timeout=CHILD_TIMEOUT_SECONDS), bind_registry.USAGE_STATUS, ports)

    def test_announces_waiting_for_the_lock(self):
        self.registry.mkdir(parents=True)
        lock = bind_registry.lock_path(str(self.registry), self.environ)
        os.mkdir(lock)
        child = self.start(20061)
        self.assertEqual(child.stdout.readline().strip(), bind_registry.WAITING_FOR_LOCK_LINE)
        os.rmdir(lock)
        self.assertEqual(json.loads(child.stdout.readline())["registeredPorts"], [])
        child.stdin.close()
        self.assertEqual(child.wait(timeout=CHILD_TIMEOUT_SECONDS), 0)


class HarnessImport(RegistryCase):
    def test_importing_the_harness_takes_no_lease(self):
        if not (Path(TESTS_DIR) / "TestHarness" / "core_symbol.py").exists():
            self.skipTest("TestHarness is only importable from the build tree")
        script = (NO_PROBE_PRELUDE + "import os\nfrom TestHarness import Cluster, TestHelper, Utils, WalletMgr\n"
                  "cluster = Cluster(defproduceraPrvtKey=None)\n"
                  "assert 'SYSIO_TEST_PORTS' not in os.environ\n"
                  "assert not os.listdir(os.environ['WIRE_BIND_REGISTRY_PATH'])\n"
                  "port = TestHelper.DEFAULT_PORT\n"
                  "assert port == Utils.getPort(Utils.PortNodeHttp)\n"
                  "assert os.listdir(os.environ['WIRE_BIND_REGISTRY_PATH'])\n")
        self.registry.mkdir()
        result = subprocess.run([sys.executable, "-c", script], cwd=TESTS_DIR, env=self.child_env(),
                                capture_output=True, text=True, timeout=CHILD_TIMEOUT_SECONDS)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
