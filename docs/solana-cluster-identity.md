# Solana Cluster Identity Operations

`outpost_solana_client_plugin` can pin each signing-capable Solana RPC client
to an expected genesis hash. This detects benign routing and configuration
errors before a connection receives ordinary reads or submitted transactions.
It is not endpoint authentication: the genesis hash is public, so a malicious
RPC service can claim the expected value, and a request-routing proxy can select
different backends behind one persistent connection. TLS endpoint
authentication and trusted network routing remain required.

## Initial backward-compatible rollout

The initial release intentionally supports two modes:

1. **Legacy unpinned:** configure no
   `--outpost-solana-cluster-identity` entries. Existing nodeop and
   `wire-tools-ts` configurations continue to start unchanged. Nodeop emits a
   security warning for every client and reports the unpinned state in
   Prometheus.
2. **Strict pinned:** configure at least one identity entry. Every configured
   Solana client must then have exactly one entry. Startup rejects partial
   coverage, duplicates, unknown client IDs, empty values, and hashes that are
   not canonical base58 encodings of exactly 32 bytes.

There is no mixed mode within one process. This makes a missing entry visible
at startup instead of silently leaving only some signing paths unprotected.

## Configuration

For each existing client:

```ini
outpost-solana-client = <client-id>,<signature-provider-id>,<rpc-url>
outpost-solana-cluster-identity = <client-id>,<expected-genesis-hash>
```

Optional policy:

```ini
outpost-solana-cluster-identity-probe-timeout-ms = 5000
```

Obtain the expected hash from an independently trusted deployment manifest or
control-plane source. Do not generate configuration by asking the same RPC URL
that nodeop will pin: that would make a redirected endpoint self-attesting.

Roll out all identity entries for a node in one configuration change. A pinned
node validates every client's first persistent RPC connection and performs a
separate ordinary `getGenesisHash` cross-check during startup. The plugin
publishes no clients unless all startup checks match.

## Runtime behavior

- Every new HTTP/TLS connection calls `getGenesisHash` before its first
  ordinary JSON-RPC request. The connection is admitted only when that peer
  returns the configured identity and preserves the connection for reuse.
  Healthy persistent connections are validated once; replacements and
  successful fallback to another resolved address are validated independently.
- Protected JSON-RPC operations can use only an admitted live connection.
  There is no process-wide cached authorization or per-operation identity
  re-probe, so one failed connection does not authorize a later replacement.
  Later callers may reuse the same admitted persistent connection, but its
  validation never transfers to a different connection.
- Signing remains local. In pinned mode, a transaction can be signed only when
  its recent blockhash came from `getLatestBlockhash` over an admitted
  connection. This binds the transaction to validated provisioning data
  without adding network I/O to the signing method. Unpinned signing preserves
  the legacy local-only behavior.
- The identity validation request has its own bounded timeout. An endpoint
  that answers the validation and then closes the connection is incompatible
  with pinned mode because no ordinary request can be safely bound to that
  validation.
- Solana clients honor cancellation and an earlier active task deadline while
  queued as well as during transport work. The lower-level transport
  inheritance switch cannot disable these fail-closed client budgets.
- Verification age is telemetry showing time since the latest successful
  connection admission. It is not an authorization TTL; authorization lives
  only on the admitted connection and disappears when that connection closes.
- A transport failure cannot transfer admission to a replacement connection.
  The next connection independently re-resolves when needed and revalidates.
- Timeout, RPC, and malformed-response failures block the current operation
  but can recover after a later matching probe.
- An identity mismatch is sticky for the process lifetime. All later protected
  operations fail without probing or signing. Investigate the endpoint and
  restart only after correcting configuration or routing.

Logs identify the configured client, sanitized endpoint, status, reason, and
operation. Query strings, URL user information, signer identifiers, account
payloads, and transaction data are omitted.

## Prometheus and alerts

The Prometheus plugin exports:

- `nodeop_solana_cluster_identity{client_id,mode,status,reason}`
- `nodeop_solana_cluster_identity_verification_age_seconds`
- `nodeop_solana_cluster_identity_verification_attempts_total`
- `nodeop_solana_cluster_identity_verification_successes_total`
- `nodeop_solana_cluster_identity_verification_mismatches_total`
- `nodeop_solana_cluster_identity_verification_failures_total`
- `nodeop_solana_cluster_identity_verification_cancellations_total`
- `nodeop_solana_cluster_identity_verification_recoveries_total`
- `nodeop_solana_cluster_identity_blocked_operations_total{operation}`
- `nodeop_solana_cluster_identity_protected_operation_failures_total{operation}`

Recommended initial alerts:

- any production client with `mode="unpinned"`;
- any client with `status="mismatch"`;
- sustained `status="error"` or increasing verification failures;
- increasing blocked signing or submission operations;
- increasing protected-operation failures.

Cancellation increases indicate local shutdown or load-shedding activity, not
an endpoint identity failure. Verification age is the age of the last
connection admission. It may grow indefinitely while a healthy persistent
connection remains in use, so it is diagnostic context rather than a
standalone freshness alert.

Metric labels are bounded to configured client IDs and closed enum values.
Expected/observed hashes and endpoint URLs are deliberately excluded.

## Follow-up enforcement

This compatibility window does not complete mandatory enforcement. Before
removing legacy mode, deployment tooling (including `wire-tools-ts`) must
provision an independently sourced identity for every generated Solana client
configuration. The enforcement release should then reject startup when the
option is absent.
