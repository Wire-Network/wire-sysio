# KV vs Legacy RAM Billing Comparison

## Overview

RAM billing ensures contracts pay for the storage they consume, preventing abuse.
The billable size is a protocol price set close to the RAM chainbase objects use. It does not follow their layout, so
what is stored can sit above or below it. KV Objects gives the bound.
This document compares legacy EOSIO (`db_*_i64`) and Wire KV billing for common patterns,
and states the KV billing rules. `docs/kv-behavior-spec.md` links each rule to its test.

## Constants

- `overhead_per_row_per_index_ram_bytes = 32` (B-tree node overhead per index)
- `kv_row_overhead_ram_bytes = 112` (billed per KV row)
- `kv_index_entry_overhead_ram_bytes = 112` (billed per KV secondary index entry)

The two KV constants are protocol prices. They were first derived from an object layout these objects no longer have,
and they do not follow it: changing the layout or the storage engine does not change what a row costs.

## Legacy Objects (EOSIO db\_\*\_i64)

| Object | Fixed Fields | Indices | Overhead | Billable |
|--------|-------------|---------|----------|----------|
| `table_id_object` | 8 id + 8 code + 8 scope + 8 table + 8 payer + 4 count = 44 | 2 | 64 | **108** |
| `key_value_object` | 8 id + 8 t_id + 8 pk + 8 payer + 12 value_header = 44 | 2 | 64 | **108** + value_size |
| `index64_object` | 8 t_id + 8 pk + 8 payer + 8 secondary = 32 | 3 | 96 | **128** |
| `index128_object` | 8 t_id + 8 pk + 8 payer + 16 secondary = 40 | 3 | 96 | **136** |
| `index256_object` | 8 t_id + 8 pk + 8 payer + 32 secondary = 56 | 3 | 96 | **152** |

## KV Objects

Keys up to 24 bytes are stored inside the row in a 24-byte `kv_key_blob`, and longer keys and all values in a
`shared_blob` (8-byte `offset_ptr` into chainbase shared memory). A key's size sits in the row, in the padding after
`table_id`, so the slot holds nothing but the key. Key data sizes are billed separately at billing time.

Each table and secondary index has a `table_id` (uint16), providing namespace
isolation via the composite chainbase index. No `table_id_object` overhead. The chain
does not enforce uniqueness; CDT codegen rejects collisions within one contract ABI.

| Object | Billable |
|--------|----------|
| `kv_object` | **112** + key_size + value_size |
| `kv_index_object` | **112** + sec_key_size + pri_key_size |

The 112 is a protocol constant: 48 bytes of fixed fields plus 64 of overhead in the layout these objects had when the
price was set. It does not follow the layout. A row now takes a 104-byte node and an index entry a 120-byte one, each
with its keys inside when they fit in 24 bytes. A longer key adds a blob of 8 + key_size bytes, rounded up to 8, on top
of the 24-byte slot it no longer uses.

So the price sits above what is stored for keys of 8 to 24 bytes, except for a few bytes on a row whose value lands just
past a rounding boundary. It sits below what is stored for keys under 8 bytes, and for most shapes with a key past 24
bytes. Stored minus billed is positive where a row stores more than it pays for. Stored here is measured: bytes of state
per row over 20,000 rows, which includes 3 to 5 bytes of allocator bookkeeping per row, and 14 at 512 + 512 byte keys.
`stored_size_matches_the_billing_doc` in `unittests/kv_store_tests.cpp` pins these numbers.

