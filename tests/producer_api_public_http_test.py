#!/usr/bin/env python3
"""Verify producer administrative APIs cannot bind to non-loopback HTTP listeners."""

import shutil
import subprocess
from pathlib import Path

from TestHarness import TestHelper, Utils


SYSIO_PUBLIC_KEY = "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"
SYSIO_PRIVATE_KEY = "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"
EXPECTED_PRODUCER_RW_ERROR = "producer_api_plugin refuses to expose the producer_rw API category"
EXPECTED_SNAPSHOT_ERROR = "producer_api_plugin refuses to expose the snapshot API category"


def expect_nodeop_rejects_public_admin_listener(node_num, http_args, expected_error):
    """Start nodeop and require producer_api_plugin to reject the supplied HTTP listener configuration."""
    data_dir = Path(Utils.getNodeDataDir(node_num))
    config_dir = Path(Utils.getNodeConfigDir(node_num))
    data_dir.mkdir(parents=True)
    config_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        Utils.SysServerPath,
        "-e",
        "-p",
        "sysio",
        "--data-dir",
        str(data_dir),
        "--config-dir",
        str(config_dir),
        "--plugin",
        "sysio::http_plugin",
        "--plugin",
        "sysio::producer_api_plugin",
        "--signature-provider",
        f"wire-1,wire,wire,{SYSIO_PUBLIC_KEY},KEY:{SYSIO_PRIVATE_KEY}",
        "--resource-monitor-not-shutdown-on-threshold-exceeded",
    ]
    cmd.extend(http_args)

    try:
        completed = subprocess.run(cmd, capture_output=True, encoding="utf-8", timeout=30, check=False)
    except subprocess.TimeoutExpired as ex:
        Utils.Print("nodeop kept running with a public producer API HTTP listener")
        Utils.Print((ex.stdout or "") + (ex.stderr or ""))
        Utils.errorExit("nodeop did not reject public producer API listener during initialization")

    combined_output = completed.stdout + completed.stderr

    if completed.returncode == 0:
        Utils.errorExit("nodeop unexpectedly accepted a public producer API HTTP listener")

    if expected_error not in combined_output:
        Utils.Print("nodeop output did not contain expected producer API guard message")
        Utils.Print(combined_output)
        Utils.errorExit("missing producer API public-listener rejection")


args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v"})
Utils.Debug = args.v
test_successful = False
data_root = Path(Utils.DataPath)

try:
    if data_root.exists():
        shutil.rmtree(data_root)

    public_http_address = f"0.0.0.0:{Utils.getPort(Utils.PortNodeHttp, 12)}"
    expect_nodeop_rejects_public_admin_listener(
        0, ["--http-server-address", public_http_address], EXPECTED_PRODUCER_RW_ERROR
    )

    public_snapshot_address = f"snapshot,0.0.0.0:{Utils.getPort(Utils.PortNodeHttp, 13)}"
    expect_nodeop_rejects_public_admin_listener(
        1,
        ["--http-server-address", "http-category-address", "--http-category-address", public_snapshot_address],
        EXPECTED_SNAPSHOT_ERROR,
    )

    test_successful = True
finally:
    if not args.keep_logs and data_root.exists():
        shutil.rmtree(data_root)

exit(0 if test_successful else 1)
