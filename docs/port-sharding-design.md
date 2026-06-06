# Port Sharding Test Design

This document describes how the Python integration tests and the C++ test helper assign ports so
`nonparallelizable_tests` and `long_running_tests` can run concurrently without listener collisions.

Port sharding has two layers:

1. CTest assigns each NP/LR test a unique `SYSIO_TEST_PORT_OFFSET`.
2. Test code asks for ports by logical category with `getPort(category, index)`.

When `SYSIO_TEST_PORT_OFFSET` is unset or `0`, the helpers return the historical local ports such as `8888`, `9876`,
and `9899`. This keeps direct manual test runs familiar.

## CTest Offset Allocation

CTest offsets are assigned in `cmake/test-helpers.cmake` by one shared allocator:

```cmake
set(SYSIO_TEST_PORT_OFFSET_START 100)
set(SYSIO_TEST_PORT_OFFSET_STRIDE 192)
```

Both `add_np_test()` and `add_lr_test()` call this allocator. The allocator is global, not label-specific, so an NP
test and an LR test cannot receive the same offset during one CMake configure.

The generated sequence is compact and deterministic:

```text
100, 292, 484, 676, 868, ...
```

Each offset reserves one 192-port shard. For a nonzero offset, category slots are mapped from this base:

```text
shard_base = 8888 + SYSIO_TEST_PORT_OFFSET
```

## Avoiding Local Listener Ports

By default, CMake assigns compact shards without reserving machine-specific ports. This keeps the allocation
deterministic across developer machines and CI:

```cmake
SYSIO_TEST_FORBIDDEN_PORTS=""
SYSIO_DETECT_LISTENING_TEST_PORTS=OFF
```

When a developer machine has a long-lived local service inside the generated shard range, pass a semicolon-separated
list of actual TCP ports that tests must not overlap:

```bash
cmake -S . -B build/codex-system-contracts -DSYSIO_TEST_FORBIDDEN_PORTS='11434;19981'
```

The allocator skips any shard whose actual port range contains one of those ports. For example, with
`SYSIO_TEST_FORBIDDEN_PORTS=11434`, the shard `11292..11483` is skipped because it contains `11434`.

For local-only convenience, CMake can also snapshot currently listening TCP ports during configure and append them to
the forbidden list:

```bash
cmake -S . -B build/codex-system-contracts -DSYSIO_DETECT_LISTENING_TEST_PORTS=ON
```

Detection is intentionally opt-in. It depends on the host state at configure time, so it is useful for local developer
machines but should not be required for reproducible CI allocation.

## Category Mapping

New test listener ports must use the logical category API:

- Python: `Utils.getPort(Utils.PortStateHistory, index)`
- C++: `sysio::testing::get_port(sysio::testing::port_category::state_history, index)`

The old raw-port compatibility helpers `Utils.shardPort(port)` and `sysio::testing::shard_port(port)` were introduced
only for this port-sharding work and have been removed. New code should not add another raw-port fallback. Assign a
category instead, because categories make collisions visible at review time.

Current category slots are:

| Category | Default unsharded port | Slot range | Capacity | Primary use |
|---|---:|---:|---:|---|
| `ship` | `7899` | `0` | `1` | explicit SHiP helper port |
| `state_history` | `8080` | `1` | `1` | state history websocket endpoint |
| `bios_http` | `8788` | `2` | `1` | BIOS HTTP endpoint |
| `node_http` | `8888` | `3..90` | `88` | node HTTP endpoints |
| `alternate_service` | `8976` | `91` | `1` | explicit alternate service listener |
| `plugin_http_peer` | `9009` | `92` | `1` | plugin HTTP peer endpoint |
| `plugin_http_local` | `9011` | `93` | `1` | plugin HTTP local endpoint |
| `alternate_p2p` | `9776` | `94..140` | `47` | alternate P2P/listener endpoints |
| `p2p` | `9876` | `141..163` | `23` | normal node P2P endpoints |
| `wallet` | `9899` | `164..168` | `5` | wallet/kiod endpoints |
| `transaction_only` | `9902` | `169..170` | `2` | transaction-only P2P endpoints |
| `ipv6_probe` | `9997` | `171..174` | `4` | IPv6 probe listeners |

The current highest used slot is `174`, leaving slots `175..191` reserved for future categories without changing the
CTest stride.

## Why Categories Avoid Collisions

Adjacent CTest shards differ by `192`. The category allocator only uses slots `0..174`, so no mapped listener from one
test can overlap the mapped listener ports from the next test.

For example, with offsets `100` and `292`:

| Category/index | Offset `100` | Offset `292` |
|---|---:|---:|
| `state_history` | `8989` | `9181` |
| `bios_http` | `8990` | `9182` |
| `node_http[0]` | `8991` | `9183` |
| `alternate_p2p[0]` | `9082` | `9274` |
| `p2p[0]` | `9129` | `9321` |
| `wallet[0]` | `9152` | `9344` |
| `transaction_only[0]` | `9157` | `9349` |

The first shard's active category range ends before the next shard starts.

