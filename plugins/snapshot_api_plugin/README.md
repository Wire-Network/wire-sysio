# Snapshot API Plugin — Operator Guide

The `snapshot_api_plugin` provides public, read-only HTTP endpoints for snapshot discovery and download. It allows node operators to serve snapshots to other nodes on the network, enabling fast bootstrap without manual file transfers.

## Enabling the Plugin

Add the plugin to your `nodeop` startup:

```
--plugin sysio::snapshot_api_plugin
```

The plugin depends on `chain_plugin`, `producer_plugin`, and `http_plugin` (loaded automatically).

## Network Configuration

### Default (all APIs on one address)

With the default `--http-server-address` setting, the snapshot endpoints are served alongside all other APIs:

```
nodeop \
  --http-server-address 0.0.0.0:8888 \
  --plugin sysio::snapshot_api_plugin \
  ...
```

This exposes **all** enabled API categories on port 8888. For production nodes this is usually not desirable — you likely want to keep admin APIs (producer, chain write) on loopback while exposing only snapshot endpoints publicly.

This is especially important when `producer_api_plugin` is enabled. Its `producer_rw` and admin `snapshot` categories
can pause production, alter runtime settings, or create/schedule snapshot work. `nodeop` warns when those categories
are reachable beyond loopback, but it does not block the configuration because some producers intentionally run their
admin APIs on closed private networks. Treat any non-loopback bind for those categories as a deliberate
private-management-network decision, not as a public HTTP configuration.

### Recommended: Isolate with `--http-category-address`

Use `--http-category-address` to bind only the `snapshot_ro` category to a public address, keeping everything else on loopback:

```
nodeop \
  --http-server-address http-category-address \
  --http-category-address chain_ro,127.0.0.1:8888 \
  --http-category-address chain_rw,127.0.0.1:8888 \
  --http-category-address producer_ro,127.0.0.1:8888 \
  --http-category-address producer_rw,127.0.0.1:8888 \
  --http-category-address net_ro,127.0.0.1:8888 \
  --http-category-address net_rw,127.0.0.1:8888 \
  --http-category-address snapshot,127.0.0.1:8888 \
  --http-category-address snapshot_ro,0.0.0.0:9090 \
  --plugin sysio::snapshot_api_plugin \
  --plugin sysio::chain_api_plugin \
  --plugin sysio::producer_api_plugin \
  --plugin sysio::net_api_plugin \
  ...
```

In this setup:
- Port **8888** (loopback only) serves chain, producer, net, and admin snapshot APIs
- Port **9090** (public) serves the read-only snapshot endpoints plus the node-global endpoints —
  `/v1/chain/get_info` and `/v1/node/get_supported_apis` are reachable on every listener by design

When using `--http-category-address`, the `--http-server-address` must be set to the literal string `http-category-address` and `--unix-socket-path` must not be set.

### Minimal public-only configuration

If the node's sole purpose is serving snapshots:

```
nodeop \
  --http-server-address http-category-address \
  --http-category-address snapshot_ro,0.0.0.0:9090 \
  --plugin sysio::snapshot_api_plugin \
  ...
```

Only the three snapshot read-only endpoints will be reachable, along with the node-global
`/v1/node/get_supported_apis` (always served) and `/v1/chain/get_info` when `sysio::chain_api_plugin`
is loaded — node-global endpoints are reachable on every listener by design.

## Full Provider Setup

A snapshot provider generates snapshots, attests to them on-chain, and serves them to the network. This requires a registered producer with a delegated snapshot provider account.

### 1. Register as a producer

The node operator must be a registered producer with a rank of 30 or below:

```bash
# Register as a block producer
clio push action sysio regproducer \
  '{"producer": "myproducer1", "producer_key": "SYS6...", "url": "", "location": 0}' \
  -p myproducer1@active

# Set producer rank (must be <= 30 to be eligible as a snapshot provider)
clio push action sysio setrank \
  '{"producer": "myproducer1", "rank": 1}' \
  -p sysio@active
```

### 2. Register a snapshot provider account

Each producer delegates a separate account to act as its snapshot provider. This account signs the attestation votes:

