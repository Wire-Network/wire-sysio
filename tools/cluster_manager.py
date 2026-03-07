#!/usr/bin/env python3

"""
cluster_manager.py - Create, run, and manage a local Wire chain cluster.

Usage:
    ./tools/cluster_manager.py [--chain-dir /opt/wire-chains/dev-001] create --build-dir build/claude
    ./tools/cluster_manager.py [--chain-dir /opt/wire-chains/dev-001] run
    ./tools/cluster_manager.py [--chain-dir /opt/wire-chains/dev-001] stop
"""

from __future__ import annotations

import copy
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import click

UTC = timezone.utc

# TestHarness imports are deferred until _setup_harness() is called with the
# build directory, since the package requires cmake-generated files
# (e.g. core_symbol.py) that only exist under <build-dir>/tests/.
Utils: Any = None
Node: Any = None
WalletMgr: Any = None
cluster_generator: Any = None
Account: Any = None
CORE_SYMBOL: Any = None


def _setup_harness(build_dir: Path) -> None:
    """Import TestHarness from <build-dir>/tests/ and bind module globals."""
    global Utils, Node, WalletMgr, cluster_generator, Account, CORE_SYMBOL  # noqa: PLW0603

    tests_dir = build_dir / "tests"
    harness_marker = tests_dir / "TestHarness" / "core_symbol.py"
    if not harness_marker.exists():
        raise click.ClickException(
            f"TestHarness not found at {tests_dir}/TestHarness/. "
            f"Ensure --build-dir points to a completed cmake build."
        )

    sys.path.insert(0, str(tests_dir))

    from TestHarness.testUtils import Utils as _Utils  # pyright: ignore[reportMissingImports]
    from TestHarness.Node import Node as _Node  # pyright: ignore[reportMissingImports]
    from TestHarness.WalletMgr import WalletMgr as _WalletMgr  # pyright: ignore[reportMissingImports]
    from TestHarness.launcher import cluster_generator as _cg  # pyright: ignore[reportMissingImports]
    from TestHarness.accounts import Account as _Account  # pyright: ignore[reportMissingImports]
    from TestHarness.core_symbol import CORE_SYMBOL as _CS  # pyright: ignore[reportMissingImports]

    Utils = _Utils
    Node = _Node
    WalletMgr = _WalletMgr
    cluster_generator = _cg
    Account = _Account
    CORE_SYMBOL = _CS  # pyright: ignore[reportConstantRedefinition]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

PID_FILENAME = ".pid"


def _pid_file(chain_dir: Path) -> Path:
    return chain_dir / PID_FILENAME


def _acquire_lock(chain_dir: Path) -> None:
    """Write our PID to .pid; abort if another process already holds it."""
    pf = _pid_file(chain_dir)
    if pf.exists():
        try:
            existing_pid = int(pf.read_text().strip())
            # Check whether that process is still alive
            os.kill(existing_pid, 0)
            raise click.ClickException(
                f"Another cluster_manager (pid {existing_pid}) is already running "
                f"on chain-dir {chain_dir}. Use 'stop' to terminate it first."
            )
        except (ProcessLookupError, ValueError):
            # stale pid file - safe to overwrite
            pass
    pf.write_text(str(os.getpid()))


def _release_lock(chain_dir: Path) -> None:
    pf = _pid_file(chain_dir)
    if pf.exists():
        try:
            stored = int(pf.read_text().strip())
            if stored == os.getpid():
                pf.unlink()
        except (ValueError, OSError):
            pass


def _echo(msg: str) -> None:
    click.echo(f"[cluster_manager] {msg}")


def _ensure_chain_dir(chain_dir: Path, force: bool) -> None:
    """Prepare the chain-dir for a fresh 'create'.

    If the directory exists and is non-empty:
      - with --force: clear it
      - without --force: error
    """
    if chain_dir.exists():
        contents = list(chain_dir.iterdir())
        if contents:
            if not force:
                raise click.ClickException(
                    f"chain-dir {chain_dir} exists and is not empty. "
                    f"Use --force to overwrite."
                )
            _echo(f"--force: clearing {chain_dir}")
            for item in contents:
                if item.is_dir():
                    shutil.rmtree(item)
                else:
                    item.unlink()
    chain_dir.mkdir(parents=True, exist_ok=True)


def _subdirs(chain_dir: Path) -> dict[str, Path]:
    """Return the canonical sub-directory paths.

    Layout:
      data/          Per-node dirs (node_bios/, node_00/, ...) with config,
                     chain data, and logs co-located (TestHarness convention).
      wallet/        kiod wallet files.
    """
    return {
        "data": chain_dir / "data",
        "wallet": chain_dir / "wallet",
    }


def _state_file(chain_dir: Path) -> Path:
    return chain_dir / ".cluster_state.json"


def _save_state(chain_dir: Path, state: dict[str, Any]) -> None:
    _state_file(chain_dir).write_text(json.dumps(state, indent=2))


def _load_state(chain_dir: Path) -> dict[str, Any]:
    sf = _state_file(chain_dir)
    if not sf.exists():
        raise click.ClickException(
            f"No cluster state found in {chain_dir}. Run 'create' first."
        )
    return json.loads(sf.read_text())


# ---------------------------------------------------------------------------
# Cluster creation - mirrors Cluster.launch() + bootstrap()
# ---------------------------------------------------------------------------


_HTTP_INSECURE_CONFIG = """\

# -- http-insecure settings (cluster_manager) --
# Specify the Access-Control-Allow-Origin to be returned on each request (sysio::http_plugin)
access-control-allow-origin = *
# Specify the Access-Control-Allow-Headers to be returned on each request (sysio::http_plugin)
access-control-allow-headers = *
# Append the error log to HTTP responses (sysio::http_plugin)
verbose-http-errors = true
# If set to false, then any incoming "Host" header is considered valid (sysio::http_plugin)
http-validate-host = false
"""


