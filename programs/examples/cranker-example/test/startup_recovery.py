"""Verify a real cranker survives pending discovery and later reads gas prices.

Run through tests/run_with_bind_config.cjs; every endpoint uses its held ports.
Run directories and logs are retained for diagnostics. Only owned children stop.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TIMEOUT_SECONDS = 25
POLL_SECONDS = 0.1
CHAIN_ID = 31337
GAS_PRICE = 42
# Public Anvil fixture, also used by the native Ethereum client regression tests.
SIGNER = (
    "signer-a,ethereum,ethereum,"
    "0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed753547f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5,"
    "KEY:0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"
)


class RpcHandler(BaseHTTPRequestHandler):
    """Answer only discovery and gas-price requests; record actual consumption."""

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        method = request["method"]
        self.server.methods.append(method)
        values = {"eth_chainId": hex(CHAIN_ID), "eth_gasPrice": hex(GAS_PRICE)}
        payload = {"jsonrpc": "2.0", "id": request["id"]}
        if method in values:
            payload["result"] = values[method]
        else:
            payload["error"] = {"code": -32601, "message": "unsupported test method"}
        encoded = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, *_args):
        """Keep RPC fixture traffic out of test output."""


def wait_for(process, log_path, message):
    """Observe real callback output while requiring the process to stay alive."""
    deadline = time.monotonic() + TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        assert process.poll() is None, f"cranker exited ({process.returncode}); inspect {log_path}"
        if message in log_path.read_text():
            return
        time.sleep(POLL_SECONDS)
    raise AssertionError(f"missing {message!r}; inspect {log_path}")


def run_case(binary, source, root, port, deferred):
    """Test immutable pending snapshots and configured-ID startup separately."""
    case = root / ("deferred" if deferred else "configured")
    case.mkdir(mode=0o700)
    log_path = case / "cranker.log"
    endpoint = f"http://127.0.0.1:{port}"
    client = f"ETHEREUM,signer-a,{endpoint}" + ("" if deferred else f",{CHAIN_ID}")
    command = [str(binary), "--data-dir", str(case / "data"), "--config-dir", str(case / "config"),
               "--signature-provider", SIGNER, "--outpost-ethereum-client", client,
               "--ethereum-abi-file", str(source / "tests/fixtures/ethereum-abi-counter-01.json")]
    server = None
    thread = None
    process = None
    try:
        with log_path.open("w") as output:
            if deferred:
                process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
                wait_for(process, log_path, "pending discovery; retrying on the next tick")
            server = ThreadingHTTPServer(("127.0.0.1", port), RpcHandler)
            server.methods = []
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            if process is None:
                process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
            wait_for(process, log_path, f"Current Price> {GAS_PRICE}WEI")
            assert "eth_gasPrice" in server.methods
            assert ("eth_chainId" in server.methods) == deferred
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=TIMEOUT_SECONDS) == 0
        print(f"{case.name}: gas price observed; clean shutdown")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=TIMEOUT_SECONDS)
        if server is not None:
            if thread is not None:
                server.shutdown()
                thread.join(timeout=TIMEOUT_SECONDS)
            server.server_close()


def main():
    """Require explicit binaries, source fixtures and a fresh evidence directory."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    ports = [int(value) for value in os.environ["WIRE_TEST_PORTS"].split()]
    assert len(ports) >= 2, "run through run_with_bind_config.cjs"
    args.output_root.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="cranker-startup-", dir=args.output_root)).resolve()
    print(f"Evidence directory: {output}", flush=True)
    for index, deferred in enumerate((True, False)):
        run_case(args.binary.resolve(), args.source.resolve(), output, ports[index], deferred)


if __name__ == "__main__":
    main()
