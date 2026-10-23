"""Lease test ports from the WIRE bind registry and run a test under the lease.

The bind registry is the host-wide record of ports held by WIRE processes: a
directory with one ``<pid>.bind-config.json`` file per owner, plus a lock taken
while a process reads the directory and writes its own file. wire-tools-ts'
``BindConfigProvider`` (Node) and this module (Python standard library only)
implement the same protocol, specified in wire-tools-ts
``docs/bind-registry/README.md``.

It is part of TestHarness, so an installed TestHarness can lease ports too.
``tests/bind_registry.py`` is its program entry point, the wrapper CTest puts
around network tests::

    python3 tests/bind_registry.py <test command> [args...]

It leases ``LEASE_PORT_COUNT`` ports, runs the command as the leader of a new
session with ``SYSIO_TEST_PORTS`` set to the comma-separated lease, waits for the
whole session to exit, deletes its registration and exits with the test's status
(128 + signal number when the test died of a signal).

``python3 tests/bind_registry.py --register <port>...`` registers given ports
for wire-tools-ts' interop suite (see :func:`register`).

As a module, :func:`ensure_ports` returns the lease a wrapper passed in, or takes
one for the calling process on first use, so a test run by hand needs no wrapper.
"""
import atexit
from concurrent.futures import ThreadPoolExecutor
import errno
import hashlib
import json
import os
import random
import signal
import socket
import subprocess
import sys
import threading
import time

#: Ports leased per test. TestHarness and the C++ test helpers index into the
#: lease by listener category; every category's slots must fit inside it.
LEASE_PORT_COUNT = 192

#: Environment variable that carries a lease from the wrapper to the test.
PORTS_ENV = "SYSIO_TEST_PORTS"

#: Overrides the registry directory (tests give each suite a private registry).
REGISTRY_PATH_ENV = "WIRE_BIND_REGISTRY_PATH"
REGISTRY_DIRNAME = "wire-platform-bind-config"
REGISTRATION_SUFFIX = ".bind-config.json"
TEMPORARY_SUFFIX = ".tmp"
TMPDIR_ENV_ORDER = ("TMPDIR", "TMP", "TEMP")
DEFAULT_TMPDIR = "/tmp"
LOCK_KEY_HEX_CHARS = 16
#: Largest value a pid_t (signed 32-bit) can hold; larger file-name pids are invalid.
MAX_PID = 2**31 - 1

#: Agave validators bind this band whatever their flags say; never lease it (inclusive).
RESERVED_PORT_BAND = (8000, 10000)
LOWEST_LEASED_PORT = 1024
#: Leases come from above the reserved band first: 1024-7999 holds common dev-service ports
#: (3000, 5000, 5432, 6379, ...) that can start listening mid-test. Lower blocks are a fallback.
PREFERRED_LOWEST_PORT = RESERVED_PORT_BAND[1] + 1
HIGHEST_PORT = 65535
#: An IPv6 bind failing with these means the host has no usable IPv6 (disable_ipv6=1, no
#: IPv6 support), not that the port is taken; like Node's isIpv6PortFree, IPv4 then decides.
IPV6_UNAVAILABLE_ERRNOS = (errno.EADDRNOTAVAIL, errno.EAFNOSUPPORT)
#: A bind failing with these means the port is taken (or reserved for another user).
PORT_TAKEN_ERRNOS = (errno.EADDRINUSE, errno.EACCES)
LINUX_EPHEMERAL_RANGE_FILE = "/proc/sys/net/ipv4/ip_local_port_range"
MACOS_EPHEMERAL_RANGE_COMMAND = ("/usr/sbin/sysctl", "-n", "net.inet.ip.portrange.first",
                                 "net.inet.ip.portrange.last")

#: proper-lockfile parameters shared with BindConfigProvider: a lock directory
#: older than LOCK_STALE_SECONDS belongs to a dead holder; Node gives up after
#: about LOCK_WAIT_SECONDS of retries.
LOCK_STALE_SECONDS = 30
LOCK_WAIT_SECONDS = 25.5
LOCK_RETRY_SECONDS = 0.1

#: Bind checks run on this many threads: a bind can take tens of milliseconds on some hosts (WSL2).
BIND_CHECK_THREADS = 32

#: After the test's leader exits, how long the rest of its session gets to exit
#: on its own, then after SIGTERM, then after SIGKILL, before the wrapper gives
#: up and fails loudly. Together they bound the time the wrapper can add after
#: the test exits (25 s); a test's CTest TIMEOUT must leave room for it when the
#: test can leave processes behind.
GROUP_DRAIN_SECONDS = 5
GROUP_TERM_SECONDS = 10
GROUP_KILL_SECONDS = 10
GROUP_POLL_SECONDS = 0.1

