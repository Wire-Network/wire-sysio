# Local council resource measurement

The opt-in `maximum_tiers_and_ballot_storage` test measures actual transaction CPU billing and action-trace RAM deltas with 1,000 registered candidates and frozen tiers of 21, 84, and 1,000 owners. Every owner submits a complete ballot: 60 choices for T1 and 63 for T2/T3. It generates missing flights, tabulates, aborts the partial election, and purges all ephemeral rows in bounded batches.

Build the matching ABI/WASM and `contracts_unit_test` first, then run from the Sysio repository (choose an absolute report path):

```sh
WIRE_COUNCIL_RESOURCE_REPORT=/absolute/path/council-resources.json \
  build/wire-fix/build/contracts/tests/contracts_unit_test \
  --run_test=sysio_councl_tests/maximum_tiers_and_ballot_storage \
  --report_level=detailed -- --sys-vm-jit
```

The measurement path passes zero for the tester's CPU override, disabling its usual fixed 2,000 µs charge. The chain enforces its configured transaction CPU limit and the test supplies a finite wall deadline using that limit. The JSON report records the configured limit, actual billed CPU, elapsed trace time, and RAM changes. Without the environment variable, the same case is an ordinary deterministic behavior test.

## Observed October 8, 2026

Linux x86-64 on an Intel Xeon 6975P-C, Clang 18.1.8 Release host build, CDT `-Os` contract, `sys-vm-jit`. The measured case passed 5,842 assertions. The configured per-transaction limit was 150,000 µs.

| Operation | Transactions | Maximum billed CPU (µs) | Net RAM change (bytes) |
|---|---:|---:|---:|
| Register candidates | 1,000 | 124 | 156,178 |
| Start initialization | 1 | 137 | 5,880 |
| Load tier snapshots | 24 | 316 | 303,520 |
| Finalize initialization | 1 | 1,253 | 263 |
| Generate remaining flights (21-seat batch) | 1 | 2,551 | 4,161 |
| T1 complete ballots | 21 | 180 | 5,418 |
| T2 complete ballots | 84 | 256 | 22,008 |
| T3 complete ballots | 1,000 | 413 | 262,000 |
| Tabulate (21-seat batch) | 1 | 202 | 338 |
| Purge (7 rows per batch) | 462 | 194 | -760,026 |

Peak cumulative net RAM growth across measured council actions was 760,204 bytes; after abort cleanup the net remainder was 178 bytes. Two manual nominations plus transition/reset calls are also included in the JSON totals. The largest individual ballot allocation was 262 bytes. RAM values sum deltas for every payer reported by the measured council action traces, including refunds.

These are observations from one local JIT run, not production budgets or latency guarantees. Trace elapsed time also includes host work and differs from billed CPU; first-use compilation/caches and host load affect timing. Account creation, deployed code, chain bootstrap, non-council transactions, and node process memory are outside these RAM/CPU totals. The fixture uses unlimited account CPU/NET weights and tests the configured transaction limit; deployed account resource allocations can be more restrictive. Other runtimes remain separate validation. Operators should use the deployment's actual configuration and reduce generation/tabulation or cleanup batches when needed.
