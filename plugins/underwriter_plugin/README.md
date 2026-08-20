# underwriter_plugin

Autonomous underwriter daemon. Polls `sysio.uwrit::uwreqs` for PENDING
swaps, derives each candidate's authoritative stored-evidence state, and
submits signed `UnderwriteIntentCommit` records only for missing outpost legs.
Before submitting, it budgets the candidate's full eventual depot collateral
requirement, including stored or locally confirmed legs whose locks do not yet
exist. A complete depot candidate never automatically replays a paid outpost
commit; it remains reserved while the request awaits an external exact-UIC
replay.

The underwriter is a **separate daemon** from the batch operator. It does
not relay OPP envelopes — that is the batch operator's job. The
underwriter signs and submits underwriting commits to outposts. The depot
receives those commits through ordinary OPP dispatch and performs the terminal
winner-selection decision.

## Lifecycle

### Startup pre-flight (unconditional, no dev escape hatch)

`plugin_startup` runs a series of checks before scheduling the cron job;
any gating failure logs a structured `elog` and skips cron registration:

1. The underwriter exists in `sysio.opreg::operators`. Its status is observed
   but non-gating because bootstrap may activate it after node startup;
   `poll_own_status()` blocks work until it is `OPERATOR_STATUS_ACTIVE`.
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
4. The required source-deposit function / instruction names resolve
   against the loaded ABI / IDL files; exactly one operator-configured WIRE
   UIC provider whose public key is a direct key whose weight alone reaches the
   underwriter account's current `owner` or `active` threshold is selected
   (automatically registered defaults and unrelated WIRE/block-signing
   providers are ignored);
   its public key and self-test signature use the same supported
   fixed-size recoverable variant (K1, R1, EM, or ED); and the signature
   self-test uses compact recovery headers `31..34` for K1/R1 or Ethereum
   recovery values `27..30` for EM, is scalar-valid and low-`s` for ECDSA,
   and recovers that threshold-satisfying key. If multiple
   operator-configured providers can each satisfy one of those permissions,
   startup fails as ambiguous instead of selecting by registry order. The
   provider targets `chain=wire`; its native key type is
   `wire` for K1/R1, `ethereum` for EM, or `solana` for ED.

Raw collateral is observed during preflight but zero balance is non-fatal:
bootstrap may deposit after the node starts. Runtime admission uses each
request's exact `(chain_code, token_code)` buckets, so an unrelated active
chain with no collateral does not halt otherwise coverable work.

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
   `(chain_code, token_code)` by mirroring the depot's `sysio.opreg::available()`
   math:

       available = balance(opreg::balances)
                 − sum(uwrit::locks where underwriter == self)
                 − sum(opreg::wtdwqueue where account == self)

4. `scan_pending_requests()` — walk the small in-flight
   `sysio.uwrit::uwreqs` KV table, filter to `PENDING` rows, and derive this
   underwriter's absent, partial, complete, or terminal candidate state
   from the authoritative `commits_by` row.
5. Remove terminal candidates before cover selection. For a complete stored
   candidate, reserve its full eventual bond while it awaits an external exact
   UIC replay; do not automatically replay either paid outpost transaction.
6. For candidates whose remaining legs are only locally confirmed, reserve the
   full eventual bond while OPP catches up, then remove them from submission
   work. Saturating reservation prevents a concurrent shortfall from exposing
   leftover capacity to a new paid commit.
7. For remaining absent/partial candidates, run the bounded branch-and-bound
   selector (with its value-sorted fallback) over every eventual non-depot
   lock, including a leg whose UIC is already stored or locally confirmed. The
   selector uses the daemon's current credit snapshot to avoid knowingly
   submitting a candidate the depot cannot cover. `submit_intent_to_outpost()`
   still sends only missing legs. This pre-validation is advisory because
   collateral can change before OPP delivery: the contract rechecks live state
   atomically, creates locks on success, and otherwise disqualifies only that
   candidate while the request remains PENDING for another underwriter.

Fresh outpost evidence, including an exact replay of a stored UIC, can make
request-level terminal decisions such as a variance rejection. If the route,
quote, or liquidity is still unsuitable, the row remains PENDING for another
external replay or for expiry pruning.

### Commit submission (`create_signed_uic_bytes`)

For each leg of every selected uwreq:

1. Construct a proto `UnderwriteIntentCommit` with `uw_account`,
   `uw_request_id`, `chain_code`, and a blank `signature`. Populate
   `uw_ext_chain_addr` from the same concrete outpost client's authenticated
   transaction signer (20-byte EVM address or 32-byte Solana public key), so
   every production field has one unambiguous serialized representation.
2. Serialize the proto, compute `sha256(blanked_bytes)` — the digest.
3. Sign with the permission-authorized provider selected and cached during
   startup preflight. Runtime signing never re-queries or switches providers;
   a null cache fails closed and no UIC is emitted. Named and anonymous
   `--signature-provider` entries qualify, but generated defaults and unrelated
   WIRE signers do not. K1/R1 use the `wire` native key type, EM uses
   `ethereum`, and ED uses `solana`. The fc::crypto::signature is packed via
   `fc::raw::pack` into the wire format the depot's
   `sysio.uwrit::verify_uic_signature` reads.
