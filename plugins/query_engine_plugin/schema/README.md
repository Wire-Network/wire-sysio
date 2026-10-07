# query.execute schemas

Both schemas use JSON Schema Draft-04 and are validated with the repository's RapidJSON validator.
The response contract is version `1.0`. Breaking shape or encoding changes require a new version.
The request schema describes the single-request profile; method validation and SQL semantics occur
in the service. Notifications have no response document (HTTP 204, empty body).

Success and error envelopes are mutually exclusive. A successful result is always complete for the
submitted SQL, including its explicit LIMIT. Budget/decode failures contain only an error.

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
nullability, encoding, asset members, owner/ABI correspondence, and returned-row counts. These
relationships depend on runtime columns and cannot be expressed by this static Draft-04 schema.

Numeric RPC IDs and error positions/codes are explicit exceptions to string-only numeric cells.
Numeric IDs must be integral and within [-4294967295, 4294967295]; larger IDs must be strings.
All metadata counters/heights are unsigned decimal strings. Unknown envelope/result members are
rejected. HTTP transport errors raised before plugin dispatch are outside the response schema.

Examples use illustrative chain IDs, ABI hashes and timestamps. They are validated as documents;
the HTTP tests validate actual serialized responses from a running plugin and signed-block fixture.