#: Signals the wrapper passes on to every process in the test's session.
FORWARDED_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT,
                     signal.SIGUSR1, signal.SIGUSR2)

#: Shell convention: a process killed by signal N exits with 128 + N.
SIGNAL_STATUS_BASE = 128
#: Exit status when the wrapper itself fails (lease or session cleanup).
WRAPPER_FAILURE_STATUS = 125
USAGE_STATUS = 2

#: ``bind_registry.py --register <port>...``: the wire-tools-ts interop client contract (see :func:`register`).
REGISTER_COMMAND = "--register"
WAITING_FOR_LOCK_LINE = "waiting-for-lock"
REGISTER_PORT_HELD_STATUS = 3
REGISTER_LOCK_TIMEOUT_STATUS = 4


class BindRegistryError(RuntimeError):
    """The registry could not be used; the message says why."""


class LockTimeoutError(BindRegistryError):
    """The registry lock was still held by another process when the bounded wait ran out."""


def node_tmpdir(environ=None):
    """Return the temp directory exactly as Node's ``os.tmpdir()`` resolves it.

    The first non-empty of TMPDIR, TMP and TEMP, else ``/tmp``, with one trailing
    ``/`` removed unless the value is ``/``. Not ``tempfile.gettempdir()``: that
    checks TEMP before TMP and falls back to other directories, so it can
    disagree with Node.
    """
    environ = os.environ if environ is None else environ
    path = next((environ[name] for name in TMPDIR_ENV_ORDER if environ.get(name)), DEFAULT_TMPDIR)
    if len(path) > 1 and path.endswith("/"):
        path = path[:-1]
    return path


def registry_dir(environ=None):
    """Return the registry directory, creating it if missing.

    ``$WIRE_BIND_REGISTRY_PATH`` when set and not empty, else
    ``<tmpdir>/wire-platform-bind-config``.
    """
    environ = os.environ if environ is None else environ
    if environ.get(REGISTRY_PATH_ENV):
        path = environ[REGISTRY_PATH_ENV]
    else:
        path = os.path.join(node_tmpdir(environ), REGISTRY_DIRNAME)
    os.makedirs(path, exist_ok=True)
    return path


def lock_path(registry, environ=None):
    """Return the proper-lockfile lock directory that guards ``registry``."""
    key = hashlib.sha256(os.path.abspath(registry).encode("utf-8")).hexdigest()[:LOCK_KEY_HEX_CHARS]
    return os.path.join(node_tmpdir(environ), f"wire-cluster-ports-{key}.lock.lock")


class RegistryLock:
    """The registry lock, held as a context manager.

    Acquiring makes the lock directory; an existing one older than
    ``LOCK_STALE_SECONDS`` is removed as abandoned. Acquisition gives up with
    :class:`BindRegistryError` after ``wait_seconds``. Releasing removes it.
    Callers hold it for milliseconds, well under the 15 s after which a holder
    would have to refresh its mtime.
    """

    def __init__(self, path, wait_seconds=LOCK_WAIT_SECONDS, on_wait=None):
        self.path = path
        self.wait_seconds = wait_seconds
        self.on_wait = on_wait  # Called once, the first time the lock is found held.
        self._identity = None

    def __enter__(self):
        deadline = time.monotonic() + self.wait_seconds
        waited = False
        while True:
            try:
                os.mkdir(self.path)
                held = os.stat(self.path)
                self._identity = (held.st_dev, held.st_ino, held.st_mtime_ns)
                return self
            except FileExistsError:
                pass
            try:
                age = time.time() - os.stat(self.path).st_mtime
            except FileNotFoundError:
                continue  # Released between mkdir and stat.
            if age > LOCK_STALE_SECONDS:
                try:
                    os.rmdir(self.path)
                except FileNotFoundError:
                    pass
                continue
            if time.monotonic() >= deadline:
                raise LockTimeoutError(
                    f"bind registry lock {self.path} is still held after {self.wait_seconds} s")
            if not waited and self.on_wait is not None:
                self.on_wait()
            waited = True
            time.sleep(LOCK_RETRY_SECONDS)

    def __exit__(self, *_):
        """Remove the lock directory, unless another process broke it as stale and now holds its own."""
        try:
            held = os.stat(self.path)
        except FileNotFoundError:
            _report(f"lock {self.path} was removed while held")
            return
        if (held.st_dev, held.st_ino, held.st_mtime_ns) != self._identity:
            _report(f"lock {self.path} was taken over while held; leaving it to its new holder")
            return
        os.rmdir(self.path)


def process_basename(pid):
    """Return the basename of a process's argv[0] from procfs, or None when unreadable.

    An empty string means procfs shows no argv (a zombie or kernel thread).
    """
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as cmdline:
            argv0 = cmdline.read().split(b"\0", 1)[0]
    except OSError:
        return None
    return os.path.basename(argv0.decode("utf-8", "replace"))


