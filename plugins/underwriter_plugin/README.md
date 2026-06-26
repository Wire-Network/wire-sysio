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

`plugin_startup` runs three checks before scheduling the cron job; any
failure logs a structured `elog` and skips cron registration:

1. `sysio.opreg::operators[underwriter_account].status == OPERATOR_STATUS_ACTIVE`.
2. `sysio.authex::links` covers every chain present in
   `sysio.epoch::outposts` — the underwriter cannot sign a commit on a
   chain it has no authex link for.
3. Non-zero balance on at least one TokenKind for every registered
   outpost chain.

No `--strict=false` flag, no dev fallback. Cluster bootstrap is
responsible for establishing the required state — see
`feedback_no_dev_escape_hatches.md`.

### Per-cycle scan

Every `--underwriter-scan-interval-ms` (default 5 s):

1. `poll_own_status()` — short-circuit if the underwriter's status has
   flipped to `SLASHED` / `TERMINATED`.
2. `read_outpost_registry()` — refresh the `(chain_code → chain_kind)`
   cache from `sysio.epoch::outposts`.
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
| `--underwriter-eth-client-id` | `eth-default` | Ethereum outpost RPC client id |
| `--underwriter-sol-client-id` | `sol-default` | Solana outpost RPC client id |
| `--underwriter-eth-opreg-addr` | — | OperatorRegistry contract address on Ethereum (hex) |
| `--underwriter-eth-source-deposit-lookback-blocks` | 7200 | Recent finalized ETH blocks searched per source deposit |
| `--underwriter-sol-program-id` | — | opp-outpost program id on Solana (base58) |

## Dependencies

- `chain_plugin` — read-only table access against `sysio.opreg`, `sysio.uwrit`, `sysio.authex`, `sysio.epoch`.
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
- **Diagnostic `clio` query** — read-only HTTP endpoint exposing
  outstanding-commit state + counters.
