# KV Benchmark Results

A summary of what KV storage costs, for contract writers. The full record is `docs/kv-baseline.md`.

Every number here comes from `docs/kv-baseline.md` as it stood at commit `8940102fbb`. Those numbers were measured before
the iterator and interface work on that branch, and they are the baseline that work reports against. They describe the
intrinsics as they were then, so they do not show the cost of the current ones. The host call counts below have the same
limit.

All of it is a Release build under sys-vm-oc, on a quiet machine. Each number is the mean of three clean passes.
`docs/kv-baseline.md` has the ranges and the commands. A change smaller than a range is not shown by these numbers.

## Running the benchmarks

Build `benchmark` and `test_contracts_project` in a Release build, then run from the build directory:

```bash
benchmark/benchmark -f kv_table -r 20 --kv-rows 1000000 10000000
benchmark/benchmark -f kv_ops -r 20 --kv-rows 1000000 10000000
benchmark/benchmark -f kv_contract -r 20
benchmark/benchmark -f kv_token -r 20
```

| Feature | What it measures |
|---|---|
| `kv_table` | `kv::table`, `kv::scoped_table`, `kv::index` and `kv::global` per operation |
| `kv_ops` | `multi_index` per operation, and the cost of one host call |
| `kv_contract` | one action over a whole table, raw intrinsics against `multi_index` |
| `kv_token` | token transfers |

## `kv::table` per operation

Nanoseconds per operation, with rows visited in an order unrelated to how they were stored, so cache misses show.

| Operation | Host calls | 1M rows | 10M rows |
|---|---:|---:|---:|
| get | 1 | 1,233 | 1,692 |
| find | 6 | 2,095 | 2,595 |
| find + modify | 7 | 2,605 | 3,155 |
| modify by key | 2 | 1,754 | 2,226 |
| emplace | 2 | 1,670 | 2,195 |
| find + erase | 10 | 2,677 | 3,316 |
| erase by key | 2 | 1,414 | 1,934 |
| primary scan, per row | 3 | 1,334 | 2,028 |
| secondary scan (`kv::index`), per row | 3 | 2,537 | 4,048 |
| `kv::scoped_table` primary scan, per row | 3 | 1,343 | 1,994 |
| `kv::global` get | 1 | 50 | 50 |

Going from 1M to 10M rows costs 1.2x to 1.6x per operation, except `kv::global` get, which reads one row over and over.

## `kv::table` against `multi_index`

The ratio is `kv::table` divided by `multi_index`, from the same three passes, so below 1 means `kv::table` is faster.

| Operation | 1M `kv::table` | 1M `multi_index` | 1M ratio | 10M `kv::table` | 10M `multi_index` | 10M ratio |
|---|---:|---:|---:|---:|---:|---:|
| get | 1,233 | 1,423 | 0.87 | 1,692 | 2,001 | 0.85 |
| find | 2,095 | 2,481 | 0.84 | 2,595 | 3,149 | 0.82 |
| find + modify | 2,605 | 3,134 | 0.83 | 3,155 | 3,833 | 0.82 |
| emplace | 1,670 | 2,791 | 0.60 | 2,195 | 3,465 | 0.63 |
| find + erase | 2,677 | 4,188 | 0.64 | 3,316 | 5,075 | 0.65 |
| primary scan, per row | 1,334 | 1,504 | 0.89 | 2,028 | 2,161 | 0.94 |
| secondary scan, per row | 2,537 | 3,123 | 0.81 | 4,048 | 4,810 | 0.84 |

Both sides store the same row. A `kv::table` key is the 8-byte id, and a `multi_index` key is 16 bytes with the scope in
front.

## Raw intrinsics against `multi_index`

Microseconds for one action over the whole table.

| Operation | Rows | Raw | `multi_index` | Ratio |
|---|---:|---:|---:|---:|
| Populate | 100 | 25 | 78 | 3.2 |
| Find All | 100 | 9 | 63 | 6.7 |
| Iterate All | 100 | 13 | 44 | 3.3 |
| Update All | 100 | 22 | 91 | 4.1 |
| Erase All | 100 | 21 | 105 | 5.1 |
| Populate | 500 | 96 | 380 | 3.9 |
| Find All | 500 | 37 | 313 | 8.5 |
| Iterate All | 500 | 41 | 210 | 5.1 |
| Update All | 500 | 98 | 459 | 4.7 |
| Erase All | 500 | 71 | 528 | 7.5 |

## Token transfers

Microseconds for one action. Each transfer is two reads and two writes. Fast is `multi_index` on a trivially copyable
row.

| Transfers | `multi_index` | Raw | Fast | `multi_index`/Raw | Fast/Raw |
|---:|---:|---:|---:|---:|---:|
| 10 | 20 | 9 | 20 | 2.2 | 2.2 |
| 50 | 66 | 27 | 64 | 2.4 | 2.4 |
| 100 | 118 | 49 | 120 | 2.4 | 2.5 |
| 500 | 504 | 187 | 509 | 2.7 | 2.7 |

## What these numbers say

- **Use `kv::table`.** It beat `multi_index` on every operation both measure, and the system contracts use it and its
  relatives. `multi_index` is the compatibility shim. In CDT, `sysio::multi_index` is `kv_multi_index`, and `kv::table` is
  `sysio::kv::table<Name, K, V>`, with `sysio::kv::scoped_table` for scoped tables.
- **Crossing into the host is a small part of the cost.** A scan step makes three host calls, about 6 ns each, together
  about 1% of the step. That is a lower bound, as the call measured passes no pointers.
- **Raw intrinsics pay off where a contract does not need a wrapper.** They took 2 to 8.5 times less time than
  `multi_index` in these tests, at the price of writing a storage layer by hand.

## How KV storage works

These are the facts behind the numbers, at the commit above.

- Rows sort by (code, table_id, key) and index entries by (code, table_id, sec_key, pri_key), compared by `kv_key_less`,
  which has an integer fast path for 8-byte keys.
- Keys and values were separate allocations behind offset pointers at that commit. Keys up to 24 bytes have moved inside
  the row since then, and these numbers do not show it. Longer keys and all values are still separate allocations.
- Each row and index entry has a payer, and a payer change moves the charge. `docs/kv-ram-billing.md` has the rules.
- An action has two iterator pools, primary and secondary, each sized on demand up to 1024 slots.
- There is no per-table object like the legacy `table_id_object`, and one index object replaces the five legacy secondary
  index types.
- SHiP reports rows as `contract_row_kv` and index entries as `contract_index_kv`, each with code, payer, table_id and raw
  bytes. Clients resolve `table_id` to a table name through the contract's ABI. Nothing translates them to `contract_row`,
  which the SHiP ABI still declares and nothing emits.
