# underwriter_plugin

Autonomous underwriter daemon. Polls `sysio.uwrit::uwreqs` for PENDING
swaps, picks the ones its collateral can cover, and submits a signed
`UnderwriteIntentCommit` to **both** outposts (source + destination)
for the depot's race resolver.

The underwriter is a **separate daemon** from the batch operator. It does
not relay OPP envelopes — that is the batch operator's job. The
underwriter only signs and submits underwriting commits.

## Lifecycle

### Startup pre-flight (unconditional, no dev escape hatch)

`plugin_startup` runs a series of checks before scheduling the cron job;
any failure logs a structured `elog` and skips cron registration:

1. `sysio.opreg::operators[underwriter_account].status == OPERATOR_STATUS_ACTIVE`.
2. Every **active** non-depot chain in `sysio.chains::chains` has a
   configured `--underwriter-{eth,sol}-outpost` endpoint of the matching
   VM family. The served set is derived from the registry while the
   outpost clients are built from config, so a missing or wrong-family
   endpoint would let the scan loop pick a request it cannot fully
   commit — landing one leg and stalling the other. Inactive
   (not-yet-`activchain`ed) chains are skipped, so registering a future
   chain never blocks startup before its endpoint/collateral land.
3. `sysio.authex::links` covers every active chain in `sysio.chains::chains` —
   the underwriter cannot sign a commit on a chain it has no authex link for.
4. Non-zero balance on at least one TokenKind for every active
   outpost chain.
5. The required source-deposit function / instruction names resolve
   against the loaded ABI / IDL files, and a signature self-test passes.

No `--strict=false` flag, no dev fallback. Cluster bootstrap is
responsible for establishing the required state — see
`feedback_no_dev_escape_hatches.md`.

### Per-cycle scan

Every `--underwriter-scan-interval-ms` (default 5 s):

1. `poll_own_status()` — short-circuit if the underwriter's status has
   flipped to `SLASHED` / `TERMINATED`.
2. `read_outpost_registry()` — refresh the `(chain_code → chain_kind)`
   cache from `sysio.chains::chains`.
3. `read_credit_lines()` — compute available bond per
   `(chain, token_kind)` by mirroring the depot's `sysio.opreg::available()`
   math:

       available = balance(opreg::balances)
                 − sum(uwrit::locks where underwriter == self)
                 − sum(opreg::wtdwqueue where account == self)

4. `scan_pending_requests()` — read `sysio.uwrit::uwreqs` via the
   `bystatus` secondary index, filter to `PENDING` rows we are eligible
   for.
5. `select_coverable()` — greedy ascending-by-`src_amount` selection
   (knapsack optimization deferred); reserves both legs' credit so the
   same balance can't be double-used inside a single cycle.
6. `submit_intent_to_outpost()` — for each selected uwreq, verify the
   source-chain deposit, build a signed `UnderwriteIntentCommit` per
   leg, and submit to that leg's outpost.

### Commit submission (`build_signed_uic_bytes`)

For each leg of every selected uwreq:

1. Construct a proto `UnderwriteIntentCommit` with `uw_account`,
   `uw_request_id`, `chain_code`, and a blank `signature`.
2. Serialize the proto, compute `sha256(blanked_bytes)` — the digest.
3. Sign the digest via `signature_provider_manager_plugin::query_providers`
   (WIRE chain kind + K1 key type). The fc::crypto::signature is packed
   via `fc::raw::pack` into the wire format the depot's
   `sysio.uwrit::verify_uic_signature` reads.
4. Place the packed signature back into the proto, re-serialize, and
   submit those bytes verbatim to the outpost — `commit(bytes uicBytes)`
   on Ethereum, `commit_underwrite(uic_bytes)` on Solana.

The outpost auth-checks `msg.sender` / `Signer` as a registered ACTIVE
underwriter and relays the bytes onto the OPP outbound queue. The
depot's `sysio.uwrit::try_select_winner` reconstructs the digest and
verifies the signature against every permission on `uw_account` via the
`get_permission_lower_bound` chain intrinsic.

## Configuration

| Option | Default | Description |
|---|---|---|
| `--underwriter-account` | — | WIRE account name for this underwriter |
| `--underwriter-scan-interval-ms` | 5000 | How often to scan for pending uwreqs (ms) |
| `--underwriter-action-timeout-ms` | 15000 | Timeout for outpost RPC calls + table reads (ms) |
| `--underwriter-enabled` | false | Enable underwriter functionality |
| `--underwriter-eth-outpost` | — | Per-EVM-chain outpost wiring (repeatable, one per served EVM chain). Format `<chain_code>,<client_id>,<operator_registry_addr>,<source_deposit_contract_addr>` — keyed by exact `chain_code`, so two EVM chains are wired independently |
| `--underwriter-sol-outpost` | — | Per-SVM-chain outpost wiring (repeatable, one per served SVM chain). Format `<chain_code>,<client_id>,<opp_outpost_program_id>` |
| `--underwriter-eth-source-deposit-function` | — | Name of the ETH swap-deposit function; the chain-agnostic 4-byte selector is resolved at preflight from the loaded `--ethereum-abi-file` ABIs (required) |
| `--underwriter-sol-source-deposit-instruction` | — | Name of the SOL swap-deposit instruction; the 8-byte anchor discriminator is resolved at preflight from the loaded `--solana-idl-file` IDLs (required) |
| `--underwriter-eth-source-deposit-lookback-blocks` | 7200 | Recent finalized ETH blocks searched per source deposit |

