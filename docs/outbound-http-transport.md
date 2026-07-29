# Outbound HTTP transport

The SEC-134 nodeop/libfc HTTP paths -- Ethereum and Solana JSON-RPC/REST, KIOD, external debugging, and snapshot
bootstrap -- use one Boost.Beast/Boost.Asio transport core. The existing synchronous `fc::http::transport`,
`fc::network::json_rpc::json_rpc_client`, and `fc::http_client` APIs are blocking facades over that core; they do
not implement separate DNS, socket, TLS, HTTP parsing, retry, or download stacks.

The interactive `clio` executable remains a separate libcurl consumer. It is outside the nodeop/libfc transport
boundary.

## HTTPS trust and routing

HTTPS always loads the OpenSSL platform trust store, verifies the peer certificate chain, and verifies the original
URL DNS name or IP address. OpenSSL's normal `SSL_CERT_FILE` and `SSL_CERT_DIR` trust-store overrides remain active.
Private roots augment that trust; there is no option that disables verification. Proxy environment variables are
intentionally ignored so routing changes require an explicit node option. A proxy uses the supported
`http://host:port` form without embedded credentials.

The process-wide fallbacks are `--outbound-http-additional-ca-file`,
`--outbound-http-additional-ca-path`, and `--outbound-http-proxy`. A caller-specific value in the table below
overrides its process-wide fallback.

| Caller | Additional CA file | Additional CA directory | Explicit proxy |
|---|---|---|---|
| Ethereum | `--outpost-ethereum-additional-ca-file` | `--outpost-ethereum-additional-ca-path` | `--outpost-ethereum-proxy` |
| Solana | `--outpost-solana-additional-ca-file` | `--outpost-solana-additional-ca-path` | `--outpost-solana-proxy` |
| External debugging | `--ext-debugging-additional-ca-file` | `--ext-debugging-additional-ca-path` | `--ext-debugging-proxy` |
| KIOD/signing | `--http-client-additional-ca-file` | `--http-client-additional-ca-path` | `--http-client-proxy` |
| Snapshot bootstrap | `--snapshot-endpoint-additional-ca-file` | `--snapshot-endpoint-additional-ca-path` | `--snapshot-endpoint-proxy` |

Missing, empty, malformed, or unreadable custom trust configuration fails during plugin initialization.
Plain `http://` remains available for explicitly configured trusted networks but is not authenticated.

## Replay policy and connection reuse

Ethereum and Solana reads use a narrowly bounded stale-connection policy: a read may be retried once, immediately,
and only when its first attempt used a cached HTTP/1.1 connection that proved stale. Healthy connections are reused.
The shared request policy expresses this through a retry-filter hook. The hook can only reject a failure that the
transport already classified as retryable; it cannot broaden replay eligibility or bypass the explicit idempotence
requirement. The legacy snapshot retry flag remains source-compatible and maps internally to the same hook.

Side-effecting calls such as `eth_sendTransaction`, `eth_sendRawTransaction`, Solana `sendTransaction`, and
`requestAirdrop` are always single-attempt. The generic JSON-RPC `call`, notification, batch, and raw HTTP APIs also
enforce single-attempt behavior even if a caller supplies permissive base retry options. A caller must select the
explicit idempotent API to receive the stale-connection retry.

JSON-RPC construction performs no DNS or network I/O. Resolution happens on the first request and the result is
retained until a connection failure invalidates it. The process-wide resolver runs at most four platform lookups at
once on four independent resolver services; up to 256 additional callers wait through an event-driven admission gate
under their connection/task deadlines rather than queueing platform work or polling. Further callers fail admission
immediately. The unbounded snapshot exception remains cancellable.

The idle connection pool is capped at 32 connections per client. Connections idle for more than 15 seconds are
closed before reuse, which keeps the reuse window below common provider keep-alive limits and bounds stale or
peer-closed sockets retained by the process. A non-blocking socket peek also rejects a cached connection when a
peer FIN/reset or unexpected unread bytes are already observable.

