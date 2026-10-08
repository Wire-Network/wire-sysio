# query.execute schemas

Both schemas use JSON Schema Draft-04 and are validated with the repository's RapidJSON validator.
The response contract is version `1.1`. Shape or encoding changes require a new version.
The request schema describes the single-request profile; method validation and SQL semantics occur
in the service. Notifications have no response document (HTTP 204, empty body).

Request `params` hold the required `query` and the optional paging members `limit` and `offset`
(integers in [0, 9007199254740991]) and `timeout_ms` (an integer of at least 1). The schema bounds
`timeout_ms` only by that range; the service additionally rejects a value above the configured
`query-timeout-ms` as INVALID_PARAMS, because a request may only lower the deadline. Request
integers are exact JSON numbers; response counters are unsigned decimal strings.

Success and error envelopes are mutually exclusive. A successful result is always complete for the
submitted SQL, including its explicit LIMIT. Budget/decode failures contain only an error.
`page` describes the returned window over the complete evaluated output: the requested `offset`,
the effective `limit` (the smaller of SQL LIMIT and the request limit, or null), `returned_rows`
(equal to `stats.returned_rows`), `total_rows` (rows passing WHERE, or groups passing HAVING) and
`has_more`, which is true exactly when `min(offset, total_rows) + returned_rows < total_rows`. A
page with `has_more: true` is still `complete`: nothing was truncated, more rows simply lie beyond
the window.

`source.owners` lists distinct contract accounts in chain-name order. `state.abis` contains the
matching owner/hash entries in the same order. Every row was captured at `state.block_id` in one
read-only callback. The state can advance or switch forks before the response arrives; no historical
selector or snapshot token is accepted. `synced` describes the local node's synchronization.

The generic cell schema recursively permits null, boolean, string, array and object, never JSON
numbers. Column metadata supplies the dynamic cell type and encoding. Numeric strings are exact
decimal values; AVG is rounded half-even to at most 18 fractional places. Assets are objects with
decimal-string `amount`, text `symbol`, unsigned-decimal-string `precision`; extended assets add
`contract`. IEEE values are raw little-endian hex bytes prefixed by `0x`. Nested numeric leaves use
the same string rule. A variant projects as the ABI type name and its normalized value.

Integration tests additionally verify unique column names, identical row/column key sets,
nullability, encoding, asset members, owner/ABI correspondence, returned-row counts, and the
agreement of `page` with the rows and `stats` it describes. These
relationships depend on runtime columns and cannot be expressed by this static Draft-04 schema.

Numeric RPC IDs and error positions/codes are explicit exceptions to string-only numeric cells.
Numeric IDs must be integral and within [-4294967295, 4294967295]; larger IDs must be strings.
All metadata counters/heights are unsigned decimal strings. Unknown envelope/result members are
rejected. HTTP transport errors raised before plugin dispatch are outside the response schema.

Examples use illustrative chain IDs, ABI hashes and timestamps. They are validated as documents;
the HTTP tests validate actual serialized responses from a running plugin and signed-block fixture.
