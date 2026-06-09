# get_table_rows API

Query table rows by primary key or secondary index. Works with all table types (`kv::table`, `kv::scoped_table`, `kv_multi_index`, `kv::global`).

## Endpoint

```
POST /v1/chain/get_table_rows
```

## Parameters

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `json` | bool | `true` | ABI-decode keys and values (`true`) or return raw hex (`false`) |
| `code` | name | required | Contract account |
| `table` | string | required | Table name (from ABI) |
| `scope` | string | `""` | Scope for scoped tables. Empty = unscoped (all rows). Parsed using ABI type. |
| `find` | string | `""` | Exact key lookup (JSON key object or hex). Cannot be combined with bounds. |
| `index_name` | string | `""` | Secondary index name (e.g. `"byowner"`) or position (e.g. `"2"`). Empty = primary key. |
| `lower_bound` | string | `""` | Lower bound (inclusive). JSON key object when `json=true`, hex when `json=false`. |
| `upper_bound` | string | `""` | Upper bound (exclusive). Same format as `lower_bound`. |
| `limit` | uint32 | `50` | Max rows to return |
| `reverse` | bool | `false` | Iterate in reverse order |
| `show_payer` | bool | `false` | Include RAM payer in each row |
| `time_limit_ms` | uint32 | (server default) | Max processing time |

## Response

```json
{
   "rows": [
      { "key": {...}, "value": {...}, "payer": "alice" }
   ],
   "more": false,
   "next_key": ""
}
```

- `rows` — array of `{key, value}` objects. When `show_payer=true`, includes `payer` field.
- `more` — `true` if there are more rows beyond `limit`.
- `next_key` — use as `lower_bound` for the next page. Scope is stripped (pass same `scope` param).

## Scoped Queries

For scoped tables (`kv::scoped_table`, `kv_multi_index`), use the `scope` parameter:

### All rows in a scope

```json
{
   "code": "sysio.token",
   "table": "accounts",
   "scope": "alice"
}
```

Returns all token balances for account `alice`.

### Scope parsing

The scope string is parsed using the ABI's declared scope type (fixes [AntelopeIO/spring#1379](https://github.com/AntelopeIO/spring/issues/1379)):
- When ABI scope type is `"name"`: `"alice"` → name encoding. Fallback to uint64 for values like `"0"` (= `name{}`).
- When ABI scope type is `"uint64"`: parsed as uint64 directly.
- When ABI scope type is `"symbol_code"`: parsed as symbol (e.g., `"4,SYS"`).

### All scopes (no scope filter)

```json
{
   "code": "sysio.token",
   "table": "accounts"
}
```

Returns all balances across all accounts, ordered by `(scope, primary_key)`.

## Exact Key Lookup (`find`)

Look up a single row by exact key:

```json
{
   "code": "sysio.token",
   "table": "accounts",
   "scope": "alice",
   "find": {"primary_key": 1397703940}
}
```

Where `1397703940` is `symbol_code("SYS").raw()`. Returns alice's SYS balance.

`find` cannot be combined with `lower_bound` or `upper_bound`.

## Primary Key Queries

### Bounded range

Bounds are JSON objects with field names matching the table's `key_names` from the ABI. For scoped tables with `scope` set, bounds represent the within-scope key (scope is prepended automatically).

```json
{
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

Query by secondary index using `index_name`. Accepts index names (`"byowner"`) or numeric positions (`"2"` = first secondary).

### By name

```json
{
   "code": "mycontract",
   "table": "users",
   "index_name": "byowner",
   "lower_bound": {"byowner": "alice"},
   "upper_bound": {"byowner": "alicf"}
}
```

### By position (backward compat)

```json
{
   "code": "mycontract",
   "table": "users",
   "index_name": "2"
}
```

Position 1 = primary, 2 = first secondary, 3 = second secondary, etc.

### Reverse iteration

```json
{
   "code": "mycontract",
   "table": "users",
   "index_name": "byowner",
   "reverse": true
}
```

## Pagination

When `more` is `true`, use `next_key` as `lower_bound` for the next request. Keep the same `scope` parameter:

```json
{
   "code": "sysio.token",
   "table": "accounts",
   "scope": "alice",
   "lower_bound": "...(next_key from previous response)...",
   "limit": 50
}
```

`next_key` is scope-stripped — the scope prefix is not included. Just pass it back as `lower_bound` with the same `scope`.

## RAM Payer

```json
{
   "code": "sysio.token",
   "table": "accounts",
   "scope": "alice",
   "show_payer": true
}
```

Response rows include `"payer"` field:
```json
{"key": {"primary_key": 1397703940}, "value": {"balance": "100.0000 SYS"}, "payer": "sysio"}
```

## ABI Requirements

The contract's ABI must include the table definition with `table_id`, `key_names`, and `key_types`. For `kv::table`/`kv::scoped_table` with `_n` names, the ABI is auto-generated. For `_i` names, add `[[sysio::table("name")]]` to the value struct.

Secondary index queries require `secondary_indexes` in the ABI:

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

Scoped tables have `"scope"` as the first entry in `key_names`:

```json
{
   "name": "accounts",
   "type": "account",
   "table_id": 25660,
   "key_names": ["scope", "primary_key"],
   "key_types": ["name", "uint64"]
}
```

## Migration from old get_table_rows

| Old param | New equivalent |
|-----------|---------------|
| `scope` (required positional) | `scope` (optional field) |
| `index_position` | `index_name` (accepts names or numbers) |
| `key_type` | Not needed (inferred from ABI) |
| `encode_type` | Not needed |
| `table_key` | Not needed |

Old params are silently ignored if present — existing curl scripts continue to work for simple queries. The main breaking change is the response format: rows are now `{key, value}` objects instead of bare value objects.
