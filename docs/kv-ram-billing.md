# KV vs Legacy RAM Billing Comparison

## Overview

RAM billing ensures contracts pay for the storage they consume, preventing abuse.
The billable size must cover at minimum the actual RAM used by chainbase objects.
This document compares legacy (db_*_i64) and KV billing for common patterns.

## Constants

- `overhead_per_row_per_index_ram_bytes = 32` (B-tree node overhead per index)

## Legacy Objects

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

| Object | Fixed Fields | Indices | Overhead | Billable |
|--------|-------------|---------|----------|----------|
| `kv_object` | 8 id + 8 code + 8 payer + 8 key (offset_ptr) + 8 value (offset_ptr) + 1 key_format + 7 padding = 48 | 2 | 64 | **112** + key_size + value_size |
| `kv_index_object` | 8 id + 8 code + 8 payer + 8 table + 8 sec_key (offset_ptr) + 8 pri_key (offset_ptr) + 1 index_id + 7 padding = 56 | 2 | 64 | **120** + sec_key_size + pri_key_size |

## KV Key Formats

KV supports two key formats with different RAM trade-offs:

- **Format 1 (standard):** `kv_multi_index` uses 24-byte primary keys:
  `[table:8B][scope:8B][pk:8B]`. Secondary index entries store
  `pri_key = [pk:8B]` and `sec_key = [scope:8B][secondary_value]` so that
  secondary iteration is naturally scoped without chain-side scope parameters.

- **Format 0 (raw):** `kv::raw_table` / `kv::indexed_table` use contract-defined
  key structs, BE-encoded for correct sort order. Key size is controlled by the
  contract. A uint64 primary key is just 8 bytes. This is the most RAM-efficient
  option. Secondary indices use `TableName` for table-level isolation; contracts
  use key discriminators for further partitioning.

## Per-Row Comparisons (16-byte value, no secondary index)

### sysio.token balance — 1 scope per holder (table_id amortized per row)

| Approach | Component Breakdown | Total |
|----------|-------------------|-------|
| Legacy | kvo(108+16) + table_id(108) | **232** |
| kv_multi_index (fmt=1, 24B key) | kv_obj(112+24+16) | **152** (-34%) |
| kv::raw_table (fmt=0, 16B key) | kv_obj(112+16+16) | **144** (-38%) |

### Many rows per table (table_id negligible)

| Approach | Component Breakdown | Total |
|----------|-------------------|-------|
| Legacy | kvo(108+16) | **124** |
| kv_multi_index (fmt=1, 24B key) | kv_obj(112+24+16) | **152** (+23%) |
| kv::raw_table (fmt=0, 8B key) | kv_obj(112+8+16) | **136** (+10%) |

## Per-Row Comparisons (16-byte value, 1 uint64 secondary index)

### 1 scope per row (table_id amortized per row)

| Approach | Primary | Secondary | Total |
|----------|---------|-----------|-------|
| Legacy | 124 + 108 tid | 128 | **360** |
| kv_multi_index (fmt=1) | 152 | 120+16+8=144 | **296** (-18%) |
| kv::raw_table (fmt=0, 8B key) | 136 | 120+8+8=136 | **272** (-24%) |

### Many rows per table

| Approach | Primary | Secondary | Total |
|----------|---------|-----------|-------|
| Legacy | 124 | 128 | **252** |
| kv_multi_index (fmt=1) | 152 | 144 | **296** (+17%) |
| kv::raw_table (fmt=0, 8B key) | 136 | 136 | **272** (+8%) |

Note: `kv_index_object` has 2 chainbase indices (by_id, by_code_table_idx_seckey).
For format 1, `sec_key = [scope:8B][secondary_value]` and `pri_key = [pk:8B]`.
For format 0, both `sec_key` and `pri_key` are contract-defined compact keys.

## EOS Mainnet Estimate

Estimated using EOS v8 snapshot (`snapshot-2026-03-10-19-eos-v8-0487758850.bin`).
Secondary index counts: 13.3M idx64, 3.9M idx128, 355.6M idx256 (345M from xsat),
47K idx_double, 5K idx_long_double.

### Excluding xsat (~103M primary rows, ~28M secondary entries)

| Approach | Total | vs Legacy |
|----------|-------|-----------|
| Legacy | 14.4 GB | baseline |
| kv_multi_index (fmt=1) | 15.1 GB | **+5%** |
| kv::raw_table (fmt=0, ~10B avg key) | 13.8 GB | **-4%** |

### Including xsat (~262M primary rows, ~373M secondary entries)

Secondary index storage alone (primary rows unchanged by this PR):

| Approach | Secondary Only | vs Legacy |
|----------|---------------|-----------|
| Legacy | 56.3 GB | baseline |
| kv_multi_index (fmt=1) | 62.3 GB | **+11%** |
| kv::raw_table (fmt=0) | 59.3 GB | **+5%** |

The `kv_multi_index` premium comes from 24-byte standard keys replacing
legacy's 8-byte fixed `uint64_t primary_key` and scope-prefixed secondary keys.
With `kv::raw_table`, compact keys reduce this gap. Dropping the unused
`by_code_table_idx_prikey` index saves 32 bytes per secondary entry (11.9 GB
across all 373M entries including xsat).

## Summary

| Scenario | Legacy | kv_multi_index | kv::raw_table |
|----------|--------|----------------|---------------|
| Token balance (1 scope/row, no idx) | 232 | 152 (-34%) | 144 (-38%) |
| Token balance + 1 idx (1 scope/row) | 360 | 296 (-18%) | 272 (-24%) |
| Dense table (no idx) | 124 | 152 (+23%) | 136 (+10%) |
| Dense table + 1 idx | 252 | 296 (+17%) | 272 (+8%) |

**Bottom line:** Dropping the unused `by_code_table_idx_prikey` chainbase index
reduced `kv_index_object` overhead from 152 to 120 bytes per secondary entry,
significantly improving secondary index RAM costs. `kv::raw_table` with compact
keys now saves 8-24% vs legacy for tables with secondary indices.
`kv_multi_index` saves 18% for scoped patterns and adds only 17% for dense
tables — a major improvement from the previous 33% premium. Contracts without
secondary indices and with many scopes (like sysio.token) save 34-38%
regardless of format, thanks to `table_id_object` elimination.
