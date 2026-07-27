# Outbound HTTP transport

The SEC-134 nodeop/libfc HTTP paths—Ethereum and Solana JSON-RPC/REST, KIOD, external debugging, and snapshot
bootstrap—use one Boost.Beast/Boost.Asio transport core. The existing synchronous `fc::http::transport`,
`fc::network::json_rpc::json_rpc_client`, and `fc::http_client` APIs are blocking facades over that core; they do
not implement separate DNS, socket, TLS, HTTP parsing, retry, or download stacks.

The interactive `clio` executable remains a separate libcurl consumer. It is outside the nodeop/libfc transport
boundary.

## HTTPS trust and routing

HTTPS always loads the platform trust store, verifies the peer certificate chain, and verifies the original URL
DNS name or IP address. Private roots augment system trust; there is no option that disables verification.
Proxy environment variables are ignored. A proxy is used only when its caller-specific option is configured,
using the supported `http://host:port` form without embedded credentials.

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

JSON-RPC clients warm their configured endpoint at construction and retain the resolved address until a connection
failure invalidates it. The process-wide resolver admits one platform lookup at a time with no unbounded work queue.
Ordinary callers bound their wait with the connection deadline; the unbounded snapshot exception remains cancellable.

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

## Limits, cancellation, and snapshot downloads

The shared transport bounds request and response headers and bodies and applies connect, header, read, idle, and
total deadlines. Wire's predicate-based cancellation remains connected through the synchronous facades to resolver
and socket operations.

Snapshot bootstrap is the reviewed long-running exception. Metadata requests retain a finite total deadline.
The streamed snapshot file transfer has no DNS/connect, request-upload, response-header, response-body, idle, total,
or inherited task deadline, preserving its pre-refactor behavior for large or temporarily stalled downloads. It
remains cancellable, checks available disk space before and during transfer, writes to a temporary sibling, atomically
publishes only a complete response, and removes partial files on failure.

## Metrics

Prometheus exports process-wide request, success, request-byte, response-byte, and fixed-category failure counters.
Request bytes count every complete body upload, including a completed retry attempt. Request outcome and failure
counters describe the final logical request outcome rather than intermediate stale-connection attempts.