def _patch_configs_http_insecure(data_path: Path) -> None:
    """Append HTTP insecure settings to every config.ini under data_path."""
    for config_ini in data_path.glob("*/config.ini"):
        _echo(f"  Patching {config_ini.name} with --http-insecure settings ({config_ini.parent.name})")
        with config_ini.open("a") as f:
            f.write(_HTTP_INSECURE_CONFIG)


def _create_cluster(
    chain_dir: Path,
    build_dir: Path,
    pnodes: int,
    total_nodes: int,
    prod_count: int,
    topo: str,
    http_insecure: bool = False,
) -> None:
    """Generate configs, start nodes, bootstrap, then shut down."""
    delay = 2
    activate_if = True
    dirs = _subdirs(chain_dir)
    for d in dirs.values():
        d.mkdir(parents=True, exist_ok=True)

    data_path = dirs["data"]
    wallet_path = dirs["wallet"]

    # -- Set up the Utils paths so the TestHarness writes into our chain-dir --
    # The launcher places node directories (node_bios/, node_00/, etc.) under
    # the path given via --config-dir / --data-dir.  parseClusterKeys() then
    # looks for start.cmd files via Utils.DataDir.  We point everything at
    # data_path so all paths stay consistent.
    Utils.TestLogRoot = str(data_path)
    Utils.DataPath = str(data_path)
    Utils.DataDir = str(data_path) + "/"
    Utils.ConfigDir = str(data_path) + "/"
    Utils.checkOutputFilename = str(data_path / "subprocess_results.log")

    # WalletMgr class variables are evaluated at import time from the original
    # Utils.DataPath.  Override them so wallet data lands in <chain-dir>/wallet/.
    WalletMgr._WalletMgr__walletDataDir = str(wallet_path)
    WalletMgr._WalletMgr__walletLogOutFile = str(wallet_path / "kiod_out.log")
    WalletMgr._WalletMgr__walletLogErrFile = str(wallet_path / "kiod_err.log")

    maximum_p2p_per_host = total_nodes

    # Build launcher args (mirrors Cluster.launch)
    launch_time = datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3]
    args_str = (
        f"-p {pnodes} -n {total_nodes} -d {delay} "
        f"--producers {prod_count} "
        f"-i {launch_time} -f "
        f"--unstarted-nodes 0 --logging-level debug "
        f"--logging-level-map {{}}"
    )
    args_arr = args_str.split()
    args_arr.extend(["--config-dir", str(data_path)])
    args_arr.extend(["--data-dir", str(data_path)])

    nodeop_args = (
        " --vote-threads 4"
        " --max-transaction-time -1"
        " --abi-serializer-max-time-ms 990000"
        f" --p2p-max-nodes-per-host {maximum_p2p_per_host}"
        " --max-clients 25"
        " --connection-cleanup-period 15"
        " --contracts-console"
        " --plugin sysio::producer_api_plugin"
        " --plugin sysio::trace_api_plugin"
        " --trace-no-abis"
        " --http-max-response-time-ms 990000"
    )
    args_arr.extend(["--nodeop", nodeop_args])
    args_arr.extend(["--max-block-cpu-usage", "400000"])
    args_arr.extend(["--max-transaction-cpu-usage", "375000"])
    args_arr.extend(["--shape", topo])

    # Generate the network with the launcher
    launcher = cluster_generator(args_arr)
    launcher.define_network()
    launcher.generate()

    # --- Start all nodes ---
    bios_node = None
    nodes: list[Any] = []

    for instance in launcher.network.nodes.values():
        cmd = launcher.construct_command_line(instance)
        node_num = instance.index
        node = Node(
            "localhost",
            8888 + node_num,
            node_num,
            Path(instance.data_dir_name),
            Path(instance.config_dir_name),
            cmd,
            unstarted=instance.dont_start,
            launch_time=launcher.launch_time,
            walletMgr=None,
            nodeopVers="",
        )
        node.keys = instance.keys
        node.isProducer = len(instance.producers) > 0
        node.producerName = instance.producers[0] if node.isProducer else None
        if node_num == Node.biosNodeId:
            bios_node = node
        elif node.popenProc:
            nodes.append(node)
        time.sleep(delay)

    if not bios_node:
        raise click.ClickException("Bios node failed to start.")

    _echo("Waiting for bios node to be responsive...")
    if not Utils.waitForBool(bios_node.checkPulse, Utils.systemWaitTimeout):
        raise click.ClickException("Bios node not responding.")

    _echo("Waiting for cluster sync (block 1)...")
    all_nodes = nodes + [bios_node]
    for n in all_nodes:
        if n.pid and not n.waitForBlock(1, timeout=Utils.systemWaitTimeout):
            raise click.ClickException(f"Node {n.nodeId} failed to reach block 1.")

    # --- Bootstrap ---
    _echo("Bootstrapping cluster...")
    _bootstrap(build_dir, bios_node, nodes, total_nodes, prod_count, activate_if)

    # --- Kill bios node (it is not needed after bootstrap) ---
    _echo("Killing bios node (not needed after bootstrap)...")
    bios_node.kill(signal.SIGTERM)

    # --- Persist state for 'run' (bios excluded) ---
    node_states = []
    for n in nodes:
        node_states.append(
            {
                "nodeId": n.nodeId,
                "host": n.host,
                "port": n.port,
                "data_dir": str(n.data_dir),
                "config_dir": str(n.config_dir),
                "cmd": n.cmd,
                "isProducer": n.isProducer,
                "producerName": n.producerName,
            }
        )

    _save_state(
        chain_dir,
        {
            "pnodes": pnodes,
            "total_nodes": total_nodes,
            "prod_count": prod_count,
            "topo": topo,
            "nodes": node_states,
        },
    )

    # --- Shut down remaining nodes cleanly ---
    _echo("Shutting down nodes after bootstrap...")
    for n in nodes:
        n.kill(signal.SIGTERM)

    # Patch config.ini files with permissive HTTP settings (takes effect on next 'run')
    if http_insecure:
        _patch_configs_http_insecure(data_path)

    _echo("Cluster created and bootstrapped successfully.")
    _echo(f"Chain directory: {chain_dir}")


