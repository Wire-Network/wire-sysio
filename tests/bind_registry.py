"""Run native network tests under the canonical registry's Node custodian."""
import atexit
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

try:
    from bind_registry_config import SOURCE_ROOT
except ImportError:
    SOURCE_ROOT = str(Path(__file__).resolve().parent.parent)

_issuer = None


def ensure_ports():
    raw = os.environ.get("SYSIO_TEST_PORTS")
    if raw is None:
        raise RuntimeError("Run network tests through python3 tests/bind_registry.py <test command>; install root pnpm dependencies and use Node 24")
    ports = [int(port) for port in raw.split(",")]
    if len(ports) != 192 or len(set(ports)) != 192 or any(not 0 < port < 65536 for port in ports):
        raise RuntimeError("Invalid BindConfigProvider native-test lease")
    os.kill(int(os.environ["SYSIO_TEST_PORT_ISSUER_PID"]), 0)
    return ports


def _release():
    if _issuer is not None:
        if not _issuer.stdin.closed:
            _issuer.stdin.close()
        # Never kill the custodian: it retains the lease until its entire test
        # process group has stopped, even if the Python wrapper is killed.
        _issuer.wait(timeout=60)


if __name__ == "__main__":
    def interrupted(signum, frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    _issuer = subprocess.Popen(
        ["node", str(Path(__file__).with_name("bind_ports.cjs")), SOURCE_ROOT, *sys.argv[1:]],
        stdin=subprocess.PIPE, text=True,
        start_new_session=True)
    atexit.register(_release)
    raise SystemExit(_issuer.wait())
