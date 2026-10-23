# Test ports

Network tests (`nonparallelizable_tests` and `long_running_tests`) take their listener ports from the host's **bind
registry**, the same registry wire-tools-ts clusters use. Every port a test binds comes from a lease, so tests and
clusters running at the same time on one host never collide.

The registry protocol is specified in wire-tools-ts
[`docs/bind-registry/README.md`](https://github.com/Wire-Network/wire-tools-ts/blob/master/docs/bind-registry/README.md).
`tests/TestHarness/bind_registry.py` is its Python client (standard library only). It ships inside TestHarness, so an
installed TestHarness leases ports too; `tests/bind_registry.py` is its command-line entry point.

## How a test gets its ports

- **Under CTest**, `add_np_test` / `add_lr_test` (`cmake/test-helpers.cmake`) run the test through
  `tests/bind_registry.py`. The wrapper leases one block of 192 consecutive ports, runs the test in its own session
  with the lease in `SYSIO_TEST_PORTS`, waits for the whole session to exit, deletes its registration and exits with
  the test's status (`128 + N` when the test died of signal N). Signals sent to the wrapper are forwarded to the
  session. Processes the test leaves behind get 5 s, then SIGTERM and 10 s, then SIGKILL and 10 s; if any survive the
  wrapper fails loudly. A test that can leave processes behind needs up to 25 s of its CTest `TIMEOUT` for this.
- **By hand**, nothing is needed. `python3 tests/<test>.py` takes a lease for itself the first time it asks for a port
  and releases it on exit; importing TestHarness or running `--help` takes none. A native test binary (C++) reads
  `SYSIO_TEST_PORTS` only, so run it as `python3 tests/bind_registry.py <binary> [args]`.

A lease avoids every port in a live registration, the reserved agave band 8000–10000 and the kernel's ephemeral range,
and every port in it binds on TCP and UDP when it is taken (IPv4 and IPv6; a host without usable IPv6 is judged on
IPv4). Blocks above 10000 are preferred; 1024–7999, where development services often listen, is a fallback.

## Asking for a port

Ask by listener category and index, never by number:

- Python: `Utils.getPort(Utils.PortStateHistory)`, `Utils.getPort(Utils.PortP2P, node_id)`, or the `Cluster`
  helpers (`getHttpEndpoint`, `getNodeP2pEndpoint`, `getBiosP2pEndpoint`).
- C++: `sysio::testing::get_port(port_category::state_history, index)` from `tests/test_ports.hpp`.

Each category owns fixed slots of the lease. Python (`testUtils.py`) and C++ (`test_ports.hpp`) share this layout;
`trx_generator_tests` checks the C++ side.

| Category | Slots | Capacity | Use |
|---|---:|---:|---|
| `ship` | `0` | 1 | explicit SHiP helper port |
| `state_history` | `1` | 1 | state history websocket |
| `bios_http` | `2` | 1 | BIOS HTTP |
| `node_http` | `3..90` | 88 | node HTTP |
| `alternate_service` | `91` | 1 | alternate service listener (also an unbound endpoint for clio) |
| `plugin_http_peer` | `92` | 1 | plugin HTTP peer endpoint |
| `plugin_http_local` | `93` | 1 | plugin HTTP local endpoint |
| `bios_p2p` | `94` | 1 | BIOS P2P |
| `alternate_p2p` | `95..140` | 46 | alternate P2P listeners |
| `p2p` | `141..163` | 23 | node P2P |
| `wallet` | `164..168` | 5 | kiod |
| `transaction_only` | `169..170` | 2 | transaction-only P2P listeners |
| `ipv6_probe` | `171..174` | 4 | IPv6 probe listeners |
| `raw_client` | `175..190` | 16 | source ports of the raw P2P sessions `TestHarness/raw_p2p.py` dials |

Slot `191` is free for a new category. The lease is consecutive, so `getPort(PortNodeHttp, i)` equals
`getPort(PortNodeHttp) + i`.

## Self-tests

`bind_registry_test` (unit and wrapper tests; private registries with the bind check faked, real sockets only on a
block it leases from the host registry) and `bind_registry_stress_test` (32 concurrent leases from the host registry,
every port bound; `RUN_SERIAL`) run in `long_running_tests` and carry the label `bind_registry_tests`:

```bash
ctest -L bind_registry_tests --output-on-failure
```

`tests/bind_registry_register.py <port>...` (or `python3 tests/bind_registry.py --register <port>...`) registers
exactly the given ports and holds them until stdin closes, with the command-line contract of wire-tools-ts' interop
suite (its README section 8): point `WIRE_BIND_REGISTRY_PYTHON_CLIENT` at `tests/bind_registry_register.py`.