def _bootstrap(  # noqa: C901, PLR0912, PLR0915
    build_dir: Path,
    bios_node: Any,
    nodes: list[Any],
    total_nodes: int,
    prod_count: int,
    activate_if: bool,
) -> None:
    """Deploy system contracts, create accounts, issue tokens - mirrors Cluster.bootstrap().

    build_dir is the build directory root (e.g. build/claude).  Expected layout:
      build_dir/
        libraries/testing/contracts/   (sysio.bios, sysio.roa)
        contracts/                     (sysio.system, sysio.token)
        unittests/test-contracts/      (noop - optional)
    """
    lib_testing_contracts = build_dir / "libraries" / "testing" / "contracts"
    contracts_path = build_dir / "contracts"
    unittests_test_contracts = build_dir / "unittests" / "test-contracts"

    # Parse producer keys from start files
    producer_keys = _parse_cluster_keys(total_nodes)
    if producer_keys is None or len(producer_keys) < 2:
        raise click.ClickException("Failed to parse producer keys from start files.")

    # Start wallet manager (keepLogs=True preserves wallet data on shutdown)
    wallet_mgr = WalletMgr(True, keepLogs=True)
    if not wallet_mgr.launch():
        raise click.ClickException("Failed to launch wallet manager (kiod).")

    bios_node.walletMgr = wallet_mgr
    for n in nodes:
        n.walletMgr = wallet_mgr

    try:
        wallet_mgr.create("ignition")

        # Set up sysio account
        sysio_name = "sysio"
        sysio_keys = producer_keys[sysio_name]
        sysio_account = Account(sysio_name)
        sysio_account.ownerPrivateKey = sysio_keys["private"]
        sysio_account.ownerPublicKey = sysio_keys["public"]
        sysio_account.activePrivateKey = sysio_keys["private"]
        sysio_account.activePublicKey = sysio_keys["public"]

        if not wallet_mgr.importKey(sysio_account, wallet_mgr.wallets.get("ignition")):
            raise click.ClickException("Failed to import sysio keys into wallet.")

        # Deploy sysio.bios
        _echo("Deploying sysio.bios contract...")
        contract_dir = str(lib_testing_contracts / "sysio.bios")
        trans = bios_node.publishContract(
            sysio_account,
            contract_dir,
            "sysio.bios.wasm",
            "sysio.bios.abi",
            waitForTransBlock=True,
        )
        if trans is None:
            raise click.ClickException("Failed to publish sysio.bios contract.")
        Node.validateTransaction(trans)

        # Activate all builtin protocol features
        _echo("Activating protocol features...")
        bios_node.activateAllBuiltinProtocolFeature()

        if activate_if:
            _echo("Activating instant finality...")
            finalizer_nodes = []
            for n in nodes:
                if n and n.keys and n.keys[0].blspubkey and n.isProducer:
                    finalizer_nodes.append(n)
            if finalizer_nodes:
                _set_finalizers(bios_node, finalizer_nodes)

        # Create producer accounts
        _echo("Creating producer accounts...")
        producer_keys_copy = dict(producer_keys)
        producer_keys_copy.pop(sysio_name)
        accounts = []
        for name, keys in producer_keys_copy.items():
            acct = Account(name)
            acct.ownerPrivateKey = keys["private"]
            acct.ownerPublicKey = keys["public"]
            acct.activePrivateKey = keys["private"]
            acct.activePublicKey = keys["public"]
            trans = bios_node.createAccount(acct, sysio_account, 0)
            if trans is None:
                raise click.ClickException(f"Failed to create account {name}.")
            Node.validateTransaction(trans)
            accounts.append(acct)

        trans_id = Node.getTransId(trans)
        if not bios_node.waitForTransactionInBlock(trans_id):
            raise click.ClickException(
                "Failed to validate last account creation in block."
            )

        bios_node.validateAccounts(accounts)

        # Create system accounts
        _echo("Creating system accounts...")
        system_accounts = [
            "sysio.noop",
            "sysio.bpay",
            "sysio.msig",
            "sysio.names",
            "sysio.token",
            "sysio.vpay",
            "sysio.wrap",
            "sysio.roa",
            "sysio.acct",
            "sysio.authex",
            "dev.owner1",
        ]
        acct_trans = []
        for acct_name in system_accounts:
            new_account = copy.deepcopy(sysio_account)
            new_account.name = acct_name
            t = bios_node.createAccount(new_account, sysio_account, 0)
            if t is None:
                raise click.ClickException(
                    f"Failed to create system account {acct_name}."
                )
            acct_trans.append(t)

        for t in acct_trans:
            Node.validateTransaction(t)
        trans_ids = list(map(Node.getTransId, acct_trans))
        if not bios_node.waitForTransactionsInBlock(trans_ids):
            raise click.ClickException("Failed to validate system account creation.")

        # Deploy noop contract (optional - only available if test contracts were built)
        noop_dir = unittests_test_contracts / "noop"
        if (noop_dir / "noop.wasm").exists():
            _echo("Deploying noop contract...")
            noop_account = copy.deepcopy(sysio_account)
            noop_account.name = "sysio.noop"
            trans = bios_node.publishContract(
                noop_account,
                str(noop_dir),
                "noop.wasm",
                "noop.abi",
                waitForTransBlock=True,
            )
            if trans is None:
                _echo("WARNING: Failed to publish noop contract (non-fatal).")
        else:
            _echo("Skipping noop contract (test contracts not built).")

        # Deploy sysio.system
        _echo("Deploying sysio.system contract...")
        contract_dir = str(contracts_path / "sysio.system")
        trans = bios_node.publishContract(
            sysio_account,
            contract_dir,
            "sysio.system.wasm",
            "sysio.system.abi",
            waitForTransBlock=True,
        )
        if trans is None:
            raise click.ClickException("Failed to publish sysio.system contract.")
        Node.validateTransaction(trans)

        # Wait for sync before setting producers
        head_block = bios_node.getHeadBlockNum()
        if nodes:
            nodes[0].waitForBlock(head_block, timeout=Utils.systemWaitTimeout)

        # Set producers
        _echo("Setting producers...")
        counts: dict[int, int] = dict.fromkeys(range(total_nodes), 0)  # pyright: ignore[reportAssignmentType]
        prod_stanzas: list[dict[str, str]] = []
        prod_names: list[str] = []
        for name, keys in list(producer_keys_copy.items())[:21]:
            if counts[keys["node"]] >= prod_count:
                continue
            prod_stanzas.append(
                {"producer_name": keys["name"], "block_signing_key": keys["public"]}
            )
            prod_names.append(keys["name"])
            counts[keys["node"]] += 1
        set_prods_str = json.dumps({"schedule": prod_stanzas})
        _echo(f"Setting producers: {', '.join(prod_names)}")
        opts = "--permission sysio@active"
        trans = bios_node.pushMessage("sysio", "setprodkeys", set_prods_str, opts)
        if trans is None or not trans[0]:
            raise click.ClickException("Failed to set producers.")

        trans = trans[1]
        trans_id = Node.getTransId(trans)
        if not bios_node.waitForTransactionInBlock(trans_id):
            raise click.ClickException(
                "Failed to validate setprodkeys transaction in block."
            )

        # Wait for producer handoff
        _echo("Waiting for producer handoff...")

        def _check_handoff() -> bool:
            return bios_node.getInfo(exitOnError=True)["head_block_producer"] != "sysio"

        if not Utils.waitForBool(_check_handoff, timeout=90):
            raise click.ClickException("Block production handover failed.")

        # Deploy sysio.token
        _echo("Deploying sysio.token contract...")
        sysio_token_account = copy.deepcopy(sysio_account)
        sysio_token_account.name = "sysio.token"
        contract_dir = str(contracts_path / "sysio.token")
        trans = bios_node.publishContract(
            sysio_token_account,
            contract_dir,
            "sysio.token.wasm",
            "sysio.token.abi",
            waitForTransBlock=True,
        )
        if trans is None:
            raise click.ClickException("Failed to publish sysio.token contract.")

        # Set sysio.token as privileged
        trans = bios_node.setPriv(
            sysio_token_account, sysio_account, isPriv=True, waitForTransBlock=True
        )
        if trans is None:
            raise click.ClickException("Failed to set sysio.token as privileged.")

        # Create currency
        _echo("Creating and issuing tokens...")
        action = "create"
        data = f'{{"issuer":"{sysio_account.name}","maximum_supply":"1000000000.0000 {CORE_SYMBOL}"}}'
        opts = f"--permission {sysio_token_account.name}@active"
        trans = bios_node.pushMessage(sysio_token_account.name, action, data, opts)
        if trans is None or not trans[0]:
            raise click.ClickException("Failed to create currency.")
        Node.validateTransaction(trans[1])
        trans_id = Node.getTransId(trans[1])
        if not bios_node.waitForTransactionInBlock(trans_id):
            raise click.ClickException(
                "Failed to validate create currency transaction."
            )

        # Issue tokens
        action = "issue"
        data = (
            f'{{"to":"{sysio_account.name}",'
            f'"quantity":"1000000000.0000 {CORE_SYMBOL}",'
            f'"memo":"initial issue"}}'
        )
        opts = f"--permission {sysio_account.name}@active"
        trans = bios_node.pushMessage(sysio_token_account.name, action, data, opts)
        if trans is None or not trans[0]:
            raise click.ClickException("Failed to issue tokens.")
        Node.validateTransaction(trans[1])
        trans_id = Node.getTransId(trans[1])
        bios_node.waitForTransactionInBlock(trans_id)

        # Transfer initial funds
        _echo("Transferring initial funds to producer accounts...")
        initial_funds = f"1000000.0000 {CORE_SYMBOL}"
        for name, keys in producer_keys_copy.items():
            data = (
                f'{{"from":"{sysio_account.name}","to":"{name}",'
                f'"quantity":"{initial_funds}","memo":"init transfer"}}'
            )
            opts = f"--permission {sysio_account.name}@active"
            trans = bios_node.pushMessage(
                sysio_token_account.name, "transfer", data, opts
            )
            if trans is None or not trans[0]:
                raise click.ClickException(f"Failed to transfer funds to {name}.")
            Node.validateTransaction(trans[1])
        trans_id = Node.getTransId(trans[1])
        if not bios_node.waitForTransactionInBlock(trans_id):
            raise click.ClickException("Failed to validate last transfer transaction.")

        # Deploy sysio.roa
        _echo("Deploying sysio.roa contract...")
        sysio_roa_account = copy.deepcopy(sysio_account)
        sysio_roa_account.name = "sysio.roa"
        contract_dir = str(contracts_path / "sysio.roa")
        trans = bios_node.publishContract(
            sysio_roa_account,
            contract_dir,
            "sysio.roa.wasm",
            "sysio.roa.abi",
            waitForTransBlock=True,
        )
        if trans is None:
            raise click.ClickException("Failed to publish sysio.roa contract.")

        trans = bios_node.setPriv(
            sysio_roa_account, sysio_account, isPriv=True, waitForTransBlock=True
        )
        if trans is None:
            raise click.ClickException("Failed to set sysio.roa as privileged.")

        # Activate ROA
        _echo("Activating ROA...")
        action = "activateroa"
        data = '{"total_sys":"75496.0000 SYS","bytes_per_unit":"104"}'
        opts = f"--permission {sysio_roa_account.name}"
        trans = bios_node.pushMessage(sysio_roa_account.name, action, data, opts)
        trans_id = Node.getTransId(trans[1])
        if not bios_node.waitForTransactionInBlock(trans_id):
            raise click.ClickException("Failed to validate ROA activation.")

        # Register dev.owner1 as tier 1 node owner
        _echo("Registering dev.owner1 as tier-1 node owner...")
        trans = bios_node.pushMessage(
            "sysio.roa", "forcereg", '{"owner": dev.owner1, "tier": 1}', "-p sysio.roa"
        )
        trans_id = Node.getTransId(trans[1])
        if not bios_node.waitForTransactionInBlock(trans_id):
            raise click.ClickException("Failed to register dev.owner1.")

        # Deploy sysio.authex
        _echo("Deploying sysio.authex contract...")
        sysio_authex_account = copy.deepcopy(sysio_account)
        sysio_authex_account.name = "sysio.authex"
        contract_dir = str(contracts_path / "sysio.authex")
        trans = bios_node.publishContract(
            sysio_authex_account,
            contract_dir,
            "sysio.authex.wasm",
            "sysio.authex.abi",
            waitForTransBlock=True,
        )
        if trans is None:
            raise click.ClickException("Failed to publish sysio.authex contract.")

        trans = bios_node.setPriv(
            sysio_authex_account, sysio_account, isPriv=True, waitForTransBlock=True
        )
        if trans is None:
            raise click.ClickException("Failed to set sysio.authex as privileged.")

        txn = {
            "actions": [{
                "account": "sysio",
                "name": "updateauth",
                "data": {
                    "account": "sysio.authex",
                    "permission": "owner",
                    "parent": "",
                    "auth": {
                        "threshold": 1,
                        "keys": [
                            {
                                "key": sysio_account.activePublicKey,
                                "weight": 1
                            }
                        ],
                        "accounts": [
                            {
                                "permission": {
                                    "actor": "sysio.authex",
                                    "permission": "sysio.code"
                                },
                                "weight": 1
                            }
                        ],
                        "waits": []
                    }
                },
                "authorization": [
                    {
                        "actor": "sysio.authex",
                        "permission": "owner"
                    }
                ]
            }]
        }
        bios_node.pushTransaction(txn)

        # Init system contract
        _echo("Initializing system contract...")
        action = "init"
        data = f'{{"version":0,"core":"4,{CORE_SYMBOL}"}}'
        opts = f"--permission {sysio_account.name}@active"
        trans = bios_node.pushMessage(sysio_account.name, action, data, opts)
        trans_id = Node.getTransId(trans[1])
        if not bios_node.waitForTransactionInBlock(trans_id):
            raise click.ClickException("Failed to validate system init transaction.")

        _echo("Bootstrap complete.")

    finally:
        wallet_mgr.shutdown()