## Shared State Considerations

Port sharding only solves listener-port conflicts. Some tests also need non-port shared state kept local to the test:

- The performance harness includes target TPS, `SYSIO_TEST_PORT_OFFSET`, and PID in generated artifact paths so
  concurrent performance tests do not scrape each other's `trxGenLogs`.
- `ship_test_unix` uses a short absolute Unix socket path under `/tmp`, keyed by the allocated state-history port.
  This avoids the Linux `sockaddr_un` path length limit when the build directory or `TestLogs` path is long.
- Wallet-manager shutdown clears the tracked process so later tests do not inherit stale `kiod` state.

## Timeout Considerations

`nodeop_irreversible_mode_lr_test` is a 20-node scenario that validates 19 relaunch, replay, read-mode, and snapshot
combinations. Under high concurrent NP/LR execution it can exceed the default CTest timeout while still making
progress. The test therefore has an explicit `TIMEOUT 1500`.

This timeout is not a port-sharding mechanism; it prevents a legitimate long-running scenario from being killed early
when the suite is intentionally run with high parallelism.

## Developer Usage

Run an individual test with an isolated port range by setting `SYSIO_TEST_PORT_OFFSET`:

```bash
cd build/codex-system-contracts
SYSIO_TEST_PORT_OFFSET=100 python3 tests/nodeop_run_test.py
```

When launching tests manually at the same time, choose offsets separated by at least `192`:

```bash
SYSIO_TEST_PORT_OFFSET=100 python3 tests/nodeop_run_test.py
SYSIO_TEST_PORT_OFFSET=292 python3 tests/http_plugin_test.py
```

New test endpoints should use one of these patterns:

- `Utils.getPort(Utils.PortStateHistory)` for a named Python listener category.
- `Utils.getPort(Utils.PortP2P, node_id)` for an indexed Python listener category.
- `Cluster.getHttpEndpoint(node_id)` for node HTTP endpoints.
- `Cluster.getNodeP2pEndpoint(node_id)` for node P2P endpoints.
- `Cluster.getBiosP2pEndpoint()` for the BIOS P2P endpoint.
- `sysio::testing::get_port(port_category::state_history)` in C++ test helpers or test binaries.

Do not hardcode shifted port numbers in tests. Keep the unsharded category default local and let the harness assign
the current test's shard.

## Validation Commands

Check that combined NP/LR CTest metadata has no duplicate shard offsets:

```bash
ctest --test-dir build/codex-system-contracts -N -V -L 'nonparallelizable_tests|long_running_tests' \
  | rg 'SYSIO_TEST_PORT_OFFSET=' \
  | sed 's/.*=//' \
  | sort -n \
  | uniq -d
```

Summarize the current offset range and highest compact hot listener port:

```bash
python3 - <<'PY'
import re
import subprocess

out = subprocess.check_output([
    "ctest",
    "--test-dir",
    "build/codex-system-contracts",
    "-N",
    "-V",
    "-L",
    "nonparallelizable_tests|long_running_tests",
], text=True)

offsets = [int(x) for x in re.findall(r"SYSIO_TEST_PORT_OFFSET=(\d+)", out)]
print("count", len(offsets))
print("duplicates", len(offsets) - len(set(offsets)))
print("min_offset", min(offsets))
print("max_offset", max(offsets))
print("max_hot_port", 8888 + max(offsets) + 191)
PY
```

Run the full NP/LR set with high local concurrency:

```bash
ctest --test-dir build/codex-system-contracts -j30 -L 'nonparallelizable_tests|long_running_tests' \
  --output-on-failure
```

Stress all NP/LR tests at once to look for immediate listener collisions:

```bash
ctest --test-dir build/codex-system-contracts -j90 -L 'nonparallelizable_tests|long_running_tests' \
  --output-on-failure
```

Then scan the log for collision signatures:

```bash
rg 'Address already in use|bind|Failed to bind|Failed to find free port|port .*not available' \
  /tmp/wire-np-lr-j90-*.log
```

## Current Validation Results

The current combined NP/LR metadata assigns 105 offsets with no duplicates:

```text
min_offset = 100
max_offset = 20068
max_hot_port = 29147
```

Targeted validation after the latest fixes:

```text
p2p_multiple_listen_test:          passed
nodeop_irreversible_mode_lr_test:  passed
ship_test_unix:                    passed
```

A full `-j30` NP/LR run after fixing `p2p_multiple_listen_test` and `nodeop_irreversible_mode_lr_test` showed both
original failures passing. It then exposed `ship_test_unix` failing with `File name too long` on the Unix socket path.
That was fixed by moving the Unix socket to the short `/tmp/sysio-ship-<port>.sock` path and validated with a targeted
`ship_test_unix` run.

## Known Non-Port Limits

Port sharding removes listener collisions; it does not make every NP/LR test safe under unbounded machine load.
At very high parallelism, many tests bootstrap chains, publish large contracts, run transaction generators, and start
many `nodeop` processes at once. Treat `-j90` as a collision stress probe, not as the expected CI concurrency level.