def own_executable():
    """Return this process's executable name as other registry readers will see it ("" without procfs)."""
    return process_basename("self") or ""


def _pid_alive(pid):
    """Return whether ``pid`` exists (EPERM means it exists under another user).

    A number no process can have (too large for the kernel's pid type) is not alive.
    """
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OverflowError:
        return False
    return True


def _registration_pid(name, suffix):
    """Return the pid a registry file name encodes, else None.

    The whole stem must be decimal digits without a leading zero, 1..MAX_PID.
    """
    stem = name[:-len(suffix)]
    if not stem.isascii() or not stem.isdigit() or stem.startswith("0"):
        return None
    pid = int(stem)
    return pid if 0 < pid <= MAX_PID else None


def _valid_registration(document):
    """Return whether a parsed registration has a string ``executable`` and distinct ports in 1..65535."""
    return (isinstance(document, dict)
            and isinstance(document.get("executable"), str)
            and isinstance(document.get("ports"), list)
            and all(isinstance(port, int) and not isinstance(port, bool) and 0 < port <= HIGHEST_PORT
                    for port in document["ports"])
            and len(set(document["ports"])) == len(document["ports"]))


def _remove(path):
    """Delete a file, ignoring one that is already gone.

    A stale file this user may not delete (another user's, in a shared temp
    directory) is reported and left; its ports are not excluded either way.
    """
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    except PermissionError as error:
        _report(f"cannot remove {path}: {error}")


def release(path):
    """Delete a registration file once nothing binds its ports (ignores one already gone)."""
    _remove(path)


def _reap(path, reason):
    """Delete a stale registry file and say why on stderr."""
    _report(f"removing {path}: {reason}")
    _remove(path)


def read_exclusions(registry, own_pid):
    """Return every port in a live registration, deleting stale registrations.

    A registration is stale when its name does not encode a positive pid, the
    pid is dead, its content is not a valid registration, or procfs shows the
    pid now runs a different program. Leftover ``.tmp`` files of dead pids are
    deleted too. The file of ``own_pid`` is never deleted: before this process
    first wrote it, it belongs to a dead process with a recycled pid and is
    skipped (the first write replaces it); after that its ports are excluded like
    any other live registration's, so a process that leases twice never gets its
    own live ports back. Must be called with the registry lock held.
    """
    excluded = set()
    for name in os.listdir(registry):
        path = os.path.join(registry, name)
        if name.endswith(REGISTRATION_SUFFIX + TEMPORARY_SUFFIX):
            pid = _registration_pid(name, REGISTRATION_SUFFIX + TEMPORARY_SUFFIX)
            if pid is None or not _pid_alive(pid):
                _reap(path, "leftover temporary file of a dead or invalid pid")
            continue
        if not name.endswith(REGISTRATION_SUFFIX):
            continue
        pid = _registration_pid(name, REGISTRATION_SUFFIX)
        own = pid == own_pid
        if own and path not in _written_registrations:
            continue
        if pid is None:
            _reap(path, "the name does not start with a positive pid")
            continue
        if not own and not _pid_alive(pid):
            _remove(path)  # Its owner exited without cleaning up: the normal crash path.
            continue
        try:
            with open(path, encoding="utf-8") as registration:
                document = json.load(registration)
        except FileNotFoundError:
            continue
        except (OSError, ValueError):
            document = None
        if not _valid_registration(document):
            if not own:
                _reap(path, "not a registration (executable must be a string, ports distinct integers in 1..65535)")
            continue
        if not own:
            running = process_basename(pid)
            if running and running != document["executable"]:  # None or "": cannot tell, keep it.
                _reap(path, f"pid {pid} now runs {running!r}, not {document['executable']!r}")
                continue
        excluded.update(document["ports"])
    return excluded


def ephemeral_port_range():
    """Return the kernel's ephemeral port range as an inclusive (first, last) pair."""
    try:
        with open(LINUX_EPHEMERAL_RANGE_FILE, encoding="ascii") as port_range:
            first, last = (int(value) for value in port_range.read().split())
    except FileNotFoundError:
        output = subprocess.run(MACOS_EPHEMERAL_RANGE_COMMAND, check=True, capture_output=True,
                                text=True).stdout
        first, last = (int(value) for value in output.split())
    return first, last