def _set_finalizers(bios_node: Any, finalizer_nodes: list[Any]) -> None:
    """Activate instant finality with the given finalizer nodes."""
    finalizers = []
    for n in finalizer_nodes:
        for key in n.keys:
            if key.blspubkey:
                finalizers.append(
                    {
                        "description": f"finalizer-{n.nodeId}",
                        "weight": 1,
                        "public_key": key.blspubkey,
                        "pop": key.blspop,
                    }
                )
    if not finalizers:
        return
    data = json.dumps(
        {
            "finalizer_policy": {
                "threshold": len(finalizers) * 2 // 3 + 1,
                "finalizers": finalizers,
            }
        }
    )
    opts = "--permission sysio@active"
    trans = bios_node.pushMessage("sysio", "setfinalizer", data, opts)
    if trans is None or not trans[0]:
        _echo("WARNING: Failed to set finalizers.")
        return
    trans_id = Node.getTransId(trans[1])
    bios_node.waitForTransactionInBlock(trans_id)


def _parse_cluster_keys(total_nodes: int) -> dict[str, Any] | None:
    """Parse producer keys from cluster start files (mirrors Cluster.parseClusterKeys)."""
    from TestHarness.Cluster import Cluster  # pyright: ignore[reportMissingImports]

    return Cluster.parseClusterKeys(total_nodes)


