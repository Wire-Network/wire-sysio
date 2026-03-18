# Cluster Manager Guide

`tools/cluster_manager.py` is a CLI tool for creating, running, and managing local Wire blockchain clusters. It wraps the TestHarness framework (used by integration tests) into a developer-friendly workflow for standing up fully-bootstrapped chains on your machine.

## Prerequisites

- Python 3.10+
- `click` package (`pip install click`)
- A completed CMake build of wire-sysio with contracts (see `CLAUDE.md` for build instructions)

The build directory must contain:
- `tests/TestHarness/` (generated at cmake configure time)
- `libraries/testing/contracts/sysio.bios/sysio.bios.wasm`
- `contracts/sysio.system/sysio.system.wasm`
- `contracts/sysio.token/sysio.token.wasm`
- `libraries/testing/contracts/sysio.roa/sysio.roa.wasm`

## Global Options

These flags appear **before** any subcommand:

| Option | Default | Description |
|---|---|---|
| `--chain-dir PATH` | `.` (current directory) | Base directory where all chain data is stored. Each cluster gets its own chain-dir. |
| `--force` | `false` | Allow destructive operations. Required when `create` targets a non-empty directory. |

```bash
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 --force create ...
```

## Commands

### `create`

Create and bootstrap a new cluster. This generates node configurations, starts all nodes, deploys system contracts, creates accounts, issues tokens, then shuts down cleanly. The resulting chain-dir is ready to be started with `run`.

```bash
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 \
    create --build-dir build/debug-clang-clion
```

#### Options

| Option | Default | Description |
|---|---|---|
| `--build-dir PATH` | *(required)* | CMake build directory containing compiled contracts and TestHarness. |
| `-p`, `--pnodes INT` | `21` | Number of producer nodes in the cluster. |
| `-n`, `--nodes INT` | same as `--pnodes` | Total number of nodes (includes non-producing nodes if larger than `--pnodes`). |
| `--prod-count INT` | `21` | Number of producers (defproducerX accounts) registered across the cluster. |
| `-s`, `--topology STR` | `mesh` | Network topology connecting the nodes. Options: `star`, `mesh`, `ring`, `line`. |
| `--http-secure` | `false` | Skip adding permissive CORS and HTTP settings to each node's `config.ini` (see below). |

#### `--http-secure`

By default, permissive CORS and HTTP settings are appended to every node's `config.ini` after generation. This is useful when developing frontends or tools that make cross-origin requests to the node's HTTP API. Pass `--http-secure` to skip these settings and use nodeop's defaults instead.

Default settings (applied unless `--http-secure` is given):

```ini
access-control-allow-origin = *
access-control-allow-headers = *
verbose-http-errors = true
http-validate-host = false
```

#### Bootstrap Sequence

The `create` command performs the following during bootstrap:

1. Deploy `sysio.bios` contract
2. Activate all builtin protocol features
3. Activate instant finality (BLS finalizers)
4. Create producer accounts (defproducera, defproducerb, ...)
5. Create system accounts (sysio.token, sysio.msig, sysio.roa, etc.)
6. Deploy `sysio.system` contract
7. Set producers and wait for block production handoff
8. Deploy `sysio.token` contract and issue 1B SYS tokens
9. Transfer initial funds (1M SYS) to each producer account
10. Deploy `sysio.roa` contract and activate ROA
11. Initialize the system contract

#### Directory Layout

After `create`, the chain-dir has the following structure:

```
<chain-dir>/
  .cluster_state.json   # Persisted node metadata (used by run/ide)
  data/
    node_bios/          # Bios node data, config, and logs
    node_00/            # First producer node
    node_01/            # Second producer node (if applicable)
    ...
  wallet/
    default.wallet      # Wallet files
    kiod_out.log
    kiod_err.log
```

#### Examples

Single-node cluster (simplest setup, good for development):
```bash
./tools/cluster_manager.py --chain-dir ~/my-chain \
    create --build-dir build/debug-clang-clion -p 1 --prod-count 1
```

