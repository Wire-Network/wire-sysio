# Port Sharding Test Plan

This document describes the current port sharding scheme used by the Python integration tests. The goal is to let
tests that used to require serialized execution run concurrently without binding the same local listener port.

Port sharding has two layers:

1. CTest assigns each sharded test a unique `SYSIO_TEST_PORT_OFFSET`.
2. The test harness maps known hot listener ports into a compact, non-overlapping 256-port range for that offset.

When `SYSIO_TEST_PORT_OFFSET` is unset or `0`, the harness preserves the historical local ports such as `8888`,
`9876`, and `9899`.

## CTest Offset Allocation

CTest offsets are assigned in `cmake/test-helpers.cmake` by one shared allocator:

```cmake
set(SYSIO_TEST_PORT_OFFSET_START 100)
set(SYSIO_TEST_PORT_OFFSET_STRIDE 256)
```

Both `add_np_test()` and `add_lr_test()` call the same allocator, so `nonparallelizable_tests` and
`long_running_tests` cannot receive duplicate offsets during the same CMake configure.

The generated sequence is compact and deterministic:

```text
100, 356, 612, 868, 1124, ...
```

Each offset reserves one 256-port shard. The allocator is global instead of label-specific, which avoids the older
failure mode where an NP test and an LR test could both receive offset `1000`.

## Harness Port Mapping

All test-authored local listener ports should go through `Utils.shardPort(port)` or through a `Cluster` /
`WalletMgr` helper that already calls it.

For nonzero offsets, the harness computes:

```text
shard_base = 8888 + SYSIO_TEST_PORT_OFFSET
```

Known hot ports are then placed into fixed slots inside the 256-port shard:

| Raw port or range | Use | Sharded port |
|---|---|---|
| `9899..9999` | wallet ports | `shard_base + 0..99` |
| `8788` | BIOS HTTP helper port | `shard_base + 100` |
| `7899` | SHiP / explicit service port | `shard_base + 150` |
| `8080` | state history endpoint default | `shard_base + 151` |
| `9011` | alternate explicit service port | `shard_base + 152` |
| `9776..9822` | alternate P2P/listener ports | `shard_base + 153..199` |
| `8888` | normal node HTTP base | `shard_base + 200` |
| `9876..9898` | normal node P2P base/range | `shard_base + 225..247` |

Ports that are not in the compact map fall back to `port + SYSIO_TEST_PORT_OFFSET`. That fallback preserves old
manual sharding behavior, but new listener ports should be added to the compact map when they are part of the NP/LR
concurrent test surface.

The C++ SHiP test clients use the same compact mapping through `tests/test_port_shard.hpp`. Keep that helper in sync
with `Utils.shardPort()` whenever the compact map changes. Otherwise Python may start `nodeop` on the compact
state-history endpoint while `ship_client` or `ship_streamer` still tries to connect to `8080 + offset`.

The performance harness also has non-port shared state. Its timestamped artifact directory includes the target TPS,
`SYSIO_TEST_PORT_OFFSET`, and PID so concurrent `performance_test_basic_*` runs do not scrape each other's
`trxGenLogs`.

## Why The Slots Do Not Collide

Adjacent CTest shards differ by `256`. The compact mapping only uses slots `0..247` today, so no mapped hot listener
from one test can overlap the mapped hot listener ports from the next test.

For example, with offsets `100` and `356`:

| Raw port | Offset `100` | Offset `356` |
|---|---:|---:|
| wallet base `9899` | `8988` | `9244` |
| BIOS HTTP `8788` | `9088` | `9344` |
| SHiP `7899` | `9138` | `9394` |
| node HTTP `8888` | `9188` | `9444` |
| node P2P `9876` | `9213` | `9469` |

The first shard's compact hot range ends before the second shard starts.

## Ephemeral Port Consideration

The allocator starts low and uses compact 256-port strides so hot listener ports stay below the OS ephemeral range for
as many tests as possible. On the current Linux runner, `/proc/sys/net/ipv4/ip_local_port_range` starts at `32768`.