# ---------------------------------------------------------------------------
# Run cluster - start all nodes from saved state
# ---------------------------------------------------------------------------


def _run_cluster(chain_dir: Path) -> None:
    """Start all nodes from previously created cluster state and run in foreground."""
    state = _load_state(chain_dir)
    node_states = state["nodes"]

    procs: list[tuple[dict[str, Any], subprocess.Popen[bytes]]] = []
    log_files: list[Any] = []

    # Install SIGTERM handler so 'stop' command triggers clean shutdown
    def _sigterm_handler(_signum: int, _frame: Any) -> None:
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, _sigterm_handler)

    _echo(f"Starting {len(node_states)} nodes...")

    try:
        for ns in node_states:
            data_dir = Path(ns["data_dir"])
            cmd = list(ns["cmd"])

            # Remove --genesis-json and --genesis-timestamp for relaunch (chain already exists)
            cmd_clean: list[str] = []
            skip_next = False
            for arg in cmd:
                if skip_next:
                    skip_next = False
                    continue
                if arg in ("--genesis-json", "--genesis-timestamp"):
                    skip_next = True
                    continue
                cmd_clean.append(arg)
            cmd = cmd_clean

            # Allow producers to catch up from a stale chain timestamp
            if "--enable-stale-production" not in cmd:
                cmd.append("--enable-stale-production")

            data_dir.mkdir(parents=True, exist_ok=True)

            launch_time = datetime.now().strftime("%Y_%m_%d_%H_%M_%S")
            out_file = data_dir / "stdout.txt"
            err_file = data_dir / f"stderr.{launch_time}.txt"
            err_link = data_dir / "stderr.txt"

            sout = out_file.open("w")
            serr = err_file.open("w")
            log_files.extend([sout, serr])

            _echo(
                f"  Starting node {ns['nodeId']} (port {ns['port']}): {ns.get('producerName', 'non-producer')}"
            )
            proc = subprocess.Popen(cmd, stdout=sout, stderr=serr)

            # Update symlink
            try:
                err_link.unlink()
            except FileNotFoundError:
                pass
            err_link.symlink_to(err_file.name)

            # Write per-node pid
            pid_f = data_dir / "nodeop.pid"
            pid_f.write_text(str(proc.pid))

            procs.append((ns, proc))
            time.sleep(0.5)

        _echo(f"All {len(procs)} nodes started. Press Ctrl+C to stop.")
        _echo("")

        # Tail stderr logs from all nodes
        _tail_logs(procs)

    except (KeyboardInterrupt, SystemExit):
        _echo("Shutting down...")
    finally:
        _shutdown_procs(procs)
        for fh in log_files:
            try:
                fh.close()
            except Exception:  # noqa: BLE001
                pass


