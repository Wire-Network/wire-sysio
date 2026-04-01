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

| Object | Fixed Fields | Indices | Overhead | Billable |
|--------|-------------|---------|----------|----------|
| `kv_object` | 8 id + 8 code + 8 payer + 1 key_format + 2 key_size + 24 key_inline + 12 key_heap + 12 value_header + 5 padding = 80 | 2 | 64 | **144** + key_size + value_size |
| `kv_index_object` | 8 code + 8 payer + 8 table + 16 sec_key_heap + 16 pri_key_heap + 24 sec_key_inline + 24 pri_key_inline + 2+2 key_sizes + 1 index_id + 3 padding = 112 | 3 | 96 | **208** + sec_key_size + pri_key_size |

## Per-Row Comparison: sysio.token Balance (uint64_t key, 16-byte value, 1 secondary)

### Legacy

| Component | Bytes |
|-----------|-------|
| `table_id_object` (per scope, amortized) | 108 |
| `key_value_object` (108 + 16 value) | 124 |
| `index64_object` (secondary idx) | 128 |
| **Total per row** | **360** |

### KV

| Component | Bytes |
|-----------|-------|
| No table_id_object needed | 0 |
| `kv_object` (144 + 24 key + 16 value) | 184 |
| `kv_index_object` (208 + 8 sec_key + 16 pri_key) | 232 |
| **Total per row** | **416** |

### Difference: +56 bytes per row (+15.6%)

## Per-Row Comparison: Simple KV (no secondary index)

### Legacy

| Component | Bytes |
|-----------|-------|
| `table_id_object` (per scope) | 108 |
| `key_value_object` (108 + value_size) | 108 + value_size |
| **Total per row** | **216** + value_size |

### KV

| Component | Bytes |
|-----------|-------|
| `kv_object` (144 + key_size + value_size) | 144 + key_size + value_size |
| **Total per row** | **144** + key_size + value_size |

### Difference: -72 bytes per row (-33%) for typical 24-byte keys

## Real-World Example: sysio.token Balance Rows

sysio.token uses `sysio::kv::table` (no secondary indices). Each balance row
stores a 24-byte key and a 16-byte value (asset).

### Per Balance Row

| Component | Legacy | KV |
|-----------|--------|-----|
| `table_id_object` (1 per account scope) | 108 | 0 |
| Row object overhead | 108 | 144 |
| Key data | 0 (pk is 8B fixed field) | 24 |
| Value data (asset) | 16 | 16 |
| **Total per balance row** | **232** | **184** |
| **Savings** | | **-21% per row** |

### At Scale (10,000 token holders)

| | Legacy | KV |
|--|--------|-----|
| Row storage (10K rows) | 10,000 x 124 = 1,240,000 | 10,000 x 184 = 1,840,000 |
| table_id overhead (10K scopes) | 10,000 x 108 = 1,080,000 | 0 |
| **Total** | **2,320,000 bytes (2.2 MB)** | **1,840,000 bytes (1.8 MB)** |
| **Savings** | | **-21% total (480 KB saved)** |

The savings grow with more token holders because legacy adds 108 bytes of
`table_id_object` overhead per account scope, while KV adds zero.

### At Scale (100,000 token holders)

| | Legacy | KV |
|--|--------|-----|
| **Total** | **23,200,000 bytes (22.1 MB)** | **18,400,000 bytes (17.5 MB)** |
| **Savings** | | **-21% (4.6 MB saved)** |

## Summary

| Scenario | Legacy | KV | Delta |
|----------|--------|----|-------|
| sysio.token balance row (no secondary) | 232 | 184 | **-21%** |
| Row + 1 secondary (uint64) | 360 | 397 | +10% |
| Row + 1 secondary (uint128) | 368 | 397 | +8% |
| Per-scope overhead (table_id) | 108 | 0 | **-100%** |
| 10K token holders total | 2.2 MB | 1.8 MB | **-21%** |
| 100K token holders total | 22.1 MB | 17.5 MB | **-21%** |

**Bottom line:** KV is cheaper for all contracts that don't use secondary indices
(like sysio.token). For contracts with secondary indices, KV is more expensive
per-row (+8-10%) but eliminates the per-scope `table_id_object` overhead
entirely, making it cheaper overall for contracts with many scopes.