With the current combined NP/LR test list, the highest assigned offset observed during CTest metadata validation was
`22884`. The highest compact hot listener port is therefore:

```text
8888 + 22884 + 255 = 32027
```

That stays below `32768`, leaving test listener ports out of the default ephemeral allocation range on that runner.

## Developer Usage

Run an individual test with an isolated port range by setting `SYSIO_TEST_PORT_OFFSET`:

```bash
cd build/codex-system-contracts
SYSIO_TEST_PORT_OFFSET=100 python3 tests/nodeop_run_test.py
```

When launching tests manually at the same time, choose offsets separated by at least `256`:

```bash
SYSIO_TEST_PORT_OFFSET=100 python3 tests/nodeop_run_test.py
SYSIO_TEST_PORT_OFFSET=356 python3 tests/http_plugin_test.py
```

New test endpoints should use one of these patterns:

- `Utils.shardPort(<raw_port>)` for an explicit listener port.
- `Cluster.getHttpEndpoint(node_id)` for node HTTP endpoints.
- `Cluster.getNodeP2pEndpoint(node_id)` for node P2P endpoints.
- `Cluster.getBiosP2pEndpoint()` for the BIOS P2P endpoint.
- `sysio::testing::shard_port(<raw_port>)` in C++ test helpers or test binaries.

Do not hardcode shifted port numbers in tests. Keep the raw historical port in the test and let the harness assign the
correct shard.

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
print("max_compact_hot_port", 8888 + max(offsets) + 255)
PY
```

Run a collision-sensitive long-running subset concurrently:

```bash
ctest --test-dir build/codex-system-contracts -j3 -L long_running_tests \
  -R 'ship_streamer_test|ship_streamer_if_fetch_finality_data_test|auto_bp_gossip_peering_test' \
  --output-on-failure --timeout 1800
```

Run the full NP/LR set with normal high concurrency:

```bash
ctest --test-dir build/codex-system-contracts -j25 -L 'nonparallelizable_tests|long_running_tests' \
  --output-on-failure --timeout 1800
```

Stress all NP/LR tests at once to look for immediate listener collisions:

```bash
ctest --test-dir build/codex-system-contracts -j90 -L 'nonparallelizable_tests|long_running_tests' \
  --output-on-failure --timeout 1800
```

Then scan the log for collision signatures:

```bash
rg 'Address already in use|bind|Failed to bind|Failed to find free port|port .*not available' \
  /tmp/wire-np-lr-j90-*.log
```

## Current Validation Results

The current combined NP/LR metadata assigns 90 offsets with no duplicates. The observed offset range is:

```text
min_offset = 100
max_offset = 22884
max_compact_hot_port = 32027
```

`-j25` full NP/LR validation passed:

```text
100% tests passed, 0 tests failed out of 90
Total Test time (real) = 1047.14 sec
```

`-j90` stress validation did not find port-collision signatures. It completed with 86/90 tests passing. The failures
were overload/timing/resource-saturation failures, not listener collisions:

| Test | Observed failure |
|---|---|
| `cli_test` | `node doesn't appear to be running` after node startup under full-suite load |
| `separate_prod_fin_test` | `tx_cpu_usage_exceeded` while publishing `sysio.system` |
| `terminate-scenarios-test-resync` | `Block production handover failed` |
| `nodeop_read_terminate_at_block_lr_test` | block progress assertion: end block was not greater than terminate block |

The collision-sensitive SHiP tests and performance harness tests passed under `-j90`, which is the strongest current
evidence that port sharding itself is working.

## Known Non-Port Limits

Port sharding removes listener collisions; it does not make every NP/LR test safe under unbounded machine load.
At `-j90`, many tests bootstrap chains, publish large contracts, run transaction generators, and start many `nodeop`
processes at once. The observed failures are consistent with CPU and timing pressure. Treat `-j90` as a collision
stress probe, not as the expected CI concurrency level.
