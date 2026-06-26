# Producer API Plugin - Operator Guide

The `producer_api_plugin` exposes HTTP endpoints for inspecting and administering a producing node. It is intended
for node operators and automation that already run inside the operator's trusted management environment.

## API Categories

The plugin registers endpoints in these HTTP API categories:

- `producer_ro` - read-only producer status, runtime options, greylist/whitelist state, protocol feature state, and
  unapplied transaction inspection.
- `producer_rw` - administrative actions such as pause/resume production, changing runtime options, updating greylist
  and whitelist settings, scheduling protocol feature activations, and requesting integrity hashes.
- `snapshot` - administrative snapshot creation and snapshot scheduling through the producer API.

## Network Exposure

The `producer_rw` and `snapshot` categories are administrative APIs. Exposing them to an untrusted network can allow a
remote caller to disrupt production, change node runtime behavior, or create/schedule expensive snapshot work. The
plugin logs a startup warning when these categories are not bound only to loopback, but it does not reject that
configuration because block producers may intentionally operate inside closed or private networks.

Do not expose these categories to the public internet. If a deployment needs remote administration, make the exposure a
deliberate private-management-network decision and protect it with external controls such as firewall rules, security
groups, VPN access, or an authenticated reverse proxy.

## Recommended Category Binding

Prefer `--http-category-address` so public or peer-facing APIs are separated from producer administration:

```bash
nodeop \
  --http-server-address http-category-address \
  --http-category-address chain_ro,0.0.0.0:8888 \
  --http-category-address chain_rw,127.0.0.1:8889 \
  --http-category-address producer_ro,127.0.0.1:8889 \
  --http-category-address producer_rw,127.0.0.1:8889 \
  --http-category-address snapshot,127.0.0.1:8889 \
  --plugin sysio::chain_api_plugin \
  --plugin sysio::producer_api_plugin \
  ...
```

If the node is inside a closed management network, the admin categories may instead be bound to that private interface,
for example `producer_rw,10.0.0.10:8889`, as long as network policy restricts access to trusted operators and
automation.

Avoid using a public all-category listener such as `--http-server-address 0.0.0.0:8888` together with
`producer_api_plugin` unless the listener is reachable only from a trusted management network. That form exposes every
enabled API category, including `producer_rw` and `snapshot`.

## Public Snapshot Serving

Use `snapshot_api_plugin` for public snapshot discovery and download. Its `snapshot_ro` category is read-only and can be
bound publicly while keeping the producer API's admin `snapshot` category private.