def _probe(family, kind, port, socket_factory):
    """Bind one socket to ``port`` on the family's wildcard.

    Returns None when it bound, or the OSError when that error says something
    about the port: ``PORT_TAKEN_ERRNOS``, or for IPv6 ``IPV6_UNAVAILABLE_ERRNOS``.
    Any other OSError (EMFILE, ENOBUFS, ENOMEM, a seccomp EPERM, ...) says
    nothing about the port and is raised, so a lease fails with the real cause.
    """
    known = PORT_TAKEN_ERRNOS + (IPV6_UNAVAILABLE_ERRNOS if family == socket.AF_INET6 else ())
    address = "::" if family == socket.AF_INET6 else "0.0.0.0"
    try:
        probe = socket_factory(family, kind)
    except OSError as error:
        if family == socket.AF_INET6 and error.errno in IPV6_UNAVAILABLE_ERRNOS:
            return error
        raise
    with probe:
        try:
            if family == socket.AF_INET6:
                probe.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            probe.bind((address, port))
        except OSError as error:
            if error.errno in known:
                return error
            raise
    return None


def port_binds(port, socket_factory=socket.socket):
    """Return whether TCP and UDP sockets can bind ``port`` on the IPv4 and IPv6 wildcards.

    IPv6 failing with ``IPV6_UNAVAILABLE_ERRNOS`` (no usable IPv6 on the host)
    counts as free, as in Node's ``isIpv6PortFree``; ``PORT_TAKEN_ERRNOS`` mean
    taken. Any other OSError is raised (see :func:`_probe`).
    """
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        if _probe(socket.AF_INET, kind, port, socket_factory) is not None:
            return False
    if not socket.has_ipv6:
        return True
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        error = _probe(socket.AF_INET6, kind, port, socket_factory)
        if error is None:
            continue
        return error.errno in IPV6_UNAVAILABLE_ERRNOS
    return True


def candidate_blocks(count, excluded, ephemeral):
    """Return the start of every block of ``count`` consecutive ports a lease may take.

    A block lies entirely among the unprivileged ports outside ``excluded``, the
    reserved agave band and the ``ephemeral`` range. Blocks are cut back to back
    from the start of each run of such ports, so no two overlap.
    """
    starts = []
    run_start = None
    for port in range(LOWEST_LEASED_PORT, HIGHEST_PORT + 2):
        usable = (port <= HIGHEST_PORT
                  and not RESERVED_PORT_BAND[0] <= port <= RESERVED_PORT_BAND[1]
                  and not ephemeral[0] <= port <= ephemeral[1]
                  and port not in excluded)
        if usable and run_start is None:
            run_start = port
        elif not usable and run_start is not None:
            starts.extend(range(run_start, port - count + 1, count))
            run_start = None
    return starts


def ordered_blocks(count, excluded, ephemeral, rng=None):
    """Return candidate block starts (see :func:`candidate_blocks`) in the order a lease tries them.

    Random order, blocks above the reserved band before those below it.
    """
    rng = rng or random.Random()
    starts = candidate_blocks(count, excluded, ephemeral)
    preferred = [start for start in starts if start >= PREFERRED_LOWEST_PORT]
    fallback = [start for start in starts if start < PREFERRED_LOWEST_PORT]
    rng.shuffle(preferred)
    rng.shuffle(fallback)
    return preferred + fallback


def block_binds(block, pool, binds=None):
    """Return whether every port of ``block`` binds, checking them on ``pool``.

    Waits for every probe before answering, so no probe socket of this block
    is still open when the caller releases the block. ``binds`` defaults to
    :func:`port_binds`, looked up when called. If a probe raises, the others
    are still waited for before the exception propagates.
    """
    binds = binds or port_binds
    futures, results, error = [], [], None
    try:
        for port in block:
            futures.append(pool.submit(binds, port))
    except BaseException as raised:  # noqa: BLE001  e.g. "can't start new thread"; re-raised once submitted probes end.
        error = raised
    for future in futures:
        try:
            results.append(future.result())
        except BaseException as raised:  # noqa: BLE001  Re-raised below, once every probe has finished.
            error = error or raised
    if error is not None:
        raise error
    return all(results)


def registration_path(registry, pid):
    """Return the registration file of ``pid``."""
    return os.path.join(registry, f"{pid}{REGISTRATION_SUFFIX}")


def write_registration(path, ports, executable):
    """Write a registration atomically (temp file, then rename). Call with the registry lock held.

    Replaces whatever the file held; :func:`lease` decides whether ``ports``
    includes this process's earlier lease.
    """
    temporary = path + TEMPORARY_SUFFIX
    with open(temporary, "w", encoding="utf-8") as registration:
        json.dump({"executable": executable, "ports": ports}, registration)
    os.replace(temporary, path)


#: Registration files this process has written. The first write of a file
#: replaces it (an existing one can only be left by a dead process whose pid
#: was recycled to us); later writes add to it.
_written_registrations = set()