def _tail_logs(procs: list[tuple[dict[str, Any], subprocess.Popen[bytes]]]) -> None:
    """Block in foreground, monitoring node processes. Exit when any node dies or on interrupt."""
    # Open stderr files for tailing
    tail_fds: list[tuple[Any, Any]] = []
    for ns, _proc in procs:
        data_dir = Path(ns["data_dir"])
        err_link = data_dir / "stderr.txt"
        if err_link.exists():
            fh = err_link.open("r")
            # Seek to end so we only show new output
            fh.seek(0, 2)
            tail_fds.append((ns["nodeId"], fh))

    try:
        while True:
            # Check if any process died
            for ns, proc in procs:
                if proc.poll() is not None:
                    _echo(f"Node {ns['nodeId']} exited with code {proc.returncode}")
                    return

            # Read any new log lines
            for node_id, fh in tail_fds:
                while True:
                    line = fh.readline()
                    if not line:
                        break
                    click.echo(f"[node_{node_id}] {line}", nl=False)

            time.sleep(0.2)
    finally:
        for _, fh in tail_fds:
            try:
                fh.close()
            except Exception:  # noqa: BLE001
                pass


def _shutdown_procs(
    procs: list[tuple[dict[str, Any], subprocess.Popen[bytes]]],
) -> None:
    """Send SIGTERM to all running processes and wait for them."""
    for ns, proc in procs:
        if proc.poll() is None:
            _echo(f"  Stopping node {ns['nodeId']} (pid {proc.pid})...")
            proc.send_signal(signal.SIGTERM)
    # Wait for all to terminate
    for ns, proc in procs:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            _echo(f"  Force-killing node {ns['nodeId']} (pid {proc.pid})...")
            proc.kill()
            proc.wait()
    _echo("All nodes stopped.")


# ---------------------------------------------------------------------------
# CLI definition
# ---------------------------------------------------------------------------


@click.group()
@click.option(
    "--chain-dir",
    type=click.Path(),
    default=".",
    help="Base directory for chain data. Defaults to current directory.",
)
@click.option(
    "--force",
    is_flag=True,
    default=False,
    help="Allow destructive operations (e.g., overwrite existing chain-dir).",
)
@click.pass_context
def cli(ctx: click.Context, chain_dir: str, force: bool) -> None:
    """Wire chain cluster manager.

    Create, run, and manage a local Wire blockchain cluster.
    """
    ctx.ensure_object(dict)
    ctx.obj["chain_dir"] = Path(chain_dir).resolve()
    ctx.obj["force"] = force


@cli.command()
@click.option(
    "--build-dir",
    type=click.Path(exists=True, file_okay=False, resolve_path=True),
    required=True,
    help="CMake build directory (e.g. build/claude).",
)
@click.option("-p", "--pnodes", type=int, default=21, help="Number of producer nodes.")
@click.option(
    "-n",
    "--nodes",
    "total_nodes",
    type=int,
    default=0,
    help="Total number of nodes (default: same as pnodes).",
)
@click.option("--prod-count", type=int, default=21, help="Producers per producer node.")
@click.option(
    "-s",
    "--topology",
    "topo",
    type=str,
    default="mesh",
    help="Network topology (star, mesh, ring, line).",
)
@click.option(
    "--http-secure",
    is_flag=True,
    default=False,
    help="Skip adding permissive CORS and HTTP settings to each node's config.ini.",
)
@click.pass_context
def create(
    ctx: click.Context,
    build_dir: str,
    pnodes: int,
    total_nodes: int,
    prod_count: int,
    topo: str,
    http_secure: bool,
) -> None:
    """Create and bootstrap a new cluster.

    Generates configuration, starts all nodes, deploys system contracts,
    then shuts down cleanly. The resulting chain-dir can be started with 'run'.
    """
    chain_dir: Path = ctx.obj["chain_dir"]
    force: bool = ctx.obj["force"]
    build_path = Path(build_dir)

    # Validate that the build dir contains compiled contracts
    bios_wasm = (
        build_path
        / "libraries"
        / "testing"
        / "contracts"
        / "sysio.bios"
        / "sysio.bios.wasm"
    )
    if not bios_wasm.exists():
        raise click.ClickException(
            f"Could not find sysio.bios.wasm in {build_path}. "
            f"Expected layout: <build-dir>/libraries/testing/contracts/sysio.bios/sysio.bios.wasm"
        )

    # Import TestHarness from the build directory
    _setup_harness(build_path)

    _ensure_chain_dir(chain_dir, force)
    _acquire_lock(chain_dir)
    try:
        if total_nodes <= 0:
            total_nodes = pnodes
        _create_cluster(chain_dir, build_path, pnodes, total_nodes, prod_count, topo, not http_secure)
    finally:
        _release_lock(chain_dir)