> SEC-13/WSA-027: the former single `--underwriter-eth-client-id`,
> `--underwriter-sol-client-id`, `--underwriter-eth-opreg-addr`, and
> `--underwriter-sol-program-id` options are replaced by the repeatable,
> exact-`chain_code`-keyed `--underwriter-{eth,sol}-outpost` options above.
> One entry is required for **every active** non-depot chain in
> `sysio.chains::chains` (inactive/not-yet-activated chains are skipped);
> the underwriter's per-chain contract / program address now lives in that
> entry rather than in a per-family scalar option.

## HTTP diagnostics

Read-only diagnostic endpoints, served by `http_plugin` on the
read-only exec queue:

- `/v1/underwriter/stats` — session counters + config snapshot:
  underwriter account, enabled/active flags, scan + timeout intervals,
  per-chain outpost wiring (`chain_code`, `kind`, `client_id`,
  `commit_addr`, `source_deposit_addr`), uwreq/commit/failure/mismatch
  counters, outstanding-commit count, SOL source-deposit cursor health.
- `/v1/underwriter/commits` — outstanding confirmed commits, one entry
  per leg: `uwreq_id`, `chain_code`, `token_code`, `reserve_code`.

Both carry a `status` discriminator. Until the deferred startup body
completes they report the startup-gate state instead of the payload
(`waiting_for_sync` with `head_behind_sec` / `lib_behind_sec`,
`preflight_retrying`, or a terminal `preflight_failed` /
`wiring_failed` / `startup_failed` with `detail`); once the gate opens
they serve the payloads above with `status: "active"`.

The endpoints are registered only when the underwriter is enabled:
with the plugin loaded but `--underwriter-enabled false` (the
default), `plugin_startup` skips endpoint registration and every
listener returns 404 for these routes.

### Listener exposure

The endpoints live in the dedicated `underwriter` HTTP API category —
not the always-on `node` category — because they expose operator
metadata (account identity, client ids, outpost contract addresses,
the outstanding-commit ledger).

Default deployments are unchanged: the all-category listeners
(`--http-server-address`, `--unix-socket-path`) serve
`/v1/underwriter/*` as before.

Category-isolated deployments
(`--http-server-address http-category-address`) must opt in
explicitly: listeners without the `underwriter` category return 404
for these routes and omit them from `/v1/node/get_supported_apis`.
Bind the category to loopback or a private management network:

```
nodeop \
  --http-server-address http-category-address \
  --http-category-address underwriter,127.0.0.1:8890 \
  --plugin sysio::underwriter_plugin \
  --underwriter-enabled true \
  ...
```

The listener also serves the node-global endpoints, which are
reachable on every listener by design: `/v1/node/get_supported_apis`
(always registered) and `/v1/chain/get_info` when
`sysio::chain_api_plugin` is loaded — the underwriter depends only on
`chain_plugin`, so the example above does not load it. Like every
category,
`underwriter` is validated against its owning plugin: naming it in
`--http-category-address` without `--plugin sysio::underwriter_plugin`
is a startup configuration error. Binding it to a non-loopback address
logs a startup warning (the same pattern as the `snapshot_ro` exposure
notice).

Query them directly over HTTP, e.g.
`curl http://127.0.0.1:8890/v1/underwriter/stats`.

## Dependencies

- `chain_plugin` — read-only table access against `sysio.opreg`, `sysio.uwrit`, `sysio.authex`, `sysio.chains`.
- `cron_plugin` — scheduled scan loop.
- `signature_provider_manager_plugin` — WIRE K1 signer for the UIC digest.
- `outpost_ethereum_client_plugin` — ETH RPC + ABI loader for the `commit(bytes)` call.
- `outpost_solana_client_plugin` — SOL RPC + IDL loader for the `commit_underwrite(uic_bytes)` call.

## Deferred / follow-up

The current implementation covers the happy-path commit flow end-to-end.
The following hardening / robustness work is out of scope and tracked
for a follow-up:

- **Knapsack selector** — replace the ascending-sort greedy with a
  branch-and-bound search maximizing total committed value subject to
  per-`(chain, token_kind)` credit constraints.
- **Source-deposit locator hardening** — the current verifier validates
  `SwapRequest.source_tx_id` before committing; ETH uses a bounded
  `eth_getLogs` window over finalized blocks, while SOL reads the source
  tx directly. Future work can carry a richer tx/block locator to avoid
  event search entirely.
- **Outstanding-commits tracking + one-leg-stuck retry** — persistent
  in-process map of submitted commits, with retry of a missing leg
  after `max_partial_landing_wait_epochs`.
