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
| `kv_index_object` | 8 id + 8 code + 8 payer + 8 table + 8 sec_key (offset_ptr) + 8 pri_key (offset_ptr) + 1 index_id + 7 padding = 56 | 3 | 96 | **152** + sec_key_size + pri_key_size |

## KV Key Formats

KV supports two key formats with different RAM trade-offs:

- **Format 1 (standard):** `kv_multi_index` uses 24-byte keys: `[table:8B][scope:8B][pk:8B]`.
  Embeds legacy table/scope routing into the key. Compatible with SHiP table
  translation and existing tooling.

- **Format 0 (raw):** `kv::raw_table` uses contract-defined key structs,
  BE-encoded for correct sort order. Key size is controlled by the contract.
  A uint64 primary key is just 8 bytes. This is the most RAM-efficient option.

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
| kv_multi_index (fmt=1) | 152 | 152+8+24=184 | **336** (-7%) |
| kv::raw_table (fmt=0, 8B key) | 136 | 152+8+8=168 | **304** (-16%) |

### Many rows per table

| Approach | Primary | Secondary | Total |
|----------|---------|-----------|-------|
| Legacy | 124 | 128 | **252** |
| kv_multi_index (fmt=1) | 152 | 184 | **336** (+33%) |
| kv::raw_table (fmt=0, 8B key) | 136 | 168 | **304** (+21%) |

Note: with raw_table, the `pri_key` stored in `kv_index_object` is also compact
(8 bytes) vs 24 bytes for standard format. This compounds the savings per
secondary index row.

## EOS Mainnet Estimate (excluding xsat, ~103M primary rows)

Estimated using an EOS v8 snapshot with xsat Bitcoin UTXO data excluded
(xsat alone has 345M index256 rows that dominate the dataset).

| Approach | Total | vs Legacy |
|----------|-------|-----------|
| Legacy | 14.4 GB | baseline |
| kv_multi_index (fmt=1) | 16.0 GB | **+11%** |
| kv::raw_table (fmt=0, ~10B avg key) | 14.7 GB | **+2%** |

The +11% for `kv_multi_index` comes from 24-byte standard keys replacing
legacy's 8-byte fixed `uint64_t primary_key`. With `kv::raw_table`, compact
keys eliminate this gap.

## Summary

| Scenario | Legacy | kv_multi_index | kv::raw_table |
|----------|--------|----------------|---------------|
| Token balance (1 scope/row, no idx) | 232 | 152 (-34%) | 144 (-38%) |
| Token balance + 1 idx (1 scope/row) | 360 | 336 (-7%) | 304 (-16%) |
| Dense table (no idx) | 124 | 152 (+23%) | 136 (+10%) |
| Dense table + 1 idx | 252 | 336 (+33%) | 304 (+21%) |
| EOS mainnet (no xsat) | 14.4 GB | 16.0 GB (+11%) | 14.7 GB (+2%) |

**Bottom line:** `kv::raw_table` with compact keys achieves near-parity with
legacy (+2% on EOS-scale data) while providing variable-length key flexibility.
`kv_multi_index` pays a ~11-33% premium for the convenience of embedding
table/scope routing in 24-byte standard keys. Contracts without secondary
indices and with many scopes (like sysio.token) save 34-38% regardless of
format, thanks to `table_id_object` elimination.
