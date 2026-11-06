#!/usr/bin/env python3
"""Stress test for tests/bind_registry.py: many concurrent wrappers lease disjoint, bindable ports.

Starts CONCURRENT_LEASES wrappers at once against the HOST registry (the one every
test and wire-tools-ts cluster uses), so the ports it binds are ones nobody else
was handed. Each test command binds every leased port on TCP and UDP and holds
them until released; the run fails if two leases overlap, any port fails to bind,
or one of this run's registrations survives its wrapper. CTest runs it RUN_SERIAL.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from bind_registry import LEASE_PORT_COUNT, PORTS_ENV, RESERVED_PORT_BAND, registration_path, registry_dir

CONCURRENT_LEASES = 32
# Generous: each lease bind-checks hundreds of sockets, slow on a loaded WSL2 host.
READY_TIMEOUT_SECONDS = 1200
EXIT_TIMEOUT_SECONDS = 120
POLL_SECONDS = 0.1
WRAPPER = str(Path(__file__).with_name("bind_registry.py"))
TESTS_DIR = str(Path(__file__).resolve().parent)
# Descriptors a holder needs beyond its two sockets per leased port (stdio, the interpreter's own files).
DESCRIPTOR_HEADROOM = 64


def raise_descriptor_limit(needed, resource_module=None):
    """Raise the soft RLIMIT_NOFILE so ``needed`` descriptors can be open; exit with a message when it cannot.

    Two sockets per leased port exceed macOS's default soft limit of 256. The soft limit goes up to the hard
    one; macOS refuses an unlimited soft limit, so with an unlimited hard limit only ``needed`` is asked for.
    """
    import resource
    resource = resource_module or resource
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft == resource.RLIM_INFINITY or soft >= needed:
        return
    wanted = needed if hard == resource.RLIM_INFINITY else hard
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (wanted, hard))
    except (ValueError, OSError) as error:
        raise SystemExit(f"cannot raise RLIMIT_NOFILE from {soft} to {wanted} (hard {hard}): {error}") from error
    soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft != resource.RLIM_INFINITY and soft < needed:
        raise SystemExit(f"a holder needs {needed} descriptors but RLIMIT_NOFILE allows only {soft} (hard {hard})")

# Bind every leased port (TCP listening and UDP) on loopback, report them, then hold until released.
HOLDER = (r'''
import json, os, socket, sys, time
from pathlib import Path
sys.path.insert(0, sys.argv[4])
from bind_registry_stress_test import raise_descriptor_limit
ports = [int(port) for port in os.environ[sys.argv[3]].split(",")]
raise_descriptor_limit(2 * len(ports) + DESCRIPTOR_HEADROOM)
held = []
for port in ports:
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        listener = socket.socket(socket.AF_INET, kind)
        listener.bind(("127.0.0.1", port))
        if kind == socket.SOCK_STREAM:
            listener.listen(1)
        held.append(listener)
report = Path(sys.argv[1])
report.with_suffix(".tmp").write_text(json.dumps({"ports": ports, "wrapper": os.getppid()}))
report.with_suffix(".tmp").rename(report)
while not Path(sys.argv[2]).exists():
    time.sleep(0.1)
''').replace("DESCRIPTOR_HEADROOM", str(DESCRIPTOR_HEADROOM))


def main():
    with tempfile.TemporaryDirectory(prefix="bind-registry-stress-") as scratch:
        root = Path(scratch)
        registry = registry_dir()
        release = root / "release"
        reports = [root / f"{index}.json" for index in range(CONCURRENT_LEASES)]
        env = {key: value for key, value in os.environ.items() if key != PORTS_ENV}
        started = time.monotonic()
        wrappers = [subprocess.Popen([sys.executable, WRAPPER, sys.executable, "-c", HOLDER, str(report),
                                      str(release), PORTS_ENV, TESTS_DIR], env=env)
                    for report in reports]
        try:
            while not all(report.exists() for report in reports):
                failed = [wrapper.returncode for wrapper in wrappers if wrapper.poll() is not None]
                if failed:
                    raise AssertionError(f"a wrapper exited before every lease was held: {failed}")
                if time.monotonic() - started > READY_TIMEOUT_SECONDS:
                    raise AssertionError(f"{CONCURRENT_LEASES} leases were not held within {READY_TIMEOUT_SECONDS} s")
                time.sleep(POLL_SECONDS)
            elapsed = time.monotonic() - started
            held = [json.loads(report.read_text()) for report in reports]
            claimed = [port for lease in held for port in lease["ports"]]
            assert len(claimed) == len(set(claimed)) == CONCURRENT_LEASES * LEASE_PORT_COUNT, "leases overlap"
            assert not any(RESERVED_PORT_BAND[0] <= port <= RESERVED_PORT_BAND[1] for port in claimed)
            for lease in held:
                registration = json.loads(Path(registration_path(registry, lease["wrapper"])).read_text())
                assert registration["ports"] == lease["ports"], "registration differs from the lease"
            release.touch()
            statuses = [wrapper.wait(timeout=EXIT_TIMEOUT_SECONDS) for wrapper in wrappers]
            assert statuses == [0] * CONCURRENT_LEASES, statuses
            left = [lease["wrapper"] for lease in held if os.path.exists(registration_path(registry, lease["wrapper"]))]
            assert not left, f"registrations of wrappers {left} survived them"
            print(f"{CONCURRENT_LEASES} concurrent leases, {len(claimed)} distinct ports bound on TCP and UDP "
                  f"within {elapsed:.1f} s; every registration released")
        finally:
            release.touch()
            for wrapper in wrappers:
                if wrapper.poll() is None:
                    wrapper.terminate()
                    wrapper.wait(timeout=EXIT_TIMEOUT_SECONDS)


if __name__ == "__main__":
    main()
