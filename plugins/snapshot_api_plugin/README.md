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
- Port **9090** (public) serves **only** the read-only snapshot endpoints

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

Only the three snapshot read-only endpoints will be reachable.

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

To unregister later:

```bash
clio push action sysio delsnapprov '{"account": "mysnapprov1"}' -p mysnapprov1@active
```

### 3. Configure attestation quorum (network-wide)

The attestation config is a network-wide singleton set by the `sysio` authority. It controls how many provider votes are needed before a snapshot is considered attested:

```bash
clio push action sysio setsnpcfg \
  '{"min_providers": 3, "threshold_pct": 67}' \
  -p sysio@active
```

- `min_providers` — minimum number of registered providers required for any quorum to be possible
- `threshold_pct` — percentage of registered providers that must vote for the same hash to reach quorum (e.g., 67 means two-thirds)

### 4. Generate and attest snapshots

After creating a snapshot, the provider submits a vote with the snapshot's block ID and root hash. When enough providers vote for the same hash, an attested record is created on-chain in the `snaprecords` table.

```bash
# Create a snapshot (returns block_num, block_id, root_hash)
curl -s -X POST http://127.0.0.1:8888/v1/producer/create_snapshot

# Submit attestation vote using the snapshot metadata
clio push action sysio votesnaphash \
  '{"snap_account": "mysnapprov1", "block_id": "0000c350...", "snapshot_hash": "abcdef12..."}' \
  -p mysnapprov1@active
```

The `votesnaphash` action accumulates votes in the `snapvotes` table. Once the threshold is met, the system contract moves the entry to `snaprecords` and purges the pending votes.

Bootstrapping nodes verify the `snaprecords` table after syncing — if no attested record exists for the snapshot's block number, auto-fetched bootstraps will shut down with a fatal error.

### 5. Automate with scheduled snapshots

For production, schedule recurring snapshots rather than creating them manually:

```bash
curl -X POST http://127.0.0.1:8888/v1/producer/schedule_snapshot \
  -d '{"block_spacing": 25000, "start_block_num": 1, "end_block_num": 4294967295}'
```

This creates a snapshot every 25,000 blocks (~3.5 hours at 0.5s block time). The attestation vote (`votesnaphash`) still needs to be submitted after each snapshot finalizes — this is typically handled by an external script or monitoring daemon that watches for new snapshots and submits the vote automatically.

Both `create_snapshot` and `schedule_snapshot` require `producer_api_plugin` to be enabled. When a snapshot finalizes (becomes irreversible), it is automatically added to the serving catalog.

### Startup catalog

On startup, the plugin scans the snapshots directory for existing `snapshot-*.bin` files and adds them to the catalog. No manual re-creation is needed after a restart.

## API Endpoints

All endpoints use POST with JSON bodies, consistent with other Wire Sysio APIs.

### `POST /v1/snapshot/latest`

Returns metadata for the most recent snapshot.

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

**Response (404):** No snapshots in catalog.

### `POST /v1/snapshot/by_block`

Returns metadata for a snapshot at a specific block number.

**Request:**
```json
{"block_num": 50000}
```

**Response (200):** Same format as `/v1/snapshot/latest`.

**Response (404):** No snapshot found for the requested block.

### `POST /v1/snapshot/download`

Downloads a snapshot file as a binary stream.

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
2. Downloads the snapshot binary
3. Verifies the file's root hash matches the advertised hash
4. Loads the snapshot and begins syncing from that point
5. After syncing, verifies the snapshot's on-chain attestation record

`--delete-all-blocks` is required when existing chain data is present. The `--snapshot-endpoint` option is incompatible with `--snapshot` (local file).

## Reverse Proxy Considerations

For production deployments, consider placing a reverse proxy (nginx, caddy) in front of the snapshot endpoint:

- **Rate limiting** — prevent a single client from saturating bandwidth
- **TLS termination** — serve snapshots over HTTPS
- **Caching / CDN** — offload download bandwidth from the node
- **Access control** — restrict by IP or authentication

The snapshot files are served uncompressed via zero-copy `sendfile()`. If bandwidth is a concern, configure compression at the proxy layer (e.g., `gzip_static` or `zstd_static` in nginx with pre-compressed files).