```bash
# Create the provider account (if it doesn't exist)
clio create account sysio mysnapprov1 SYS6...

# Register the provider account under the producer
clio push action sysio regsnapprov \
  '{"producer": "myproducer1", "snap_account": "mysnapprov1"}' \
  -p myproducer1@active
```

To rotate the signing account later, call the same action with the replacement account:

```bash
clio push action sysio regsnapprov \
  '{"producer": "myproducer1", "snap_account": "mysnapprov2"}' \
  -p myproducer1@active
```

### 3. Configure attestation quorum (network-wide)

The attestation config is a network-wide singleton set by the `sysio` authority. It controls how many provider votes are needed before a snapshot is considered attested:

```bash
clio push action sysio setsnpcfg \
  '{"min_providers": 3}' \
  -p sysio@active
```

- `min_providers` — fixed number K of distinct producer votes required; voting remains disabled until this nonzero value is configured

### 4. Enable automatic scheduled snapshots

For production, enable snapshot provider mode in `config.ini`:

```ini
snapshot-provider-account = mysnapprov1
```

Provider mode creates the canonical schedule automatically: snapshots are taken at exact 25,000-block multiples
(25,000, 50,000, 75,000, ...), and `votesnaphash` is submitted automatically after each scheduled snapshot finalizes.
Manual snapshots are not attested because their heights need not satisfy the shared cadence.

### 5. Attestation lifecycle

The provider plugin submits the snapshot's block ID and root hash automatically. The equivalent action is shown here
only as a protocol reference; its block ID must name an exact 25,000-block cadence height:

```bash
clio push action sysio votesnaphash \
  '{"snap_account": "mysnapprov1", "block_id": "0000c350...", "snapshot_hash": "abcdef12..."}' \
  -p mysnapprov1@active
```

The `votesnaphash` action accumulates producer identities in the `snapvotes` table. Every tuple uses the current fixed K;
registration count does not change it. Configuration changes apply to pending heights, while provider-account rotation
does not retract accepted votes. Once K is met, the system contract moves the entry to `snaprecords` and purges pending
votes through that height.

Before replay, auto-fetching nodes require compatible `snaprecords` and `snapconfig` schemas plus a nonzero K. A
configuration-read or disabled-configuration rejection tells the operator to delete the loaded chain state before
restarting without `--snapshot-endpoint`. After syncing, nodes verify the `snaprecords` row and shut down if the
snapshot is missing or disagrees. Manual `--snapshot` remains an operator-trusted path without attestation verification.

The manual `create_snapshot` and `schedule_snapshot` APIs remain available through `producer_api_plugin`, but they are
not part of the provider attestation workflow. When a snapshot finalizes (becomes irreversible), it is automatically
added to the serving catalog and remains available through the explicit-block metadata and download endpoints.

### Startup catalog

On startup, the plugin scans the snapshots directory for existing `snapshot-*.bin` files and adds them to the catalog. No manual re-creation is needed after a restart.

## API Endpoints

All endpoints use POST with JSON bodies, consistent with other Wire Sysio APIs.

### `POST /v1/snapshot/latest`

Returns metadata for the newest snapshot at an exact attestation-cadence height whose matching on-chain attestation is
irreversible and whose file is still available on disk. Newer manual snapshots, unavailable files, and scheduled
snapshots that are not yet attested are skipped, so base-URL bootstrap discovers the newest snapshot it can immediately
download. Discovery reads final attestation records newest-first in bounded pages limited to the locally available
snapshot-height range, and transient table-read failures are retried before discovery fails closed.

**Request:** empty body or `{}`

**Response (200):**
```json
{
  "block_num": 50000,
  "block_id": "0000c350...",
  "block_time": "2025-01-15T12:00:00.000",
  "root_hash": "abcdef12..."
}
```

**Response (404):** No attested scheduled snapshot is currently downloadable. Manual snapshots may still be available
by explicit block.

### `POST /v1/snapshot/by_block`

Returns metadata for a snapshot at a specific block number. Download eligibility is enforced separately by the raw
download endpoint.

**Request:**
```json
{"block_num": 50000}
```

**Response (200):** Same format as `/v1/snapshot/latest`.

**Response (404):** No snapshot found for the requested block.

### `POST /v1/snapshot/download`

Downloads a snapshot file as a binary stream. Scheduled snapshots return 404 until an irreversible on-chain
`snaprecords` row matches the exact block ID and root hash. Manual unscheduled snapshots remain explicitly downloadable.