4. Place the packed signature back into the proto, re-serialize, and
   submit those bytes verbatim to the outpost — `commit(bytes uicBytes)`
   on Ethereum, `commit_underwrite(uic_bytes)` on Solana.

The current outposts require an ACTIVE-role transaction caller, decode and
canonically re-encode the complete UIC, and require the original bytes to match.
They bind `uw_account` and `uw_ext_chain_addr` to that authenticated caller and
its current roster row, then enqueue the original validated UIC bytes unchanged.
The outposts do not validate the embedded WIRE permission signature or bond;
those remain the depot's authoritative responsibilities. Before storing a leg,
the depot first requires the complete signed protobuf bytes to equal the
CDT generator's canonical re-encoding. It then reconstructs the digest and
accepts exactly the canonical packed fixed-size recoverable shapes: K1/R1/EM
(variant tags `0`/`1`/`3` plus a 65-byte ECC body) or ED (tag `4` plus a
96-byte body). K1 and R1 require recovery headers `31..34`; EM requires recovery
values `27..30`. All three ECDSA variants additionally require in-range scalars
and low-`s`.
It constructs that known variant directly, recovers the signing key, and
accepts it only when that direct key's weight alone reaches the `active` or
`owner` permission threshold on the claimed `uw_account`. WebAuthn and BLS
remain unsupported. Malformed,
noncanonical, unsupported, or unauthorized incoming signatures are logged and
ignored without changing candidate evidence or aborting consensus dispatch.
When a second required leg arrives, winner selection revalidates only the older
stored leg so permission-key rotation cannot authorize stale evidence. A
candidate disqualified by that check remains durably disqualified for the
request; later records cannot replace its evidence, refresh its timestamps, or
re-arm it. The `UIC_SIGNATURE_REJECTED` marker names the
account field as `claimed_underwriter` and reports the provenance-bound
`chain_code`; it identifies the rejected candidate claim, not an independently
authenticated depot submitter.

## Configuration

| Option | Default | Description |
|---|---|---|
| `--underwriter-account` | — | WIRE account name for this underwriter |
| `--underwriter-scan-interval-ms` | 5000 | How often to scan for pending uwreqs (ms) |
| `--underwriter-action-timeout-ms` | 15000 | Timeout for outpost RPC calls and table reads (ms) |
| `--underwriter-enabled` | false | Enable underwriter functionality |
| `--underwriter-eth-source-deposit-function` | — | Name of the ETH swap-deposit function; the chain-agnostic 4-byte selector is resolved at preflight from the loaded `--ethereum-abi-file` ABIs (required) |
| `--underwriter-sol-source-deposit-instruction` | — | Name of the SOL swap-deposit instruction; the 8-byte anchor discriminator is resolved at preflight from the loaded `--solana-idl-file` IDLs (required) |
| `--underwriter-eth-source-deposit-lookback-blocks` | 7200 | Recent finalized ETH blocks searched per source deposit |

### Outpost wiring

Nothing about an outpost is declared per node.

* **Which chains** — every active non-depot `sysio.chains` row. There is no
  per-chain config entry to keep in sync with the registry.
* **Where each one lives** — the row's `outpost` struct: the EVM
  OperatorRegistry (the `uw_commit` target) and source-deposit contract, or the
  single SVM outpost program, which stands in for both roles. Every underwriter
  therefore commits against the same deployment.
* **How to reach it** — the RPC client registered under that chain's **own
  code**. `--outpost-ethereum-client` / `--outpost-solana-client` take the client
  id as their first field, and for an outpost that id must be the chain code
  (`ETHEREUM`, `SOLANA`, ...).

Preflight fails closed when an active chain has no RPC client registered under
its code, or when its row carries no contract addresses. A
`sysio.chains::setoutpost` redeploy is picked up on the next scan tick — the
client handle is rebuilt against the new address.

SEC-13/WSA-027 is preserved by construction: the client is keyed by exact chain
code, so two chains of the same VM family are wired independently and a
wrong-family entry is simply a client that is not there.

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
- `signature_provider_manager_plugin` — fixed-size recoverable WIRE signer for
  the UIC digest (K1, R1, EM, or ED).
- `outpost_ethereum_client_plugin` — ETH RPC + ABI loader for the `commit(bytes)` call.
- `outpost_solana_client_plugin` — SOL RPC + IDL loader for the `commit_underwrite(uic_bytes)` call.

## Deferred / follow-up

The current implementation covers canonical commit construction, bounded
cover selection, full-candidate collateral pre-validation, terminal depot
resolution, and restart-safe missing-leg routing. The following optional
locator improvement remains out of scope:

- **Source-deposit locator hardening** — the current verifier validates
  `SwapRequest.source_tx_id` before committing; ETH uses a bounded
  `eth_getLogs` window over finalized blocks, while SOL reads the source
  tx directly. Future work can carry a richer tx/block locator to avoid
  event search entirely.
