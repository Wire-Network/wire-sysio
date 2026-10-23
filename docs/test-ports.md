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

A reader deletes a registration only when its name is not a positive pid, the pid is dead, or the pid now runs a
different program (the recorded and running executable names both non-empty and different). A live owner's file that
cannot be read or is not a valid registration is left in place, and the lease fails naming it rather than hand out its
ports. (A port is an integer in 1..65535; an integer-valued JSON number such as `20500.0` counts as `20500`, as it does
for the TypeScript client.)

Such a file blocks **every** lease on the host until its owner exits. When a lease fails this way:

- Identify the owner from the pid the error names: `ps -o pid,user,args -p <pid>`.
- Wait for it to finish, or stop it. Once it exits, the next reader deletes its file.
- The usual cause is an older harness, from another checkout, that still writes the registry's old array format.
  Update that checkout, or let its run finish.
- In a shared `/tmp`, the file may belong to another user and be unreadable to you. Ask that user, or wait for their
  process to exit.
- To keep working meanwhile, set `WIRE_BIND_REGISTRY_PATH` to a private directory. Leases then ignore the host
  registry entirely, so nothing protects your ports from that process or any other process on the host.

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

Slots `175..191` are free for new categories. The lease is consecutive, so `getPort(PortNodeHttp, i)` equals
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
Its exit codes: 0 released when stdin closes, 2 usage, 3 a requested port is held by another live registration, 4 the
lock was not taken in time, 5 a live owner's registration cannot be read or is not valid.