Default 21-producer cluster:
```bash
./tools/cluster_manager.py --chain-dir ~/my-chain \
    create --build-dir build/debug-clang-clion
```

3 producer nodes with 5 producers each in a star topology:
```bash
./tools/cluster_manager.py --chain-dir ~/my-chain \
    create --build-dir build/debug-clang-clion -p 3 --prod-count 5 -s star
```

---

### `run`

Start a previously created cluster in the foreground. All node stderr logs are streamed to the terminal. The process stays alive until interrupted.

```bash
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 run
```

- Reads `.cluster_state.json` from the chain-dir to reconstruct node commands.
- Automatically strips genesis arguments (`--genesis-json`, `--genesis-timestamp`) since the chain already exists.
- Adds `--enable-stale-production` so producers resume even if the chain timestamp is in the past.
- Writes a `.pid` file to enforce singleton (only one `run` per chain-dir).
- Press **Ctrl+C** to gracefully shut down all nodes (SIGTERM).

The `run` command does **not** require `--build-dir` since the cluster is already created and all paths are stored in the state file.

---

### `stop`

Stop a running cluster from another terminal by sending SIGTERM to the manager process.

```bash
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 stop
```

- Reads the `.pid` file to find the running `cluster_manager` process.
- Sends SIGTERM and waits up to 15 seconds for a clean exit.
- Cleans up the `.pid` file on success.
- Handles stale `.pid` files (process no longer running) gracefully.

---

### `ide run-config`

Generate an IDE run configuration that maps to the same `nodeop` invocation used by `run`. This lets you launch and debug the node directly from your IDE.

**Restriction**: Only works for single-node clusters (1 non-bios node). Multi-producer single-node clusters are fine.

```bash
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 \
    ide run-config [--name CONFIG_NAME] <vscode|clion>
```

#### Options

| Option | Default | Description |
|---|---|---|
| `--name STR` | absolute chain-dir path | Override the name shown in the IDE's run configuration dropdown. |

#### `vscode`

Creates or updates `.vscode/launch.json` with a `cppdbg` (GDB) launch configuration.

```bash
./tools/cluster_manager.py --chain-dir ~/my-chain \
    ide run-config --name "my-dev-chain" vscode
```

- Merges into an existing `launch.json` if one exists.
- If a config with the same name already exists, it is replaced.

#### `clion`

Creates a CLion run configuration in `.idea/runConfigurations/` and registers it in `.idea/workspace.xml`.

```bash
./tools/cluster_manager.py --chain-dir ~/my-chain \
    ide run-config --name nodeop-dev-001 clion
```

- Auto-detects the active CMake profile from `.idea/cmake.xml` (e.g. `Debug-WSL (clang-18)`).
- Targets the `nodeop` CMake target with build-before-run enabled.
- Updates the `RunManager` component in `workspace.xml` so the config appears in the dropdown immediately.

**Note**: The project must have been opened in CLion at least once (`.idea/cmake.xml` must exist).

---

## Singleton Enforcement

Only one `run` process can manage a given chain-dir at a time. This is enforced via a `.pid` file written to the chain-dir root. If you see an error about another process already running:

1. Use `stop` to terminate it, or
2. If the process crashed, the stale `.pid` file is detected automatically and cleaned up.

## Typical Workflow

```bash
# 1. Create a single-node dev chain
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 \
    create --build-dir build/debug-clang-clion -p 1 --prod-count 1

# 2. Generate a CLion run config for easy debugging
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 \
    ide run-config --name nodeop-dev-001 clion

# 3. Either run from terminal...
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 run

# 4. ...or launch directly from CLion using the generated config

# 5. Stop from another terminal
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 stop

# 6. Re-create with --force to start fresh
./tools/cluster_manager.py --chain-dir /opt/wire/chains/dev-001 --force \
    create --build-dir build/debug-clang-clion -p 1 --prod-count 1
```