## Per-connection validation

The asynchronous client and blocking adapter accept an optional generic validator for fixed-endpoint clients. Before
the first ordinary request on each newly established connection, the transport sends one bounded, single-attempt
validation request to that same endpoint. The response predicate either admits the connection or throws to reject
and close it. An admitted connection must remain persistent; if the peer closes the validation response, the
transport rejects it because no subsequent operation can be bound to that observation.

Validation state belongs to the live HTTP/TLS connection and is discarded when it closes. A replacement or fallback
connection therefore validates independently before carrying ordinary work. The attempt, success, and failure
lifecycle callbacks are non-blocking telemetry hooks; they cannot change the single-attempt policy and must not
throw from completion callbacks. The initiating ordinary request's already-started total deadline caps the
validation deadline, so validation cannot extend that request's overall budget.

For a connection-affine continuation, validation precedes the initial call. The hook and follow-up then use the same
already-admitted connection; the continuation mechanism does not trigger another validation between its two legs.
This keeps endpoint admission generic and independent from higher-level JSON-RPC or chain-specific policy.

## Connection-affine continuations

The asynchronous client and blocking compatibility adapter expose a generic one-step continuation hook. The
transport buffers the first bounded response, keeps that exact HTTP/TLS connection out of the idle pool while the
hook inspects it, and sends the hook-selected follow-up on the retained connection. The connection is opaque: callers
cannot manufacture or persist a session identifier.

The first request may apply its ordinary bounded retry policy before the hook runs. Once the hook is entered, a
validation exception, peer close, endpoint change, cancellation, or follow-up transport failure closes the retained
connection. The follow-up is single-attempt and never reconnects or falls back to another pooled connection. Each
request has its own limits and total-deadline budget. This supports generic challenge/response, preflight/action, and
endpoint-identity/action flows without embedding a caller-specific verification field in `request_options`.
Continuation hooks execute synchronously on the client executor. They must be non-blocking and must not re-enter the
same client; synchronous re-entry fails immediately instead of deadlocking.

The JSON-RPC facade exposes the same mechanism through `call_then`. Its hook receives the validated first call's
`result` and returns a method, parameters, and follow-up deadline policy. Replay policy is explicit:
`stale_reused_connection_once` permits only the initial call to recover once when an idle cached connection proves
stale. The follow-up options type has no replay setting because a connection-affine follow-up is unconditionally
single-attempt. An optional per-call total-timeout cap can shorten the client's configured total timeout but cannot
lengthen it or replace stricter connect, header, read, or idle limits. The first and follow-up calls each start an
independent total-timeout budget and remain bounded by an active task deadline.

## Limits, cancellation, and snapshot downloads

The shared transport bounds request and response headers and bodies. The total deadline covers queueing, DNS,
connection setup, request upload, and the complete response. Connect and header deadlines bound their named phases;
the read deadline bounds aggregate response-body time; the idle deadline is reset for each response-body read after
headers arrive. It is not described or applied as a request-upload inactivity timer. Wire's predicate-based
cancellation remains connected through the synchronous facades to resolver and socket operations.

Snapshot bootstrap is the reviewed long-running exception. Metadata requests retain a finite total deadline.
The streamed snapshot file transfer has no DNS/connect, request-upload, response-header, response-body, idle, total,
or inherited task deadline, preserving its pre-refactor behavior for large or temporarily stalled downloads. It
remains cancellable, checks available disk space before and during transfer, writes to a temporary sibling, atomically
publishes only a complete response, and removes partial files on failure.

## Metrics

Prometheus exports process-wide request, success, request-byte, response-byte, and fixed-category failure counters.
Request bytes count every complete body upload, including a completed retry attempt. Request outcome and failure
counters describe the final logical request outcome rather than intermediate stale-connection attempts.