**Request:**
```json
{"block_num": 50000}
```

**Response (200):**
- `Content-Type: application/octet-stream`
- `Content-Disposition: attachment; filename="snapshot-50000.bin"`
- `Accept-Ranges: bytes`
- `Content-Length: <file size>`

**Range header support:** Include a `Range: bytes=START-END` header for partial downloads (resumable transfers). The server responds with `206 Partial Content` and a `Content-Range` header.

## Bootstrapping a Node from a Snapshot Endpoint

Nodes can bootstrap directly from a snapshot-serving node using the `--snapshot-endpoint` option on `chain_plugin`:

```bash
# Bootstrap from the latest available snapshot
nodeop \
  --delete-all-blocks \
  --snapshot-endpoint http://snapshot-provider:9090 \
  --p2p-peer-address snapshot-provider:9876 \
  ...

# Bootstrap from a specific block number
nodeop \
  --delete-all-blocks \
  --snapshot-endpoint http://snapshot-provider:9090/50000 \
  --p2p-peer-address snapshot-provider:9876 \
  ...
```

The bootstrap process:
1. Fetches snapshot metadata from the endpoint
2. Rejects snapshots outside the exact 25,000-block attestation cadence
3. Downloads the snapshot binary
4. Verifies the file's root hash matches the advertised hash
5. Loads the snapshot and begins syncing from that point
6. After syncing, verifies the snapshot's on-chain attestation record

`--delete-all-blocks` is required when existing chain data is present. The `--snapshot-endpoint` option is incompatible with `--snapshot` (local file).
Manual/on-demand snapshots remain available through the explicit-block serving APIs. Base-URL discovery ignores their
unscheduled heights, and an explicit-block bootstrap request rejects them before download because they can never
receive an on-chain attestation.

### Bootstrap download status and limits

Snapshot bootstrap is attended. Metadata has a finite total deadline. The file transfer intentionally has no
DNS/connect, request-upload, response-header, response-body, idle, total, or inherited task deadline, preserving
support for large or temporarily stalled downloads. It remains resource-bounded by the response-size and disk-space
checks below. The transfer reports phase changes and, every five seconds, downloaded bytes, percentage, transfer
rate, and ETA when the response supplies `Content-Length`. Pressing Ctrl+C cancels pending resolver or socket work
and removes the partial file.

HTTPS endpoints use system CA roots and mandatory DNS/IP identity verification. Private roots can be added with
`--snapshot-endpoint-additional-ca-file` or `--snapshot-endpoint-additional-ca-path`; use
`--snapshot-endpoint-proxy http://host:port` for an explicit proxy. The corresponding `--outbound-http-*` options
provide process-wide fallbacks; snapshot-specific values take precedence.

The existing chain database size setting supplies the download ceiling:

| Option | Default | Purpose |
|---|---:|---|
| `--chain-state-db-size-mb` | `1024` | Maximum accepted snapshot response size and chain-state database size |

The response-size limit must be positive. A fixed-length response that exceeds the maximum is rejected from its
`Content-Length` before a temporary file is opened. Chunked or lengthless responses are stopped at the same byte
ceiling. Available disk space is checked before and throughout the transfer, and a failed transfer removes its
`.downloading` file.
Long transfers recheck after at most 64 MiB or five seconds of progress and retain an additional 64 MiB safety margin
to bound interference from concurrent disk consumers between probes.

## Reverse Proxy Considerations

For production deployments, consider placing a reverse proxy (nginx, caddy) in front of the snapshot endpoint:

- **Rate limiting** — prevent a single client from saturating bandwidth
- **TLS termination** — serve snapshots over HTTPS
- **Caching / CDN** — offload download bandwidth from the node
- **Access control** — restrict by IP or authentication

Configure the proxy with response-size, idle, and total-transfer limits that are no weaker than the origin node's
bootstrap limits. Proxy authentication and TLS reduce interception and unauthorized access risk, but they do not
replace the origin-side resource bounds.

The snapshot files are served uncompressed via zero-copy `sendfile()`. If bandwidth is a concern, configure compression at the proxy layer (e.g., `gzip_static` or `zstd_static` in nginx with pre-compressed files).