def lease(count=LEASE_PORT_COUNT, environ=None):
    """Lease ``count`` ports for this process.

    Returns ``(ports, registration_file)``. A second lease, from this or another
    thread, adds its ports to the same file. The caller deletes the file once
    nothing binds any of them.

    A block is claimed before it is probed: under the lock the registry is read
    afresh and the first candidate block disjoint from it is added to this
    process's registration; the bind checks then run outside the lock. A probe
    socket holds its port for a moment, and a probe of a port another process
    had leased could make that owner's own bind fail with EADDRINUSE, so no
    process probes a port it has not claimed.

    The lock is held for that read and claim only: the candidate order (a walk
    of every port) comes from a snapshot read before taking it, and a block
    that failed its probe is released in the same critical section that claims
    the next one. A block whose probe raises is released at once.
    """
    pid = os.getpid()
    registry = registry_dir(environ)
    path = registration_path(registry, pid)
    lock = lock_path(registry, environ)
    ephemeral = ephemeral_port_range()
    rejected = set()  # Ports of blocks this lease probed and found taken.
    claimed = None  # The block this lease registered and has not kept yet: being probed, or rejected.
    tried = 0
    with ThreadPoolExecutor(max_workers=BIND_CHECK_THREADS) as pool:
        try:
            while True:
                starts = ordered_blocks(count, _snapshot_exclusions(registry) | rejected, ephemeral)
                with RegistryLock(lock):
                    if claimed is not None:  # The block rejected last time: released with this claim.
                        _remove_ports_locked(path, claimed)
                        claimed = None
                    taken = read_exclusions(registry, pid) | rejected
                    start = next((start for start in starts if taken.isdisjoint(range(start, start + count))),
                                 None)
                    if start is None:
                        # The snapshot can name ports the locked read does not count (a recycled pid's file, a
                        # file this user may not delete): take the first block the locked read leaves free.
                        start = next(iter(ordered_blocks(count, taken, ephemeral)), None)
                    if start is None:
                        raise BindRegistryError(
                            f"no block of {count} consecutive ports is free outside the registry, the reserved band "
                            f"{RESERVED_PORT_BAND} and the ephemeral range {ephemeral} "
                            f"({tried} blocks probed and taken)")
                    claimed = list(range(start, start + count))  # Set first: an interrupt here still releases it.
                    _add_ports(path, claimed)
                if block_binds(claimed, pool):
                    block, claimed = claimed, None
                    return block, path
                tried += 1
                rejected.update(claimed)
        except BaseException as error:
            _release_after_failure(path, lock, claimed, error)
            raise


def _release_after_failure(path, lock, ports, error):
    """Give back ``ports`` (a block a failed lease still holds) without hiding ``error``, the failure itself.

    After a lock timeout the lock is not waited for again: the ports stay registered under this pid, and
    the next reader reclaims them once this process has exited. A failed release is reported, never raised.
    """
    if ports is None:
        return
    if isinstance(error, LockTimeoutError):
        _report(f"leaving ports {ports[0]}-{ports[-1]} registered: {error}")
        return
    try:
        _remove_ports(path, lock, ports)
    except Exception as release_error:  # noqa: BLE001  The original failure is the one to raise.
        _report(f"cannot release ports {ports[0]}-{ports[-1]}: {release_error}")


def _snapshot_exclusions(registry):
    """Return every port named in a registration file, read without the lock and without reaping.

    Only orders the candidates outside the lock; the claim re-checks under the
    lock (see :func:`lease`). Stale files only make the snapshot more cautious.
    """
    ports = set()
    for name in os.listdir(registry):
        if not name.endswith(REGISTRATION_SUFFIX):
            continue
        try:
            with open(os.path.join(registry, name), encoding="utf-8") as registration:
                document = json.load(registration)
        except (OSError, ValueError):
            continue  # Gone, or a file a reader will reap.
        if _valid_registration(document):
            ports.update(document["ports"])
    return ports


def _add_ports(path, ports):
    """Add ``ports`` to this process's registration. Call under the lock.

    The file is re-read here, so ports another thread of this process added
    meanwhile are kept. The first write of the file replaces whatever a dead
    process with a recycled pid left (see :data:`_written_registrations`).
    """
    current = [] if path not in _written_registrations else _registered_port_list(path)
    write_registration(path, current + [port for port in ports if port not in current], own_executable())
    _written_registrations.add(path)


def _remove_ports(path, lock, ports):
    """Remove ``ports`` from this process's registration, taking the lock (see :func:`_remove_ports_locked`)."""
    with RegistryLock(lock):
        _remove_ports_locked(path, ports)


def _remove_ports_locked(path, ports):
    """Remove ``ports`` from this process's registration. Call under the lock.

    Ports other leases of this process hold are kept. When nothing is left the
    file is deleted (the next claim of this process writes it afresh).
    """
    released = set(ports)
    remaining = [port for port in _registered_port_list(path) if port not in released]
    if remaining:
        write_registration(path, remaining, own_executable())
    else:
        _remove(path)
        _written_registrations.discard(path)


