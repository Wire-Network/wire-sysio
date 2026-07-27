# Solana Cluster Identity Operations

`outpost_solana_client_plugin` can pin each signing-capable Solana RPC client
to an expected genesis hash. This prevents a reachable but incorrect RPC
cluster from receiving reads, signatures, or submitted transactions.

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
outpost-solana-cluster-identity-max-age-ms = 30000
outpost-solana-cluster-identity-probe-timeout-ms = 5000
```

Obtain the expected hash from an independently trusted deployment manifest or
control-plane source. Do not generate configuration by asking the same RPC URL
that nodeop will pin: that would make a redirected endpoint self-attesting.

Roll out all identity entries for a node in one configuration change. A pinned
node performs a bounded `getGenesisHash` probe for every client during startup
and publishes no clients unless all probes match.

## Runtime behavior

- Every protected JSON-RPC request first calls `getGenesisHash` on the same
  HTTP/TLS connection. The protected request is not written unless that peer
  returns the configured identity and permits connection reuse. This also
  covers successful fallback between multiple DNS addresses.
- Signing always performs a fresh bounded probe immediately before invoking
  the local signer. Transaction submission performs its verification and
  `sendTransaction` call on one exact connection.
- The configured maximum age bounds reported verification freshness. Every
  protected JSON-RPC operation is stricter and performs a peer-bound preflight.
- A follow-up transport failure cannot reuse the completed preflight. The next
  operation independently re-resolves when needed and reverifies.
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
- `nodeop_solana_cluster_identity_verification_recoveries_total`
- `nodeop_solana_cluster_identity_blocked_operations_total{operation}`

Recommended initial alerts:

- any production client with `mode="unpinned"`;
- any client with `status="mismatch"`;
- sustained `status="error"` or increasing verification failures;
- increasing blocked signing or submission operations;
- verification age above the configured maximum plus normal scrape delay.

Metric labels are bounded to configured client IDs and closed enum values.
Expected/observed hashes and endpoint URLs are deliberately excluded.

## Follow-up enforcement

This compatibility window does not complete mandatory enforcement. Before
removing legacy mode, deployment tooling (including `wire-tools-ts`) must
provision an independently sourced identity for every generated Solana client
configuration. The enforcement release should then reject startup when the
option is absent.
