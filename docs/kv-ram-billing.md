# KV vs Legacy RAM Billing Comparison

## Overview

RAM billing ensures contracts pay for the storage they consume, preventing abuse.
The billable size must cover at minimum the actual RAM used by chainbase objects.
This document compares legacy EOSIO (`db_*_i64`) and Wire KV billing for common patterns.

## Constants

- `overhead_per_row_per_index_ram_bytes = 32` (B-tree node overhead per index)

## Legacy Objects (EOSIO db\_\*\_i64)

| Object | Fixed Fields | Indices | Overhead | Billable |
|--------|-------------|---------|----------|----------|
| `table_id_object` | 8 code + 8 scope + 8 table + 8 payer + 4 count = 36 | 2 | 64 | **108** |
| `key_value_object` | 8 t_id + 8 pk + 8 payer + 12 value_header = 36 | 2 | 64 | **108** + value_size |
| `index64_object` | 8 t_id + 8 pk + 8 payer + 8 secondary = 32 | 3 | 96 | **128** |
| `index128_object` | 8 t_id + 8 pk + 8 payer + 16 secondary = 40 | 3 | 96 | **136** |
| `index256_object` | 8 t_id + 8 pk + 8 payer + 32 secondary = 56 | 3 | 96 | **152** |

## KV Objects

Keys and values are stored as `shared_blob` (8-byte `offset_ptr` into chainbase
shared memory). Key data sizes are billed separately at billing time.

Each table and secondary index gets a unique `table_id` (uint16), providing
namespace isolation via the composite chainbase index. No `table_id_object` overhead.

| Object | Fixed Fields | Indices | Overhead | Billable |
|--------|-------------|---------|----------|----------|
| `kv_object` | 8 id + 8 code + 8 payer + 8 key (ptr) + 8 value (ptr) + 2 table_id + 6 pad = **48** | 2 | 64 | **112** + key_size + value_size |
| `kv_index_object` | 8 id + 8 code + 8 payer + 8 sec_key (ptr) + 8 pri_key (ptr) + 2 table_id + 6 pad = **48** | 2 | 64 | **112** + sec_key_size + pri_key_size |

## KV Key Layouts

- **multi_index:** Primary key `[scope:8B][pk:8B]` = 16 bytes. Secondary index
  entries store `pri_key = [pk:8B]` and `sec_key = [scope:8B][secondary_value]`
  so that secondary iteration is naturally scoped.

- **kv::table:** Contract-defined key structs, BE-encoded for correct sort order.
  Key size is controlled by the contract. A uint64 primary key is just 8 bytes.
  This is the most RAM-efficient option.

## Per-Row Comparisons (16-byte value, no secondary index)

### sysio.token balance — 1 scope per holder (table\_id amortized per row)

| Approach | Component Breakdown | Total |
|----------|-------------------|-------|
| Legacy | kvo(108+16) + table_id(108) | **232** |
| KV multi_index (16B key) | kv_obj(112+16+16) | **144** (-38%) |
| KV kv::table (8B key) | kv_obj(112+8+16) | **136** (-41%) |

### Many rows per table (table\_id negligible)

| Approach | Component Breakdown | Total |
|----------|-------------------|-------|
| Legacy | kvo(108+16) | **124** |
| KV multi_index (16B key) | kv_obj(112+16+16) | **144** (+16%) |
| KV kv::table (8B key) | kv_obj(112+8+16) | **136** (+10%) |

## Per-Row Comparisons (16-byte value, 1 uint64 secondary index)

### 1 scope per row (table\_id amortized per row)

| Approach | Primary | Secondary | Total |
|----------|---------|-----------|-------|
| Legacy | 124 + 108 tid | 128 | **360** |
| KV multi_index | 144 | 112+16+8=136 | **280** (-22%) |
| KV kv::table (8B key) | 136 | 112+8+8=128 | **264** (-27%) |

### Many rows per table

| Approach | Primary | Secondary | Total |
|----------|---------|-----------|-------|
| Legacy | 124 | 128 | **252** |
| KV multi_index | 144 | 136 | **280** (+11%) |
| KV kv::table (8B key) | 136 | 128 | **264** (+5%) |

Note: `kv_index_object` has 2 chainbase indices (by_id, by_code_table_id_seckey).
For multi_index, `sec_key = [scope:8B][secondary_value]` and `pri_key = [pk:8B]`.
For kv::table, both `sec_key` and `pri_key` are contract-defined compact keys.

## EOS Mainnet Estimate

Estimated using EOS v8 snapshot (`snapshot-2026-03-10-19-eos-v8-0487758850.bin`).
Secondary index counts: 13.3M idx64, 3.9M idx128, 355.6M idx256 (345M from xsat),
47K idx_double, 5K idx_long_double.

### Excluding xsat (~103M primary rows, ~28M secondary entries)

| Approach | Total | vs Legacy |
|----------|-------|-----------|
| Legacy | 14.4 GB | baseline |
| KV multi_index (16B key) | ~14.1 GB | **-2%** |
| KV kv::table (~10B avg key) | ~13.6 GB | **-6%** |

### Including xsat (~262M primary rows, ~373M secondary entries)

Secondary index storage alone (primary rows unchanged):

| Approach | Secondary Only | vs Legacy |
|----------|---------------|-----------|
| Legacy | 56.3 GB | baseline |
| KV multi_index | ~59.3 GB | **+5%** |
| KV kv::table | ~56.3 GB | **±0%** |

KV secondary index storage is larger than legacy for xsat because legacy's
`index256_object` stores 32-byte secondary keys inline (no offset pointer
overhead), while KV always uses `shared_blob` offset pointers. For idx64
entries (the common case outside xsat), KV is more efficient due to fewer
chainbase indices (2 vs 3 → 32 bytes less overhead per entry).

## Summary

| Scenario | Legacy | KV multi_index | KV kv::table |
|----------|--------|----------------|--------------|
| Token balance (1 scope/row, no idx) | 232 | 144 (**-38%**) | 136 (**-41%**) |
| Token balance + 1 idx (1 scope/row) | 360 | 280 (**-22%**) | 264 (**-27%**) |
| Dense table (no idx) | 124 | 144 (+16%) | 136 (+10%) |
| Dense table + 1 idx | 252 | 280 (+11%) | 264 (+5%) |

**Bottom line:** KV saves 22-41% for scoped patterns like token balances,
primarily from eliminating the `table_id_object` (108 bytes per scope).
For dense tables with many rows and few scopes, KV adds 5-16% overhead
from variable-length key storage. `kv::table` with compact custom keys
consistently outperforms `multi_index` by 6-8 bytes per row.
On the EOS mainnet (excluding xsat), KV saves 2-6% overall — an improvement
over the previous +5% premium, achieved by reducing primary keys from 24B to
16B and `kv_index_object` from 56B to 48B. Including xsat's 373M idx256
secondary entries, KV secondary storage is roughly break-even with legacy
for `kv::table` and +5% for `multi_index`.