def invalid_requested_ports(ports):
    """Return why ``ports`` cannot be registered (repeats, out of 1..65535, the reserved band), or None."""
    if not ports:
        return "no ports"
    if len(set(ports)) != len(ports):
        return "a port is repeated"
    if not all(isinstance(port, int) and not isinstance(port, bool) and 0 < port <= HIGHEST_PORT for port in ports):
        return "a port is not an integer in 1..65535"
    reserved = sorted(port for port in ports if RESERVED_PORT_BAND[0] <= port <= RESERVED_PORT_BAND[1])
    if reserved:
        return f"ports {reserved} are in the reserved band {RESERVED_PORT_BAND}"
    return None


def register_ports(ports, environ=None, on_wait=None):
    """Add ``ports`` (in order) to this process's registration, refusing ports another process holds.

    The first registration of this process writes exactly ``ports``; a later
    one appends them to what the file already holds. Returns
    ``(registration_file, ports_others_hold)``. Raises :class:`PortsHeldError`
    when another live registration holds any of them, and
    :class:`BindRegistryError` for repeated ports, ports outside 1..65535 or in
    the reserved band (see :func:`invalid_requested_ports`). The ports are not
    probed. Used to re-claim a saved set of ports (``tools/cluster_manager.py
    run``) and by the ``--register`` command.
    """
    problem = invalid_requested_ports(list(ports))
    if problem is not None:
        raise BindRegistryError(f"cannot register {list(ports)}: {problem}")
    pid = os.getpid()
    registry = registry_dir(environ)
    path = registration_path(registry, pid)
    with RegistryLock(lock_path(registry, environ), on_wait=on_wait):
        held = read_exclusions(registry, pid)
        mine = set(_registered_port_list(path)) if path in _written_registrations else set()
        conflicts = sorted((held - mine).intersection(ports))
        if conflicts:
            raise PortsHeldError(conflicts)
        _add_ports(path, list(ports))
    return path, held - mine


class PortsHeldError(BindRegistryError):
    """Some requested ports are held by another live registration; ``ports`` lists them."""

    def __init__(self, ports):
        super().__init__(f"ports held by another live registration: {ports}")
        self.ports = ports


def _registered_port_list(path):
    """Return the ports a valid registration file holds, in file order (none when missing or invalid)."""
    try:
        with open(path, encoding="utf-8") as registration:
            document = json.load(registration)
    except (OSError, ValueError):
        return []
    return list(document["ports"]) if _valid_registration(document) else []


def format_lease(ports):
    """Return the ``SYSIO_TEST_PORTS`` spelling of a lease."""
    return ",".join(map(str, ports))


def parse_lease(value):
    """Parse a ``SYSIO_TEST_PORTS`` value: LEASE_PORT_COUNT consecutive valid ports."""
    try:
        ports = [int(field) for field in value.split(",")]
    except ValueError:
        raise BindRegistryError(f"{PORTS_ENV} is not a comma-separated port list: {value!r}") from None
    if len(ports) != LEASE_PORT_COUNT or not 0 < ports[0] <= HIGHEST_PORT - LEASE_PORT_COUNT + 1 \
            or ports != list(range(ports[0], ports[0] + LEASE_PORT_COUNT)):
        raise BindRegistryError(f"{PORTS_ENV} must hold {LEASE_PORT_COUNT} consecutive ports: {value!r}")
    return ports


_parsed_lease = (None, None)
_lease_lock = threading.Lock()


def ensure_ports():
    """Return this test's leased ports.

    Uses the lease in ``SYSIO_TEST_PORTS`` when a wrapper set it. Otherwise
    leases ports for this process on first use, exports them in
    ``SYSIO_TEST_PORTS`` so every program the test starts sees the same lease,
    and deletes the registration when this process exits. Thread-safe; the
    parsed lease is cached.
    """
    global _parsed_lease
    with _lease_lock:
        raw = os.environ.get(PORTS_ENV)
        if raw is None:
            ports, path = lease()
            atexit.register(release, path)
            raw = os.environ[PORTS_ENV] = format_lease(ports)
        if _parsed_lease[0] != raw:
            _parsed_lease = (raw, parse_lease(raw))
        return list(_parsed_lease[1])


#: Whether this wrapper already reported that it cannot list processes (reported once per run()).
_process_list_failure_reported = False


def _describe_session(sid):
    """Return session ``sid``'s members for a diagnostic, or a note that they cannot be listed."""
    members = session_members(sid)
    return members if members is not None else "(not listable; the test's process group)"


