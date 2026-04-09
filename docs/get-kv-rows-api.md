# get_kv_rows API

Query KV table rows by primary key or secondary index. Works with all KV table types (`kv::table`, `kv_multi_index`, `kv::global`).

## Endpoint

```
POST /v1/chain/get_kv_rows
```

## Parameters

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `json` | bool | `true` | ABI-decode keys and values (`true`) or return raw hex (`false`) |
| `code` | name | required | Contract account |
| `table` | name | required | Table name (from ABI) |
| `index_name` | string | `""` | Secondary index name (e.g. `"byowner"`). Empty = primary key query |
| `lower_bound` | string | `""` | Lower bound (inclusive). JSON key object when `json=true`, hex when `json=false` |
| `upper_bound` | string | `""` | Upper bound (exclusive). Same format as `lower_bound` |
| `limit` | uint32 | `10` | Max rows to return |
| `reverse` | bool | `false` | Iterate in reverse order |
| `time_limit_ms` | uint32 | (server default) | Max processing time |

## Response

```json
{
   "rows": [
      { "key": "...", "value": {...} }
   ],
   "more": false,
   "next_key": ""
}
```

- `rows` — array of `{key, value}` objects. When `json=true`, values are ABI-decoded.
- `more` — `true` if there are more rows beyond `limit`.
- `next_key` — use as `lower_bound` for the next page.

## Primary Key Queries

Query by primary key (default when `index_name` is empty).

### All rows (json mode)

```json
{
   "json": true,
   "code": "mycontract",
   "table": "users"
}
```

### Bounded range (json mode)

Bounds are JSON objects with field names matching the table's `key_names` from the ABI:

```json
{
   "json": true,
   "code": "mycontract",
   "table": "users",
   "lower_bound": {"id": 10},
   "upper_bound": {"id": 20}
}
```

### Hex mode

When `json=false`, bounds are hex-encoded big-endian key bytes:

```json
{
   "json": false,
   "code": "mycontract",
   "table": "users",
   "lower_bound": "000000000000000a",
   "upper_bound": "0000000000000014"
}
```

## Secondary Index Queries

Query by secondary index using `index_name`. The secondary index must be defined in the contract's ABI `secondary_indexes` array.

### All rows by index

```json
{
   "json": true,
   "code": "mycontract",
   "table": "users",
   "index_name": "byowner"
}
```

Returns all rows ordered by the `byowner` secondary key. Each row contains the primary key bytes and the ABI-decoded value.

### Filtered by secondary key bounds (json mode)

Bounds are JSON objects with the index field name and value:

```json
{
   "json": true,
   "code": "mycontract",
   "table": "users",
   "index_name": "byowner",
   "lower_bound": {"byowner": "alice"},
   "upper_bound": {"byowner": "alicf"}
}
```

Returns only rows where the `byowner` secondary key falls in `[alice, alicf)`.

### Reverse iteration

```json
{
   "json": true,
   "code": "mycontract",
   "table": "users",
   "index_name": "byowner",
   "reverse": true
}
```

### Hex mode with secondary index

```json
{
   "json": false,
   "code": "mycontract",
   "table": "users",
   "index_name": "byowner",
   "lower_bound": "0000000000855c34",
   "upper_bound": "0000000000855c35"
}
```

## Pagination

When `more` is `true`, use `next_key` as `lower_bound` for the next request:

```json
{
   "json": true,
   "code": "mycontract",
   "table": "users",
   "lower_bound": "...(next_key from previous response)...",
   "limit": 10
}
```

## ABI Requirements

The contract's ABI must include the table definition. For `kv::table` with `_n` names, the ABI is auto-generated — no annotations needed. For `_i` names, add `[[sysio::table("name")]]` to the value struct.

Secondary index queries require `secondary_indexes` in the ABI table entry:

```json
{
   "name": "users",
   "type": "user_val",
   "table_id": 59372,
   "key_names": ["id"],
   "key_types": ["uint64"],
   "secondary_indexes": [
      {"name": "byowner", "key_type": "name", "table_id": 37799}
   ]
}
```

The CDT auto-generates `secondary_indexes` from `kv::table` template parameters.

## Using with multi\_index Tables

`get_kv_rows` works with `multi_index` tables too. The key layout is `[scope:8B BE][pk:8B BE]` and the ABI has `key_names: ["scope", "primary_key"]`, so bounds use those field names:

### All rows for a scope

```json
{
   "json": true,
   "code": "sysio.token",
   "table": "accounts",
   "lower_bound": {"scope": "alice", "primary_key": 0},
   "upper_bound": {"scope": "alicf", "primary_key": 0}
}
```

Returns all token balances for `alice` (equivalent to `get_table_rows` with `scope=alice`).

### Specific row lookup

```json
{
   "json": true,
   "code": "sysio.token",
   "table": "accounts",
   "lower_bound": {"scope": "alice", "primary_key": 1397703940},
   "upper_bound": {"scope": "alice", "primary_key": 1397703941}
}
```

Where `1397703940` is `symbol_code("SYS").raw()`. Returns alice's SYS balance.

### All scopes (no bounds)

```json
{
   "json": true,
   "code": "sysio.token",
   "table": "accounts"
}
```

Returns all token balances across all accounts, ordered by `(scope, primary_key)`.

## Comparison with get_table_rows

| Feature | `get_table_rows` | `get_kv_rows` |
|---------|-----------------|---------------|
| Primary key layout | `[scope:8B][pk:8B]` only | Any (user-defined key struct) |
| Scope parameter | Required | Not used (scope is a key field) |
| Secondary queries | By position (1, 2, 3...) | By name (`"byowner"`) |
| Key bounds | Scalar values | JSON key objects or hex |
| Works with | `multi_index`, `kv::table` (scope-first) | Any `kv::table` |

Use `get_table_rows` for backward compatibility with `multi_index` contracts. Use `get_kv_rows` for `kv::table` contracts with custom keys or named secondary indices.