@cli.command()
@click.pass_context
def run(ctx: click.Context) -> None:
    """Start the cluster in the foreground.

    All node logs are streamed to the terminal. Press Ctrl+C to stop.
    """
    chain_dir: Path = ctx.obj["chain_dir"]

    if not chain_dir.exists():
        raise click.ClickException(
            f"chain-dir {chain_dir} does not exist. Run 'create' first."
        )

    _acquire_lock(chain_dir)
    try:
        _run_cluster(chain_dir)
    finally:
        _release_lock(chain_dir)


@cli.command()
@click.pass_context
def stop(ctx: click.Context) -> None:
    """Stop a running cluster by sending SIGTERM to the manager process."""
    chain_dir: Path = ctx.obj["chain_dir"]
    pf = _pid_file(chain_dir)

    if not pf.exists():
        raise click.ClickException(
            f"No .pid file found in {chain_dir}. Is the cluster running?"
        )

    try:
        pid = int(pf.read_text().strip())
    except ValueError:
        raise click.ClickException(f"Invalid .pid file in {chain_dir}.")  # noqa: B904

    # Check if it's our own process (shouldn't happen, but guard against it)
    if pid == os.getpid():
        raise click.ClickException("Cannot stop self.")

    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        _echo(f"Process {pid} is not running. Removing stale .pid file.")
        pf.unlink()
        return

    _echo(f"Sending SIGTERM to cluster_manager (pid {pid})...")
    os.kill(pid, signal.SIGTERM)

    # Wait briefly for it to exit
    for _ in range(30):
        try:
            os.kill(pid, 0)
            time.sleep(0.5)
        except ProcessLookupError:
            _echo("Cluster stopped.")
            if pf.exists():
                pf.unlink()
            return

    _echo(f"Process {pid} did not exit after 15s. You may need to kill it manually.")


# ---------------------------------------------------------------------------
# IDE integration
# ---------------------------------------------------------------------------


def _get_single_node_cmd(chain_dir: Path) -> tuple[list[str], dict[str, Any]]:
    """Load cluster state and return (cmd, node_state) for a single-node cluster.

    Raises ClickException if the cluster has more than one non-bios node.
    """
    state = _load_state(chain_dir)
    node_states = state["nodes"]

    # Filter out bios node (nodeId == -1 by convention)
    non_bios = [ns for ns in node_states if ns["nodeId"] != "bios"]
    if len(non_bios) != 1:
        raise click.ClickException(
            f"IDE run configs are only supported for single-node clusters "
            f"(found {len(non_bios)} non-bios nodes)."
        )

    ns = non_bios[0]
    cmd = list(ns["cmd"])

    # Strip genesis args (same as `run`)
    cmd_clean: list[str] = []
    skip_next = False
    for arg in cmd:
        if skip_next:
            skip_next = False
            continue
        if arg in ("--genesis-json", "--genesis-timestamp"):
            skip_next = True
            continue
        cmd_clean.append(arg)
    cmd = cmd_clean

    if "--enable-stale-production" not in cmd:
        cmd.append("--enable-stale-production")

    return cmd, ns


@cli.group()
def ide() -> None:
    """IDE integration commands."""


@ide.command("run-config")
@click.option(
    "--name",
    "config_name",
    default=None,
    help="Override the run-configuration name (defaults to chain-dir absolute path).",
)
@click.argument("ide_target", type=click.Choice(["vscode", "clion"]))
@click.pass_context
def ide_run_config(ctx: click.Context, config_name: str | None, ide_target: str) -> None:
    """Generate an IDE run configuration for a single-node cluster.

    IDE_TARGET is either 'vscode' or 'clion'.

    The generated config maps to the same nodeop invocation used by 'run'.
    """
    chain_dir: Path = ctx.obj["chain_dir"]
    cmd, ns = _get_single_node_cmd(chain_dir)

    if not cmd:
        raise click.ClickException("Empty node command in cluster state.")

    nodeop_binary = cmd[0]
    args = cmd[1:]
    working_dir = str(ns["data_dir"])
    name = config_name or str(chain_dir)

    if ide_target == "vscode":
        _gen_vscode_launch(nodeop_binary, args, working_dir, name)
    elif ide_target == "clion":
        _gen_clion_run_config(nodeop_binary, args, working_dir, name)


def _gen_vscode_launch(
    binary: str, args: list[str], working_dir: str, name: str
) -> None:
    """Create or merge a launch configuration into .vscode/launch.json."""
    vscode_dir = Path(".vscode")
    vscode_dir.mkdir(exist_ok=True)
    launch_file = vscode_dir / "launch.json"

    new_config = {
        "type": "cppdbg",
        "request": "launch",
        "name": name,
        "program": binary,
        "args": args,
        "cwd": working_dir,
        "stopAtEntry": False,
        "MIMode": "gdb",
        "setupCommands": [
            {
                "description": "Enable pretty-printing for gdb",
                "text": "-enable-pretty-printing",
                "ignoreFailures": True,
            }
        ],
    }

    if launch_file.exists():
        try:
            content = json.loads(launch_file.read_text())
        except (json.JSONDecodeError, OSError):
            content = {"version": "0.2.0", "configurations": []}
    else:
        content = {"version": "0.2.0", "configurations": []}

    configs: list[dict[str, Any]] = content.get("configurations", [])

    # Replace existing config with same name, or append
    replaced = False
    for i, cfg in enumerate(configs):
        if cfg.get("name") == name:
            configs[i] = new_config
            replaced = True
            break
    if not replaced:
        configs.append(new_config)

    content["configurations"] = configs
    launch_file.write_text(json.dumps(content, indent=2) + "\n")
    _echo(f"VSCode launch config written to {launch_file}")
    _echo(f"  Config name: {name}")