def session_members(sid):
    """Return the pids of the processes in session ``sid`` that are not zombies.

    Returns None when the processes cannot be listed (``ps`` failed without
    procfs); callers then fall back to the leader's process group.

    The wrapper starts each test as the leader of a new session, so this
    includes descendants that moved to another process group with setpgid.
    """
    global _process_list_failure_reported
    if os.path.isdir("/proc/self"):
        members = []
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                with open(f"/proc/{entry}/stat", encoding="utf-8", errors="replace") as stat:
                    # After "pid (comm)": state, ppid, pgrp, session, ...
                    state, _ppid, _pgrp, session = stat.read().rsplit(")", 1)[1].split()[:4]
            except (OSError, IndexError, ValueError):
                continue  # Exited while being read.
            if state != "Z" and int(session) == sid:
                members.append(int(entry))
        return members
    # No procfs (macOS): ps lists pids and states; getsid gives the session. ps's `sess`
    # column is a kernel pointer there, not a session id.
    try:
        rows = subprocess.run(["ps", "-A", "-o", "pid=,stat="], check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        if not _process_list_failure_reported:
            _process_list_failure_reported = True
            _report(f"cannot list processes ({error}); watching and signalling the test's process group instead")
        return None
    members = []
    for pid, state in (row.split()[:2] for row in rows.splitlines() if row.strip()):
        if state.startswith("Z") or _getsid(int(pid)) != sid:
            continue
        members.append(int(pid))
    return members


def _getsid(pid):
    """Return the session id of ``pid``, or None when it is gone or not visible."""
    try:
        return os.getsid(pid)
    except (ProcessLookupError, PermissionError):
        return None


def _process_session(pid):
    """Return the session id of ``pid`` from procfs, or None when it cannot be read."""
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8", errors="replace") as stat:
            return int(stat.read().rsplit(")", 1)[1].split()[3])
    except (OSError, IndexError, ValueError):
        return None


def _signal_member(pid, sid, signum):
    """Send ``signum`` to ``pid`` only if it is still a member of session ``sid``.

    Guards against the pid being reused between the scan and the kill. With
    pidfds (Linux 5.3+) the process is pinned first, then its session checked;
    a pidfd of a process that has since exited cannot signal anything else.
    Otherwise the session is re-read (procfs, or ``getsid``) just before
    ``os.kill``. A process this user may not signal (EPERM) is reported and skipped.
    """
    try:
        if os.path.isdir("/proc/self") and hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
            try:
                pidfd = os.pidfd_open(pid)
            except ProcessLookupError:
                return
            except OSError:
                pidfd = None  # Kernel without pidfd support: fall through to the re-read check.
            if pidfd is not None:
                try:
                    if _process_session(pid) == sid:
                        signal.pidfd_send_signal(pidfd, signum)
                finally:
                    os.close(pidfd)
                return
        current = _process_session(pid) if os.path.isdir("/proc/self") else _getsid(pid)
        if current == sid:
            os.kill(pid, signum)
    except ProcessLookupError:
        pass
    except PermissionError as error:
        _report(f"cannot signal {pid}: {error}")


def _signal_session(sid, signum):
    """Send ``signum`` once to every member of session ``sid``; return the members signalled."""
    members = session_members(sid)
    if members is None:  # Cannot see the session: signal what can be reached, the leader's process group.
        try:
            os.killpg(sid, signum)
        except (ProcessLookupError, PermissionError):
            pass
        return []
    for pid in members:
        try:
            _signal_member(pid, sid, signum)
        except PermissionError as error:  # One member we may not signal must not stop the others.
            _report(f"cannot signal {pid}: {error}")
    return members


def _session_alive(sid):
    """Return whether session ``sid`` has members; without a process list, whether its leader's group exists."""
    members = session_members(sid)
    if members is not None:
        return bool(members)
    try:
        os.killpg(sid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _wait_for_session_exit(sid, seconds):
    """Return True once session ``sid`` is empty, False if members remain after ``seconds``."""
    deadline = time.monotonic() + seconds
    while _session_alive(sid):
        if time.monotonic() >= deadline:
            return False
        time.sleep(GROUP_POLL_SECONDS)
    return True


def _report(message):
    """Write a wrapper diagnostic to stderr, best effort.

    stderr can be gone (ctest interrupted): a diagnostic must never stop the
    wrapper from signalling or reaping the test.
    """
    try:
        print(f"bind_registry: {message}", file=sys.stderr, flush=True)
    except (OSError, ValueError):
        pass


def drain_session(sid, drain=GROUP_DRAIN_SECONDS, term=GROUP_TERM_SECONDS, kill=GROUP_KILL_SECONDS):
    """Wait for session ``sid`` to exit, escalating to SIGTERM and then SIGKILL.

    Returns False when members are still alive ``kill`` seconds after SIGKILL.
    """
    if _wait_for_session_exit(sid, drain):
        return True
    left = _signal_session(sid, signal.SIGTERM) or _describe_session(sid)  # Signal first: the report may fail.
    _report(f"the test left processes {left} running; sent SIGTERM")
    if _wait_for_session_exit(sid, term):
        return True
    left = _signal_session(sid, signal.SIGKILL) or _describe_session(sid)
    _report(f"processes {left} ignored SIGTERM; sent SIGKILL")
    return _wait_for_session_exit(sid, kill)


def exit_status(returncode):
    """Map a ``Popen.returncode`` to a shell exit status (128 + N for death by signal N)."""
    return SIGNAL_STATUS_BASE - returncode if returncode < 0 else returncode


def run(command, drain=GROUP_DRAIN_SECONDS, term=GROUP_TERM_SECONDS, kill=GROUP_KILL_SECONDS):
    """Lease ports, run ``command`` in its own session and return its exit status.

    Signals in ``FORWARDED_SIGNALS`` delivered to this process while it runs are
    forwarded to the session. The registration is deleted only after the whole
    session exited. Returns ``WRAPPER_FAILURE_STATUS`` (or the test's own failing
    status) when the lease fails or the session outlives SIGKILL.
    """
    global _process_list_failure_reported
    _process_list_failure_reported = False
    pending = []  # Signals received before the test started; forwarded once it has.
    child = None

    def forward(signum, _frame):
        if child is None:
            pending.append(signum)
            return
        try:
            _signal_session(child.pid, signum)
        except Exception as error:  # noqa: BLE001  A failed forward must not abort the wait and the drain.
            _report(f"cannot forward signal {signum}: {error}")

    previous = {signum: signal.signal(signum, forward) for signum in FORWARDED_SIGNALS}
    try:
        try:
            ports, path = lease()
        except (BindRegistryError, OSError, subprocess.CalledProcessError) as error:
            _report(f"cannot lease {LEASE_PORT_COUNT} ports: {error}")
            return WRAPPER_FAILURE_STATUS
        if pending:
            _remove(path)
            return SIGNAL_STATUS_BASE + pending[0]

        try:
            started = subprocess.Popen(command, env={**os.environ, PORTS_ENV: format_lease(ports)},
                                       start_new_session=True)
        except OSError as error:
            _remove(path)
            _report(f"cannot start {command[0]}: {error}")
            return WRAPPER_FAILURE_STATUS
        child = started  # From here on the handler forwards directly and appends nothing.
        for signum in list(pending):  # Delivered while Popen was starting the test.
            _signal_session(child.pid, signum)

        status = exit_status(child.wait())
        if not drain_session(child.pid, drain, term, kill):
            # The ports may still be bound, so keep the registration; the next
            # registry reader reclaims it once this wrapper has exited.
            _report(f"processes {_describe_session(child.pid)} of the test's session survived SIGKILL "
                    f"{kill} s; leaving {path} in place")
            return status or WRAPPER_FAILURE_STATUS
        _remove(path)
        return status
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def register(argv):
    """Register exactly the given ports, in the order given, hold them until stdin closes, then release them.

    Runs in the calling process, which therefore owns the registration (its pid
    names the file); ``tests/bind_registry_register.py`` is the executable form.

    The command-line contract of wire-tools-ts'
    ``packages/cluster-tool/tests/fixtures/bind-registry/register_ports.py``, so
    its interop suite can drive this client (``WIRE_BIND_REGISTRY_PYTHON_CLIENT``).
    Prints ``waiting-for-lock`` when the lock is held at the first attempt, then
    one JSON line ``{"pid", "executable", "registrationFile", "lockDirectory",
    "registeredPorts"}`` (ports other live registrations hold). Exit codes:
    0 released on stdin EOF, 2 usage, 3 a requested port is held by another
    live registration, 4 the lock was not taken within the bounded wait.
    """
    try:
        requested = [int(argument) for argument in argv]
    except ValueError:
        requested = []
    problem = invalid_requested_ports(requested)
    if problem is not None:
        _report(f"{problem}; usage: bind_registry_register.py <port>...  (or bind_registry.py --register <port>...)")
        return USAGE_STATUS
    try:
        path, held = register_ports(requested, on_wait=lambda: print(WAITING_FOR_LOCK_LINE, flush=True))
    except PortsHeldError as error:
        _report(str(error))
        return REGISTER_PORT_HELD_STATUS
    except BindRegistryError as error:
        _report(str(error))
        return REGISTER_LOCK_TIMEOUT_STATUS
    print(json.dumps({"pid": os.getpid(), "executable": own_executable(), "registrationFile": path,
                      "lockDirectory": lock_path(registry_dir()), "registeredPorts": sorted(held)}), flush=True)
    sys.stdin.read()
    _remove(path)
    return 0


def main(argv):
    """Entry point: ``bind_registry.py <test command> [args...]`` or ``bind_registry.py --register <port>...``."""
    if argv and argv[0] == REGISTER_COMMAND:
        return register(argv[1:])
    if not argv:
        _report(f"usage: {os.path.basename(sys.argv[0])} <test command> [args...]")
        return USAGE_STATUS
    return run(argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