| Shape | Stored | Billed | Stored minus billed |
|-------|---:|---:|---:|
| index entry, 24 + 24 byte keys | 123 | 160 | -37 |
| index entry, 16 + 16 | 123 | 144 | -21 |
| index entry, 8 + 8 | 123 | 128 | -5 |
| index entry, 4 + 4 | 123 | 120 | +3 |
| index entry, 32 + 8 (`checksum256` over a `uint64_t`) | 164 | 152 | +12 |
| index entry, 64 + 64 | 268 | 240 | +28 |
| index entry, 31 + 31 | 204 | 174 | +30 |
| index entry, 30 + 30 | 204 | 172 | +32 |
| index entry, 512 + 512 | 1,174 | 1,136 | +38 |
| index entry, 25 + 25, the worst case | 204 | 162 | +42 |
| index entry, 33 + 33, the worst case | 220 | 178 | +42 |
| row, 24 + 1 value | 123 | 137 | -14 |
| row, 16 + 64 value | 180 | 192 | -12 |
| row, 8 + 8 value | 123 | 128 | -5 |
| row, 8 + 1 value | 123 | 121 | +2 |
| row, 4 + 1 value | 123 | 117 | +6 |
| row, 31 + 1 value | 164 | 144 | +20 |
| row, 25 + 1 value, the worst case | 164 | 138 | +26 |
| row, 33 + 1 value, the worst case | 172 | 146 | +26 |

A long key's blob is rounded up to 8 bytes, so 25, 33 and every 8th length after them are the worst: 7 bytes of rounding
on top of the blob's 8-byte header, with the 24-byte slot unused. At any other length the excess stays near these
figures, and it shrinks as a share of the row. A value's blob is rounded the same way, which is why the 8 + 1 row is short
where the 8 + 8 row is not.

## KV Billing Rules

These apply to rows and index entries alike. A row's charge is its key size plus its value
size plus 112, and an index entry's is its secondary key size plus its primary key size plus 112.

- **Insert:** the named payer is charged in full. Payer 0 is rejected with `invalid_table_payer`,
  because a new row has no payer to keep.
- **Update, same payer:** only the size difference is billed. A same-size update bills nothing.
- **Update, payer change:** the old payer is refunded its full old charge, and the new payer is
  charged its full new charge.
- **Update, payer 0:** the row keeps its payer.
- **Erase:** the row's current payer is refunded in full.

A payer other than the receiver is checked after each action by `validate_account_ram_deltas`.
For an unprivileged receiver, an account whose RAM grew must have authorized the action, and
must have done so with its `sysio.payer` permission unless an exemption applies.

## KV Key Layouts

- **multi_index:** Primary key `[scope:8B][pk:8B]` = 16 bytes. Secondary index
  entries store `pri_key = [pk:8B]` and `sec_key = [scope:8B][secondary_value]`
  so that secondary iteration is naturally scoped.

- **kv::table:** Contract-defined key structs, BE-encoded for correct sort order.
  Key size is controlled by the contract. A uint64 primary key is just 8 bytes.
  This is the most RAM-efficient option.

## Per-Row Comparisons (16-byte value, no secondary index)

### sysio.token balance: 1 scope per holder (table\_id amortized per row)

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

The `multi_index` figure is higher than legacy for xsat because its `sec_key` carries the 8-byte scope in front of the
32-byte secondary value: 112+40+8 = 160 against legacy's 152. A `kv::table` entry has no scope in its secondary key and
costs 112+32+8 = 152, the same as legacy. For idx64 entries, `kv::table` also costs the same as legacy (128), and
`multi_index` costs 8 bytes more (136).

## Summary

| Scenario | Legacy | KV multi_index | KV kv::table |
|----------|--------|----------------|--------------|
| Token balance (1 scope/row, no idx) | 232 | 144 (**-38%**) | 136 (**-41%**) |
| Token balance + 1 idx (1 scope/row) | 360 | 280 (**-22%**) | 264 (**-27%**) |
| Dense table (no idx) | 124 | 144 (+16%) | 136 (+10%) |
| Dense table + 1 idx | 252 | 280 (+11%) | 264 (+5%) |

**Bottom line:** KV saves 22-41% for scoped patterns like token balances,
primarily from eliminating the `table_id_object` (108 bytes per scope). For dense tables with
many rows and few scopes, KV adds 5-16%. `kv::table` with compact custom keys
consistently outperforms `multi_index`, by 8 bytes per primary row and 16 bytes per row with
one secondary index.
On the EOS mainnet estimate, excluding xsat, KV bills 2-6% less than legacy. For xsat's
373M mostly idx256 secondary entries it bills the same as legacy on `kv::table` and 5% more on `multi_index`.