def _detect_clion_cmake_profile() -> str:
    """Read .idea/cmake.xml and return the first enabled CMake profile name."""
    import xml.etree.ElementTree as ET

    cmake_xml = Path(".idea/cmake.xml")
    if not cmake_xml.exists():
        raise click.ClickException(
            "No .idea/cmake.xml found. Open this project in CLion first "
            "so the CMake profiles are configured."
        )
    tree = ET.parse(cmake_xml)
    for cfg in tree.iter("configuration"):
        if cfg.get("ENABLED", "true").lower() == "true":
            profile = cfg.get("PROFILE_NAME")
            if profile:
                return profile
    raise click.ClickException(
        "No enabled CMake profile found in .idea/cmake.xml."
    )


def _clion_escape_params(args: list[str]) -> str:
    """Quote args containing spaces/special chars, matching CLion's convention."""
    escaped: list[str] = []
    for arg in args:
        if " " in arg or '"' in arg:
            escaped.append(f"&quot;{_xml_escape(arg)}&quot;")
        else:
            escaped.append(_xml_escape(arg))
    return " ".join(escaped)


def _clion_config_attrs(
    name: str, args_str: str, working_dir: str, cmake_profile: str
) -> str:
    """Return the attribute string for a CLion <configuration> element."""
    return (
        f'default="false" name="{_xml_escape(name)}"'
        f' type="CMakeRunConfiguration" factoryName="Application"'
        f' PROGRAM_PARAMS="{args_str}"'
        f' REDIRECT_INPUT="false" ELEVATE="false"'
        f' USE_EXTERNAL_CONSOLE="false" EMULATE_TERMINAL="true"'
        f' WORKING_DIR="file://{_xml_escape(working_dir)}"'
        f' PASS_PARENT_ENVS_2="true"'
        f' PROJECT_NAME="wire-sysio" TARGET_NAME="nodeop"'
        f' CONFIG_NAME="{_xml_escape(cmake_profile)}"'
        f' RUN_TARGET_PROJECT_NAME="wire-sysio"'
        f' RUN_TARGET_NAME="nodeop"'
    )


def _clion_config_block(attrs: str, indent: str = "    ") -> str:
    """Return a complete <configuration> XML block."""
    return (
        f'{indent}<configuration {attrs}>\n'
        f'{indent}  <method v="2">\n'
        f'{indent}    <option name="com.jetbrains.cidr.execution.CidrBuildBeforeRunTaskProvider'
        f'$BuildBeforeRunTask" enabled="true" />\n'
        f'{indent}  </method>\n'
        f'{indent}</configuration>'
    )


def _gen_clion_run_config(
    binary: str, args: list[str], working_dir: str, name: str
) -> None:
    """Generate a CLion run configuration.

    1. Write .idea/runConfigurations/<name>.run.xml (shared/VCS config)
    2. Update .idea/workspace.xml RunManager (add config + list item)
    """
    import re

    cmake_profile = _detect_clion_cmake_profile()
    args_str = _clion_escape_params(args)
    attrs = _clion_config_attrs(name, args_str, working_dir, cmake_profile)

    # --- 1. Write the runConfigurations file ---
    run_dir = Path(".idea/runConfigurations")
    run_dir.mkdir(parents=True, exist_ok=True)

    safe_name = ""
    for c in name:
        safe_name += c if c.isalnum() or c in "-." else "_"
    config_file = run_dir / f"{safe_name}.run.xml"

    xml_content = (
        '<component name="ProjectRunConfigurationManager">\n'
        f'  <configuration {attrs}>\n'
        f'    <method v="2">\n'
        f'      <option name="com.jetbrains.cidr.execution.CidrBuildBeforeRunTaskProvider'
        f'$BuildBeforeRunTask" enabled="true" />\n'
        f'    </method>\n'
        f'  </configuration>\n'
        f'</component>\n'
    )
    config_file.write_text(xml_content)
    _echo(f"CLion run config written to {config_file}")

    # --- 2. Update workspace.xml RunManager ---
    workspace_file = Path(".idea/workspace.xml")
    if not workspace_file.exists():
        _echo("WARNING: .idea/workspace.xml not found, skipping RunManager update.")
        _echo(f"  Config name: {name}")
        _echo(f"  CMake profile: {cmake_profile}")
        return

    ws = workspace_file.read_text()
    escaped_name = _xml_escape(name)
    item_value = f"CMake Application.{name}"

    # Remove any existing <configuration> block with the same name
    pattern = re.compile(
        r'[ \t]*<configuration\b[^>]*\bname="'
        + re.escape(escaped_name)
        + r'"[^>]*type="CMakeRunConfiguration"[^>]*>.*?</configuration>\s*',
        re.DOTALL,
    )
    ws = pattern.sub("", ws)

    # Insert the new config block before the <list> inside RunManager
    config_block = _clion_config_block(attrs, indent="    ")
    run_mgr_list = re.search(
        r'(<component\s+name="RunManager"[^>]*>.*?)([ \t]*<list>)',
        ws,
        re.DOTALL,
    )
    if run_mgr_list:
        insert_pos = run_mgr_list.start(2)
        ws = ws[:insert_pos] + config_block + "\n" + ws[insert_pos:]

    # Add item to the <list> if not already present
    escaped_item = _xml_escape(item_value)
    if escaped_item not in ws:
        run_mgr_list_end = re.search(
            r'(<component\s+name="RunManager"[^>]*>.*?<list>.*?)([ \t]*</list>)',
            ws,
            re.DOTALL,
        )
        if run_mgr_list_end:
            insert_pos = run_mgr_list_end.start(2)
            item_line = f'      <item itemvalue="{escaped_item}" />\n'
            ws = ws[:insert_pos] + item_line + ws[insert_pos:]

    workspace_file.write_text(ws)
    _echo(f"  Updated .idea/workspace.xml RunManager")
    _echo(f"  Config name: {name}")
    _echo(f"  CMake profile: {cmake_profile}")


def _xml_escape(s: str) -> str:
    """Escape special characters for XML attribute values."""
    return (
        s.replace("&", "&amp;")
        .replace('"', "&quot;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )


if __name__ == "__main__":
    cli()
