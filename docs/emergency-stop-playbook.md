# Emergency stop playbook

The depot and each outpost carry a freeze of their own (the Andon cord). While a chain is frozen, no
funds leave protocol custody on it; OPP consensus, envelope delivery and epoch advance keep running on
every chain, so funds keep arriving and stay held, and nothing that falls due is skipped. This playbook
is §5.4 of the syndication underwriting specification written out with the exact actions for each
chain. The contracts behind it are described in [`sysio-synd.md`](sysio-synd.md) (The emergency stop),
[`sysio-bond-integration.md`](sysio-bond-integration.md) (The emergency stop) and
[`contract-upgrade-order.md`](contract-upgrade-order.md); the Solana side in wire-solana's
`opp-outpost-technical-spec.md` §7.19 and §8.10; the Ethereum side in wire-ethereum's `docs/deploy.md`
(Syndication pool, Pause).

| Chain | The freeze | Pulled by | Cleared by |
|---|---|---|---|
| Depot | `sysio.andon` `cord` row, `pulled` | `sysio.andon::pull`: delegates of `sysio.andon@pull` | `sysio.andon::clear`: delegates of `sysio.andon@clear` |
| Solana | liqsol-core `GlobalState.frozen` | `set_frozen(true)`: `GlobalConfig.panic` or `GlobalConfig.admin` | `set_frozen(false)`: the same two |
| Ethereum | OpenZeppelin `paused()` on `SyndicationPool`, `LiqEthToken`, `StakingModule` | `pause()`: any holder of the `panic` role (the deploy's `panicAccount` and the admin) | `unpause()`: the same |

The three are independent: pulling one changes nothing on the others. When unsure whether a fault
crossed chains, pull all three.

## Values used below

Every command reads these shell variables. Fill them in before launch and keep them with this
playbook; they do not change between incidents unless an account is replaced.

| Variable | Value |
|---|---|
| `DEPOT_URL` | HTTP endpoint of a depot producer node (`clio -u`) |
| `PANIC` | the depot panic account, delegated by native `updateauth` |
| `SOL_RPC` | Solana RPC URL of the outpost's cluster |
| `SOL_PANIC_KEYPAIR` | keypair file of the Solana panic account (`GlobalConfig.panic`) |
| `SOL_ADMIN_KEYPAIR` | keypair file of the Solana admin (`GlobalConfig.admin`) |
| `SOL_CRANK_KEYPAIR` | any funded Solana keypair: it signs `pay_pending_desyndication` |
| `ETH_RPC_URL` | Ethereum RPC URL of the outpost's network |
| `SYNDICATION_POOL`, `OPP_INBOUND`, `OUTPOST_AUTHORITY`, `OUTPOST_MANAGER` | the `SyndicationPool`, `OPPInbound`, `OutpostManagerAuthority` and `OutpostManager` entries of the outpost deploy's `addressFile` |
| `LIQETH_TOKEN`, `STAKING_MODULE`, `LIQETH_AUTHORITY` | the `LiqEthToken`, `StakingModule` and `LiqEthAuthority` entries of the liqEth deploy's `addressFile` |

A few values belong to the incident itself and are set when it happens: `REASON` (why the cord was
pulled, one line), `NOTE` (what was found, one line), `NEW_PANIC` / `NEW_SOL_PANIC` / `NEW_ETH_PANIC`
and `OLD_ETH_PANIC` (when the panic account is replaced), `PAUSE_BLOCK` (the Ethereum block of the
first `pause()`), `REQUEST_ID` (a deferred desyndication being paid), `RELAY_SIGNER` (a Solana
relay signer's public key), and, for an envelope in doubt, `CHALLENGER`, `CHAIN_CODE`, `TOKEN_CODE`,
`EPOCH_INDEX` and `BOND_REQUEST_ID` (see [Holding and ruling](#holding-and-ruling-steps-5-and-6)), and,
for a skipped desyndication being recredited, `HOLDER` and `QUANTITY` (see
[Reconciling](#reconciling-a-desyndication-the-outpost-did-not-pay)).

Ethereum transactions below are sent with Foundry's `cast` and a keystore named with `--account`
(`--ledger` works the same way). `SyndicationPool` is not deployed in any environment yet
(wire-ethereum `docs/deploy.md`, Rollout); until it is, skip its lines.

Every `clio get table` is written in the v6 form `clio get table <code> <table> -S <scope>`. The tables
read here (`sysio.andon` `cord` and `andonconfig`, `sysio.synd` `envelopes`, `ledger`, `syndcursors` and
`desyndlog`, `sysio.epoch` `epochstate`, `sysio.msgch` `envlog`) are unscoped KV tables stored under
their contract's own account, so the scope is the contract account; rows come back as `{key, value}`.

Depot actions signed by `sysio` are shown as `-p sysio@active`. Where `sysio` is held by a multisig,
propose the same action and data with `clio multisig propose` and execute it once approved.

## Before an incident

Each item is a deploy or upgrade step. An item not done is a freeze that cannot be pulled.

1. **Depot.** `sysio.andon` is deployed privileged before `sysio.swap`, `sysio.liq`, `sysio.bond` and
   `sysio.synd` ([`contract-upgrade-order.md`](contract-upgrade-order.md)); until it exists every
   reader sees a clear cord. Fund permission storage through a RAM-only ROA policy
   from an existing registered node owner (`ROA_ISSUER`), then delegate and link the actions:
   ```bash
   clio -u "$DEPOT_URL" push action sysio.roa addpolicy "[\"sysio.andon\",\"$ROA_ISSUER\",\"0.0000 SYS\",\"0.0000 SYS\",\"0.0100 SYS\",0,0]" -p "$ROA_ISSUER@active"
   AUTH=$(jq -cn --arg panic "$PANIC" '{threshold:1,keys:[],accounts:[{permission:{actor:$panic,permission:"active"},weight:1},{permission:{actor:"sysio",permission:"active"},weight:1}]} | .accounts |= sort_by(.permission.actor)')
   clio -u "$DEPOT_URL" set account permission sysio.andon pull "$AUTH" active -p sysio.andon@active
   clio -u "$DEPOT_URL" set account permission sysio.andon clear "$AUTH" active -p sysio.andon@active
   clio -u "$DEPOT_URL" set action permission sysio.andon sysio.andon pull pull -p sysio.andon@active
   clio -u "$DEPOT_URL" set action permission sysio.andon sysio.andon clear clear -p sysio.andon@active
   clio -u "$DEPOT_URL" get account sysio.andon
   ```
   Governance controls the account's active authority before these links exist. The panic
   account signs using its own key but declares `sysio.andon@pull` or `sysio.andon@clear`.
   Privileged `sysio.synd` automatically pulls with `sysio.andon@active` on a custody shortfall;
   that declared permission must exist. Deploy Andon before syndication carries traffic.
2. **Solana.** Upgrade liqsol-core and run the GlobalState migration,
   `bash-scripts/feature-flags.sh migrate-wire-global-state-liq-fields` (menu entry `mf`, instruction
   `migrate_global_state_liq_fields`; called "the GlobalState migration" below), in the same
   maintenance window (see [Upgrade window](#upgrade-window)). Then name the panic account with the admin keypair
   (`set-panic`, [Solana commands](#solana-commands)) and read it back with `status`.
3. **Ethereum.** Set `panicAccount` in the deploy (or upgrade) config. A fresh deploy maps `pause` and
   `unpause` to the `panic` role from `permissions/outpost.yaml` and `permissions/liqeth.yaml` and
   grants the role to `panicAccount` and to the applying admin. On the outpost only `SyndicationPool`
   is pausable, and since it is not deployed anywhere yet its mapping always comes from
   `permissions/outpost.yaml`. A liqEth environment deployed earlier gets the mapping from the
   permissions-only step `liqeth_20260930.01_permissions` (wire-ethereum `docs/deploy.md`, Rolling the
   panic role out).
4. **Solana relay funding.** Fund the Solana key each `outpost_solana_client` relay signs
   `dispatch_attestations` with (the public key in its `--signature-provider`, logged at start as
   `Signer public key:`) for the rent of the payouts the outpost may store: on a freeze, a custody
   shortfall or a refused settlement, frozen or not (see [Funding](#funding-the-solana-relay-signer)).
5. **Per-transfer maximum.** Set it on every outpost (see [Per-transfer maximum](#per-transfer-maximum)).
   It is a limit, not a switch: the halt is the freeze.

## What stops and what keeps running

### Depot

| While the cord is pulled | Refused or deferred | Still runs |
|---|---|---|
| `sysio.swap` | every action that moves tokens (among them `exchange`, `addliquidity`, `remliquidity`, `withdraw`, `closeext` of a funded deposit, `tickyield` and `accrueyield` with yield owed, `inittoken`) | reads, and actions that move nothing (`sync`, `changefee`, `openext`, `setyield`) |
| `sysio.liq` | `transfer` to anything but a custody contract (`sysio.bond`, `sysio.synd`, `sysio.opreg`), so holders cannot move shadow LIQ between themselves; `claim` by anyone but a custody contract; `queueyield` | `mint`; transfers into a custody contract; `claim` by a custody contract (it moves WIRE into custody) |
| `sysio.bond` | `claim` | `request`, `addbounty`, `accept`, `hold`, `approve`, `rslvvalid`, `rslvinvalid`, `sweepyield`, `prune` |
| `sysio.synd` | the queue step's releases, delivery from `parked`, burns and the forward of a hold share (deferred to the first step after the clear); `desyndicate`, `sweep`, `dropenv`, `sweepyield` (refused: `the andon cord is pulled: funds cannot leave custody`); `linkswept` delivers nothing and returns | intake (`onsynd`, `onyield`, `closeenv`); the queue step's refresh, outcome recording and request issuing; `challenge`; `setconfig`; `importsynd` |
| `sysio.msgch`, `sysio.epoch` | nothing | everything |

A frozen queue step prints `sysio.synd::queue: the andon cord is pulled; releases, deliveries and burns
wait for the clear` and spends none of its budget. On the first tick after clear,
buckets refill for all elapsed epochs, including the freeze, up to their burst cap.
Challenge windows keep counting (they do not extend).

**What the freeze does not cover.** The depot freeze covers syndication and swap custody. Operator
collateral and the other depot payouts are a separate concern and are not frozen:

- `sysio.opreg::claimremit(account, WIRE)` pays through `sysio.token` and runs during a freeze.
- `sysio.opreg::claimremit(account, token_code)` for a shadow LIQ `token_code` pays through `sysio.liq::transfer` to the
  operator, which is not a custody contract, so it IS refused while the cord is pulled; the
  `remitclaims` row stays and is claimed after the clear.
- The WIRE payouts of `sysio.system` (`claimpay`, `claimnodedis`) and `sysio.dclaim` (`claim`) read no
  cord and run.

If an incident involves one of these, the andon cord does not stop it; the owning contract's own
controls (or a contract redeploy) do.

### Solana outpost

| While `frozen` | Refused or deferred | Still runs |
|---|---|---|
| liqsol-core | `synd` (`syndicate_liqsol`), `desynd` (`desyndicate_liqsol`) and `report_liq_yield` refuse with `OutpostFrozen` (6087); a depot `DESYNDICATE_LIQ` is stored as a `PendingPayout` (reason `OutpostFrozen`) instead of paid; `pay_pending_desyndication` refuses | `epoch_in`, `dispatch_attestations` and every other attestation; every instruction outside the syndication surface |

The outpost also stores a `DESYNDICATE_LIQ` while it is not frozen, for three more reasons
(`PendingPayoutReason`, wire-solana `programs/liqsol-core/src/states/pending_payout.rs:33-52` at
bb134198): `CustodyShortfall`, which also freezes the outpost
([A shortfall-triggered pull](#a-shortfall-triggered-pull)); `LegacyUserRecord`; and `SettlementRefused`,
any other chain-state refusal once the recipient resolved, logged as `DesyndicateLIQSettlementRefused
... err_data=<reason>` (`instructions/opp/inbound.rs:5296-5305`). Every one is paid the same way
([Deferred desyndications](#deferred-desyndications)).

`GlobalState.paused` is a different flag with its own meaning; `set_frozen` does not touch it and it is
not the emergency stop. A `DESYNDICATE_LIQ` with request id 0 cannot key a `PendingPayout` and is
recorded unpaid; `sysio.synd` numbers desyndications from 1, so this only happens when the depot and
the outpost have diverged. The relay skips the yield report of a frozen outpost and logs it at info.

### Ethereum outpost

| Contract | `pause()` stops | Keeps running |
|---|---|---|
| `SyndicationPool` | `syndicate` and `realizeYield` (`EnforcedPause`); a depot `DESYNDICATE_LIQ` is stored as a pending desyndication (`DesyndicationDeferred`, reason `OUTPOST_FROZEN`); `payPendingDesyndication` | OPP delivery; views |
| `LiqEthToken` | every liqETH transfer, mint and burn | views |
| `StakingModule` | `batchDeposit`, `topUpValidator` | the rest |

The relay reads `EnforcedPause` from `realizeYield` as expected and logs it at info every epoch until
the pool is unpaused. A `DESYNDICATE_LIQ` with request id 0, or with a request id already stored, is
dropped (`DesyndicationDropped`). While the pool is paused a release is stored with no solvency
comparison (wire-ethereum `contracts/outpost/SyndicationPool.sol:620-627` at d4fbb8f8).

**Order on Ethereum.** `SyndicationPool` pays a desyndication with a liqETH transfer. A release the
token or the pool refuses while the pool itself is unpaused (liqETH paused, the transfer returning false
or reverting, an amount above the pool's principal) is stored as a pending desyndication with reason
`SETTLEMENT_REFUSED` (`DesyndicationDeferred`), not paid, and is paid by `payPendingDesyndication` once
the cause clears (for liqETH, once `LiqEthToken` is unpaused). A pool that would hold less than the
depot's outstanding after the payout, a pool below the payout included, is a custody shortfall: the pool
pauses itself and stores the release with reason `CUSTODY_SHORTFALL`
([A shortfall-triggered pull](#a-shortfall-triggered-pull)). Pausing
`SyndicationPool` first and unpausing it last is still the practice: it stores every release under
`OUTPOST_FROZEN` for the whole freeze instead of attempting transfers against a paused token.

**Pause `LiqEthToken` only when the token itself is at fault.** A paused liqETH blocks every transfer,
mint and burn, including the liqETH staking flows; desyndications arriving while only the token is
paused are stored as `SETTLEMENT_REFUSED` and paid by `payPendingDesyndication` after the unpause.

## Pulling the cord

### Depot

```bash
clio -u "$DEPOT_URL" push action sysio.andon pull "[\"$REASON\"]" -p sysio.andon@pull
clio -u "$DEPOT_URL" get table sysio.andon cord -S sysio.andon    # pulled = true, when, reason
```

`sysio` may pull with `-p sysio@active` and its own name as the actor. The reason is cut to 256 bytes.
Pulling a pulled cord changes nothing and prints `sysio.andon::pull: the cord is already pulled;
nothing changes`.

### Solana

```bash
ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_PANIC_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts freeze
ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_PANIC_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts status
```

(The script is described in [Solana commands](#solana-commands).) The admin keypair works the same way.
The program logs `set_frozen: frozen=true (was false) by <signer>`; setting it again changes nothing.

### Ethereum

Pull `SyndicationPool` first:

```bash
cast send --rpc-url "$ETH_RPC_URL" --account panic "$SYNDICATION_POOL" 'pause()'
cast send --rpc-url "$ETH_RPC_URL" --account panic "$STAKING_MODULE"   'pause()'
for c in "$SYNDICATION_POOL" "$STAKING_MODULE"; do
  cast call --rpc-url "$ETH_RPC_URL" "$c" 'paused()(bool)'
done
```

Only when liqETH itself is at fault (see "Pause `LiqEthToken` only when the token itself is at fault"
above), also:

```bash
cast send --rpc-url "$ETH_RPC_URL" --account panic "$LIQETH_TOKEN" 'pause()'
cast call --rpc-url "$ETH_RPC_URL" "$LIQETH_TOKEN" 'paused()(bool)'
```

Pausing a paused contract reverts (`EnforcedPause`), which is harmless: it was already frozen.

## The playbook

| Step | Real incident | False alarm |
|---|---|---|
| 1 | Pull the cord on the affected chain ([Pulling the cord](#pulling-the-cord)). Pull the others if the fault may have crossed. | Same. When unsure, pull. |
| 2 | Replace the panic account in case its key is the fault ([Replacing the panic account](#replacing-the-panic-account)). | Skip. |
| 3 | Record the block, the epoch and the last accepted envelope digest on each chain ([Recording](#recording-the-state-of-each-chain)). | Same. |
| 4 | Read the depot's `mismatch` table and each outpost's shortfall records, and compare each outpost's custody with the depot's outstanding shadow ([Comparing totals](#comparing-totals), [A shortfall-triggered pull](#a-shortfall-triggered-pull)). | Same. No new `mismatch` row; custody covers the outstanding. |
| 5 | For each envelope in doubt, place a hold (`sysio.synd::challenge`), or rule it invalid (`sysio.bond::rslvinvalid`) if it is unbonded. | None. |
| 6 | `sysio` rules each held request (`rslvvalid` / `rslvinvalid`). Invalid envelopes burn as in §4.7 once the cord clears. | None. |
| 7 | Fix and redeploy the faulty contract while still frozen, in the order of [`contract-upgrade-order.md`](contract-upgrade-order.md). | None. |
| 8 | Repair custody and clear the outposts first. Have each repaired outpost emit a post-repair message (or wait for its next one); wait for depot admission, verify its sequence in `syndcursors` and no `mismatch` row for it, then clear the depot cord and do the work that waited ([Clearing](#clearing-outposts-first-then-the-depot)). | Same admission check before clearing the depot. |
| 9 | Continue monitoring admitted messages from each outpost for `mismatch` rows and `SHORTFALL` ([Comparing totals](#comparing-totals)). Record earlier pre-repair sequences reporting the known shortfall as part of this incident. | Same. |
| 10 | Write up the cause and what was burned or returned ([What to record](#what-to-record)). | Write up what triggered the pull. |

Challenge windows keep counting during a freeze and challenges stay open, so step 5 can be done while
frozen. A frozen queue step still issues requests, so `crank` during the freeze gives every WAITING
envelope a request to hold or rule.

### Holding and ruling (steps 5 and 6)

```bash
clio -u "$DEPOT_URL" get table sysio.synd envelopes -S sysio.synd -r -l 50   # chain_code, token_code, epoch_index, state, request_id
clio -u "$DEPOT_URL" push action sysio.synd crank '[256]' -p "$PANIC@active"   # issues the requests of WAITING envelopes
clio -u "$DEPOT_URL" push action sysio.synd challenge \
  "{\"challenger\": \"$CHALLENGER\", \"chain_code\": $CHAIN_CODE, \"token_code\": $TOKEN_CODE, \"epoch_index\": $EPOCH_INDEX}" \
  -p "$CHALLENGER@active"
clio -u "$DEPOT_URL" push action sysio.bond rslvinvalid "[$BOND_REQUEST_ID]" -p sysio@active
clio -u "$DEPOT_URL" push action sysio.bond rslvvalid   "[$BOND_REQUEST_ID]" -p sysio@active
```

`CHAIN_CODE` and `TOKEN_CODE` are the `chain_code` and `token_code` objects exactly as the `envelopes`
row prints them, `EPOCH_INDEX` its `epoch_index`, and `BOND_REQUEST_ID` its `request_id`. `challenge`
accepts only an envelope in state REQUESTED, RELEASABLE or DONE whose request is OPEN or BONDED: a
WAITING envelope has no request yet, so run `crank` first. `CHALLENGER` must be an account with no
contract code, and not `sysio.synd` or `sysio.bond`. The challenger pays the request's hold bond plus the pair's `challenge_extra`; both transfers go into
custody, so the challenge runs while the depot is frozen. `rslvinvalid` rules a request bonded or not,
so an unbonded envelope in doubt is ruled invalid directly. The queue records each ruling on its next
step; while the cord is pulled the burn of an INVALID envelope and the forward of a VALID ruling's hold
share wait for the clear.

### Replacing the panic account

- Depot: rerun both `set account permission` commands above with `PANIC="$NEW_PANIC"`.
  Replacing the authorities revokes the previous panic account. Keep the governance delegate.
- Solana, as the admin:
  ```bash
  ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_ADMIN_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts set-panic "$NEW_SOL_PANIC"
  ```
- Ethereum, on both access managers. The `panic` role id is read from each manager. On the outpost,
  the deploy's `managerHandoff` moves `ADMIN_ROLE` of `OutpostManagerAuthority` to `OutpostManager` and
  leaves the deployer only the configuration role, so a `grantRole` or `revokeRole` sent straight to
  `OutpostManagerAuthority` reverts. Go through `OutpostManager` instead, signed by the configuration
  role holder: its `grantRole(uint64,address)` grants, and its `execute(address,bytes)` forwards the
  `revokeRole` call to the authority. The liqEth side has no handoff, so its admin calls
  `LiqEthAuthority` directly:
  ```bash
  # Outpost access manager (SyndicationPool), through OutpostManager
  ROLE=$(cast call --rpc-url "$ETH_RPC_URL" "$OUTPOST_AUTHORITY" 'getTargetFunctionRole(address,bytes4)(uint64)' "$SYNDICATION_POOL" "$(cast sig 'pause()')")
  cast send --rpc-url "$ETH_RPC_URL" --account admin "$OUTPOST_MANAGER" 'grantRole(uint64,address)' "$ROLE" "$NEW_ETH_PANIC"
  cast send --rpc-url "$ETH_RPC_URL" --account admin "$OUTPOST_MANAGER" 'execute(address,bytes)' "$OUTPOST_AUTHORITY" \
    "$(cast calldata 'revokeRole(uint64,address)' "$ROLE" "$OLD_ETH_PANIC")"
  # liqEth access manager (LiqEthToken, StakingModule), direct
  ROLE=$(cast call --rpc-url "$ETH_RPC_URL" "$LIQETH_AUTHORITY" 'getTargetFunctionRole(address,bytes4)(uint64)' "$LIQETH_TOKEN" "$(cast sig 'pause()')")
  cast send --rpc-url "$ETH_RPC_URL" --account admin "$LIQETH_AUTHORITY" 'grantRole(uint64,address,uint32)' "$ROLE" "$NEW_ETH_PANIC" 0
  cast send --rpc-url "$ETH_RPC_URL" --account admin "$LIQETH_AUTHORITY" 'revokeRole(uint64,address)' "$ROLE" "$OLD_ETH_PANIC"
  ```
  Then put the new address in `panicAccount` in the deploy config, so the next permissions apply
  grants it rather than the old one.

### Recording the state of each chain

```bash
# Depot: head and LIB, the epoch, the last envelopes accepted per outpost, the cord
clio -u "$DEPOT_URL" get info
clio -u "$DEPOT_URL" get table sysio.epoch epochstate -S sysio.epoch
clio -u "$DEPOT_URL" get table sysio.msgch envlog -S sysio.msgch -r -l 20
clio -u "$DEPOT_URL" get table sysio.andon cord -S sysio.andon

# Solana: slot, next inbound epoch and the last accepted envelope hash, the flags and totals
solana -u "$SOL_RPC" slot
ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_CRANK_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts status

# Ethereum: block, next inbound epoch and the last accepted envelope hash
cast block-number --rpc-url "$ETH_RPC_URL"
cast call --rpc-url "$ETH_RPC_URL" "$OPP_INBOUND" 'nextEpochIndex()(uint32)'
cast call --rpc-url "$ETH_RPC_URL" "$OPP_INBOUND" 'previousEpochHash()(bytes32)'
```

### Comparing totals

The depot compares for you. Every `SYNDICATE_LIQ` and `LIQ_YIELD` an outpost sends carries its live
custody balance (`total_syndicated`), and `sysio.synd` checks it against the depot's outstanding shadow
of the token (the `sysio.liq` supply plus the yield in `liqpending`) as it admits the message
([`sysio-synd.md`](sysio-synd.md), The solvency check). Custody below the outstanding is a shortfall:
it is written to `mismatch` and pulls the cord. Custody above it is normal and only printed as `EXCESS`.

```bash
clio -u "$DEPOT_URL" get table sysio.synd mismatch -S sysio.synd -l 100   # every shortfall: chain_code, token_code, epoch_index, sequence, kind, reported, expected, at
clio -u "$DEPOT_URL" get table sysio.liq stat -S sysio.liq                 # supply per shadow symbol
clio -u "$DEPOT_URL" get table sysio.liq liqpending -S sysio.liq           # released yield not yet queued
clio -u "$DEPOT_URL" get table sysio.synd syndcursors -S sysio.synd        # last_sequence admitted per outpost
clio -u "$DEPOT_URL" get table sysio.synd desyndlog -S sysio.synd -r -l 20 # total_syndicated each desyndication carried
cast call --rpc-url "$ETH_RPC_URL" "$SYNDICATION_POOL" 'poolBalanceDepot()(uint64)'
cast call --rpc-url "$ETH_RPC_URL" "$SYNDICATION_POOL" 'liqSequence()(uint64)'
```

and `status` on Solana (`liq_sequence`, the pool ATA balance). What to check:

- Each `mismatch` row names the outpost, token and sequence of the message that reported the shortfall,
  the custody it reported and the outstanding it was compared with (`expected`). Match the sequence to
  the outpost's `synd` call (Solana) or `Syndicated` / yield event (Ethereum) to find the envelope.
  Rows are never pruned; note the last one read, so the next reading shows only new rows.
- Now: each outpost's custody (the Solana pool ATA, `poolBalanceDepot`) is at least the depot's
  outstanding for its token (`supply` plus `liqpending`). Messages in flight only widen the margin: an
  unpaid or pending desyndication, an INVALID burn and a `recredit` all leave custody above the
  outstanding.
- `syndcursors.last_sequence` for an outpost is at most the outpost's sequence (`liqSequence` /
  `liq_sequence`); the gap is messages the depot has not yet admitted, and so not yet compared.
- Every `desyndlog` request id the outpost has not paid is still in flight on the depot, stored as a
  pending payout on the outpost ([Deferred desyndications](#deferred-desyndications)), or skipped by the
  outpost; a skipped one is reconciled by the recredit rule
  ([Reconciling](#reconciling-a-desyndication-the-outpost-did-not-pay)).
- Between two readings during a freeze, no outpost custody balance falls.


### Reconciling a desyndication the outpost did not pay

Match every `desyndlog` request id with the outpost's record of it. Each one is paid, stored or skipped
([`sysio-synd.md`](sysio-synd.md), Desyndication):

- **Paid**: Solana logs `opp_outpost: desyndicate_liq: paid ... request_id=<id>` (wire-solana
  `programs/liqsol-core/src/instructions/opp/inbound.rs:5759-5760` at bb134198), and after a stored
  record also `pay_pending_desyndication: request_id=<id> paid`;
  Ethereum emits `Desyndicated(recipient, nativeAmount, depotAmount, requestId)` and
  `desyndicationsPaid(requestId)` is true.
- **Stored**: a `PendingPayout` exists at `["pending_desyndication", request_id_le8]` (`npx ts-node scripts/wire-config/emergencyStop.ts
  pending`), or `pendingDesyndications(requestId)` has a non-zero `depotAmount`. Pay it
  ([Deferred desyndications](#deferred-desyndications)). Never recredit it.
- **Skipped**: Solana logged `opp_outpost: DesyndicateLIQError ... request_id=<id> err_data=<reason>`
  (`inbound.rs:4986-4994`) for a content or configuration refusal; Ethereum emitted
  `DesyndicationDropped(requestId, depotAmount, reason)` with `wrong chain`,
  `wrong token`, `amount out of range` or `unresolvable recipient` (wire-ethereum
  `contracts/outpost/SyndicationPool.sol:594-618` at d4fbb8f8).

**The recredit rule.** Governance returns the shadow of a skipped desyndication with
`sysio.liq::recredit`, and only after confirming the outpost holds no record of the request and never
paid it:

```bash
# Solana: the PendingPayout PDA of REQUEST_ID must not exist
ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_CRANK_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts pending   # REQUEST_ID not listed
# Ethereum: nothing stored (depotAmount 0) and never paid (false)
cast call --rpc-url "$ETH_RPC_URL" "$SYNDICATION_POOL" 'pendingDesyndications(uint64)(address,uint64,uint8)' "$REQUEST_ID"
cast call --rpc-url "$ETH_RPC_URL" "$SYNDICATION_POOL" 'desyndicationsPaid(uint64)(bool)' "$REQUEST_ID"
# The depot: the net amount of the request, then the recredit to the holder who signed the desyndicate
clio -u "$DEPOT_URL" get table sysio.synd desyndlog -S sysio.synd -r -l 50   # the row whose request_id is REQUEST_ID
clio -u "$DEPOT_URL" push action sysio.liq recredit "[\"$HOLDER\", \"$QUANTITY\"]" -p sysio.liq@active
```

`recredit` is signed by `sysio.liq` itself (`contracts/sysio.liq/src/sysio.liq.cpp:86`); `QUANTITY` is the
`desyndlog` row's `amount` in the shadow symbol. A stored release is paid by the crank: recrediting it as
well pays the holder twice, and the crank's payment then takes custody down by the amount the recredit
added to the outstanding. The Ethereum drops `already paid` and `already pending`
(`SyndicationPool.sol:609-612`, `:638-641`, `:687-690`) and Solana's `DesyndicateLIQAlreadyPending`
(`inbound.rs:5361-5378`) are
re-emits of an id already paid or stored: never recredit them either. A recredit writes nothing in
`sysio.synd`; the next comparison reads the grown supply, and the custody the outpost never paid out
covers it.

### A shortfall-triggered pull

Each chain names the two readings differently. Custody: the depot's `reported`, Solana's
`pool_balance`, Ethereum's `poolBalanceDepot`. Outstanding: the depot's `expected`, Solana's
`depot_outstanding`, Ethereum's `totalSyndicated`. The depot's `expected` includes the message's own
syndication; the outposts' custody is read before the payout leaves.

A custody shortfall pulls the cord of the chain that found it: the depot finds one when an outpost's
`SYNDICATE_LIQ` or `LIQ_YIELD` reports less custody than the depot's outstanding; an outpost finds one
when a `DESYNDICATE_LIQ` would leave it holding less than the outstanding the depot carried. A shortfall
is not proof of loss: either reading may be wrong. Compare the live balances of
[Comparing totals](#comparing-totals) before ruling envelopes, and pull the other chains if the fault
may have crossed.

**Depot.** A cord pulled by `sysio.synd` is identifiable in the action trace and has a reason of the form
`custody shortfall <chain> <token> seq <n>`. Start at step 3 of [the playbook](#the-playbook): the
`mismatch` row for `(<chain>, <n>)` has the figures (`reported`, `expected`). The message that reported
it was still held and minted, and every later message is compared too: each further shortfall adds its
own row while the cord stays pulled (`CORD ALREADY PULLED -- the shortfall is recorded` on the console).

Repairing custody does not change a `SyndicateLIQ` or `LIQYield` already built: it still carries the
pre-repair custody. If admitted after the depot cord is cleared, that message can write a `mismatch`
row and pull the cord again. Repair custody, have the repaired outpost emit a message after the repair
(or wait for its next one), then wait for the depot to admit that message and confirm no `mismatch`
row for its sequence before clearing the depot cord. Use the `syndcursors` check under
[Clearing](#clearing-outposts-first-then-the-depot). Mismatch rows for earlier pre-repair sequences
reporting the known shortfall are expected: record them under this incident, not as a new incident.

Andon must be deployed before processing envelopes. A failed inline pull aborts the
transaction atomically when the account or declared permission is missing. Bootstrap must
deploy the Andon code as well as create its account before admitting traffic.

**Solana.** The outpost sets its own `GlobalState.frozen`, moves nothing and stores the payout
(`handle_desyndicate_liq`, `inbound.rs:5269-5289`). The program log of the `dispatch_attestations`
transaction carries the evidence:

```
opp_outpost: DesyndicateLIQCustodyShortfall user=<u> token_code=<t> amount=<a> request_id=<id> pool_balance=<b> depot_outstanding=<o> -- outpost frozen, payout stored
opp_outpost: DesyndicateLIQDeferred ... reason=CustodyShortfall { pool_balance: <b>, depot_outstanding: <o> } -- ...
```

`npx ts-node scripts/wire-config/emergencyStop.ts status` shows `frozen: true` and the pool balance; `pending` lists the record with
both figures. `b - a < o` is the shortfall. From then on every syndication instruction refuses with
`OutpostFrozen` and every later `DESYNDICATE_LIQ` is stored as `OutpostFrozen` without being compared
(`inbound.rs:5244`), as in any freeze. Clearing the flag is the operator's decision:
`pay_pending_desyndication` applies no floor (`inbound.rs:5822-5833`), so the stored payout is paid as
soon as the flag clears. Clear only once the pool covers the depot's current outstanding plus every
payout still stored.

**Ethereum.** `SyndicationPool` emits `CustodyShortfall(requestId, poolBalanceDepot, totalSyndicated)`
(`SyndicationPool.sol:236`), pauses itself if it is not paused already, moves no liqETH and stores the
release with reason `CUSTODY_SHORTFALL` (`_recordCustodyShortfall`, `:660-675`). `_handleDesyndicateLIQ`
checks, in order (`:591-652`):

1. content (`wrong chain`, `wrong token`, `amount out of range`), then `already paid`, then the recipient
   (`unresolvable recipient`): each dropped;
2. paused: stored as `OUTPOST_FROZEN` with no comparison (request id 0 dropped);
3. the solvency check: a shortfall emits the event, pauses the pool and stores the release. A request id
   0 still emits the event and pauses the pool, then is dropped (`request id 0 cannot be deferred`); a
   request id already stored still emits the event and pauses the pool, then is dropped as
   `already pending`, the first record kept;
4. `already pending`: dropped;
5. settlement: paid, or a refusal stored as `SETTLEMENT_REFUSED` (request id 0 dropped).

Read the evidence:

```bash
cast logs --rpc-url "$ETH_RPC_URL" --address "$SYNDICATION_POOL" \
  'CustodyShortfall(uint64 requestId, uint64 poolBalanceDepot, uint64 totalSyndicated)'
cast call --rpc-url "$ETH_RPC_URL" "$SYNDICATION_POOL" 'paused()(bool)'    # true
```

Only `SyndicationPool` is paused; pause `StakingModule` or `LiqEthToken` by hand if the
fault may reach them ([Pulling the cord](#pulling-the-cord)). `payPendingDesyndication` pays the stored
release after `unpause`, with the same rule as Solana: unpause only once the pool covers the depot's
outstanding plus every release still stored.

**A negative liqETH rebase** (`Yield.sol` calls `liqEth.decreaseIndex`, wire-ethereum
`contracts/liqEth/Yield.sol:514-518`) lowers the pool's custody with no depot burn: a true shortfall,
raised on both sides. Recovery at v1 is a top-up. Send liqETH to the pool until `poolBalanceDepot()`
covers the depot's outstanding plus every stored release (a plain transfer: a donation only raises
custody), then unpause the pool and pay the stored releases. Have the repaired outpost emit a
post-repair message (or wait for its next one), wait for depot admission and confirm no `mismatch` row
for it as below; only then clear the depot cord. A governance
write-down of the depot's outstanding instead is pending the owner's ruling.

## Clearing: outposts first, then the depot

Clear the outposts first: each one then pays the desyndications it stored and reports its yield again,
while the depot, cleared last, still releases nothing. Clearing the depot last means no release or new
desyndication is made against an outpost that is still frozen. After a custody shortfall, keep the depot
cord pulled until each repaired outpost's post-repair message has been admitted with no mismatch,
as checked in step 3 below; an older message in flight can still carry the pre-repair shortfall.

### 1. Solana

```bash
ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_PANIC_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts clear
ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_CRANK_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts pending
ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_CRANK_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts pay-all
ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_CRANK_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts pending   # empty, or only records that cannot be paid yet
```

### 2. Ethereum

Unpause `SyndicationPool` last, so the liqETH it pays out can move:

```bash
cast send --rpc-url "$ETH_RPC_URL" --account panic "$LIQETH_TOKEN"     'unpause()'   # only if it was paused
cast send --rpc-url "$ETH_RPC_URL" --account panic "$STAKING_MODULE"   'unpause()'
cast send --rpc-url "$ETH_RPC_URL" --account panic "$SYNDICATION_POOL" 'unpause()'
```

Then pay every stored desyndication (see [Paying on Ethereum](#paying-on-ethereum)).

### 3. Depot

After repairing custody and clearing the outposts, have each repaired outpost emit a `SyndicateLIQ`
or `LIQYield` built after the repair (or wait for its next one). Record its `chain_code`, `sequence`
and evidence of emission after the repair. Keep the depot cord pulled while waiting for admission:
intake continues while frozen. Read the per-chain cursor and the mismatch records with clio v6:

```bash
clio -u "$DEPOT_URL" get table sysio.synd syndcursors -S sysio.synd
clio -u "$DEPOT_URL" get table sysio.synd mismatch -S sysio.synd
```

For the repaired outpost's `chain_code`, wait until `syndcursors.last_sequence` reaches the post-repair
message's `sequence`, and confirm there is no `mismatch` row with that `(chain_code, sequence)`.
Read all result pages when checking these tables. Confirm the message was actually admitted: if the
cursor has already advanced past it, a lower-sequence message can be dropped as a replay, so the
cursor alone plus an absent mismatch is not proof that this message passed the custody check.
Use its admission trace, or observe a later post-repair message's admission and check that sequence.
A mismatch on the post-repair message means recovery is not complete; investigate before clearing.
Earlier pre-repair sequences can add mismatch rows while waiting: retain and record them as evidence
of this incident, not as a new incident. Once the post-repair admission check passes for every repaired
outpost, clear the depot cord:

```bash
clio -u "$DEPOT_URL" push action sysio.andon clear "[\"$NOTE\"]" -p sysio.andon@clear
clio -u "$DEPOT_URL" get table sysio.andon cord -S sysio.andon    # pulled = false, when, reason
```

Clearing a clear cord changes nothing and prints `sysio.andon::clear: the cord is not pulled; nothing
changes`. Then drain the queue. Every step that waited during the freeze is done by the first queue
step after the clear: releases, deliveries from `parked`, the INVALID burns and the hold-share
forwards. `closeenv` runs a step inline on each inbound envelope, but only within
`CLOSEENV_QUEUE_LIMIT` (16), so crank it:

```bash
clio -u "$DEPOT_URL" push action sysio.synd crank '[256]' -p "$PANIC@active"
```

`crank` is permissionless (any account may sign it) and never throws. Repeat it until a step releases
nothing more: every `envelopes` row is DONE or INVALID, or waits only on its bucket (`syndication bucket
is empty until it refills`), which refills for every elapsed depot epoch up to its burst cap. A backlog longer than one step
sees needs a crank with a large limit before `sysio.bond`'s prune retention (7 days after a ruling)
elapses (see [`sysio-synd.md`](sysio-synd.md), Operational rule). If the transaction runs out of CPU,
use a smaller limit. After the clear, signed payouts refused during the freeze (`sysio.bond::claim`,
`sysio.liq::claim`, `sysio.synd::sweep`, `sysio.opreg::claimremit` of shadow LIQ) are retried by
whoever is owed; nothing they are owed was lost.

## Deferred desyndications

A desyndication an outpost stored instead of paying (while it was frozen, on a custody shortfall, or
on a refused settlement) was burned on the depot and is held on the outpost. Neither relay pays it:
paying it is this step, once its cause clears (for a freeze or a shortfall, after the outpost clears).
Each record is paid exactly once; a second payment fails. Match each one against the depot's
`desyndlog` by request id.

### Paying on Solana

The record is a `PendingPayout` account at seeds `["pending_desyndication", request_id as 8
little-endian bytes]` under liqsol-core, holding `request_id`, `user`, `token_code`, `amount`,
`rent_payer` and `reason`, 114 bytes in all (`PendingPayout::SPACE`, wire-solana
`programs/liqsol-core/src/states/pending_payout.rs:82`, pinned by its test `:121-128`). The reason is one
of four (`:33-52`):

| Reason | Stored when | Payable once |
|---|---|---|
| `OutpostFrozen` | `GlobalState.frozen` was set | the flag is cleared |
| `LegacyUserRecord` | a share record (pool or user side) was still the legacy layout | the record is migrated (below) |
| `CustodyShortfall { pool_balance, depot_outstanding }` | the solvency check failed; the outpost froze itself ([A shortfall-triggered pull](#a-shortfall-triggered-pull)) | the flag is cleared |
| `SettlementRefused` | any other chain-state refusal once the recipient resolved; the reason is in the `DesyndicateLIQSettlementRefused ... err_data=<reason>` log | that state is repaired |

The tags are per chain: Solana's order is `OutpostFrozen` 0, `LegacyUserRecord` 1, `CustodyShortfall`
2, `SettlementRefused` 3, and Ethereum's differs (below: `SETTLEMENT_REFUSED` is 1 there). Never read
a reason by its number across chains; read it by name.

The settlement logs `DesyndicateLIQDeferred user=... token_code=... amount=... request_id=...
pending=<pda> reason=<reason>` (`instructions/opp/inbound.rs:5403-5411`). `pending` lists every
record (by account type, so none is missed); `pay <request_id>` pays one; `pay-all` pays each in turn
and reports the ones that refuse. `pay_pending_desyndication` refuses, and keeps the record, while the
outpost is frozen (`OutpostFrozen`), before the GlobalState migration has run (`GlobalStateMigrationRequired`), while a share
record is legacy (`LegacyUserRecordMigrationRequired`), outside PostLaunch, and on any chain-state
refusal (`PendingDesyndicationNotPayable` 6089, reason logged). On payment the account closes and its
rent returns to `rent_payer`.

**Legacy share records.** A payout whose pool-side or user-side `UserRecord` still has the legacy
layout is stored with reason `LegacyUserRecord`, frozen or not. Migrate the record, then pay:

- the holder signs `register_user` for their liqSOL ATA, or
- the admin runs `migrate_user_record`, which accepts only a legacy record with no shares and an empty
  ATA (`AmbiguousLegacyState` otherwise); `anchor run migrate-user-records -- --execute` does it in
  batches for every such record.

A legacy record that still carries shares is refused by both (`AmbiguousLegacyState`); its payout stays
stored until that record can be migrated.

### Paying on Ethereum

The record is `pendingDesyndications(requestId)` on `SyndicationPool` (`recipient`, `depotAmount`,
`reason`); a stored record has a non-zero `depotAmount`. Its `reason` (`PendingPayoutReason`, wire-ethereum
`contracts/outpost/SyndicationPool.sol:155-159` at d4fbb8f8, `uint8` in the event and the getter) is one
of three:

| `reason` | Value | Stored when |
|---|---|---|
| `OUTPOST_FROZEN` | 0 | the pool was paused |
| `SETTLEMENT_REFUSED` | 1 | the pool was unpaused but the payout was refused: liqETH paused, the transfer returning false or reverting, or an amount above the pool's principal |
| `CUSTODY_SHORTFALL` | 2 | the solvency check failed and the pool paused itself ([A shortfall-triggered pull](#a-shortfall-triggered-pull)) |

These values are Ethereum's own; Solana numbers its reasons differently (`SettlementRefused` is 3
there). All three are found and paid the same way. A request id is outstanding when it has a
`DesyndicationDeferred` event and no later `Desyndicated`. From the block of the pause (or of the
`CustodyShortfall` event):

```bash
cast logs --rpc-url "$ETH_RPC_URL" --from-block "$PAUSE_BLOCK" --address "$SYNDICATION_POOL" \
  'DesyndicationDeferred(uint64 indexed requestId, address indexed recipient, uint64 depotAmount, uint8 reason)'
# for each requestId (topic 1), as REQUEST_ID:
cast call --rpc-url "$ETH_RPC_URL" "$SYNDICATION_POOL" 'pendingDesyndications(uint64)(address,uint64,uint8)' "$REQUEST_ID"
cast send --rpc-url "$ETH_RPC_URL" --account crank "$SYNDICATION_POOL" 'payPendingDesyndication(uint64)' "$REQUEST_ID"
```

`payPendingDesyndication` is permissionless. It reverts while paused, with `WIRE_NoPendingDesyndication`
once paid (or when nothing is stored), and with `WIRE_PendingDesyndicationUnpayable` when the pool
cannot pay now (the record is kept; retry after the pool's state changes). There is no cancel.

## Funding the Solana relay signer

Storing a payout creates a `PendingPayout` account (114 bytes) whose rent the relay's Solana signer
pays in the `dispatch_attestations` transaction that settles the `DESYNDICATE_LIQ`
(`rent_payer: payer.key()`, `instructions/opp/inbound.rs:5398`). That happens for every stored reason,
frozen or not: a freeze, a custody shortfall, a legacy share record, a refused settlement. It is
refunded to that signer when the payout is paid. A signer that cannot fund the rent aborts the dispatch
window; the epoch waits (nothing is skipped) until the signer is funded and the relay retries. The
relay supplies the record's PDA writable for every non-zero request id
(`plugins/outpost_solana_client_plugin/src/outpost_solana_client.cpp:1416`), so any desyndication may
need the rent. Keep each relay signer funded for the desyndications in flight, and more during a freeze:

```bash
solana -u "$SOL_RPC" rent 114           # lamports per stored payout
solana -u "$SOL_RPC" balance "$RELAY_SIGNER"
```

## Upgrade window

- **Solana.** The `frozen` flag is appended to `GlobalState` (8+131 to 8+132 bytes). Run the GlobalState
  migration in the same maintenance window as the liqsol-core upgrade:
  ```bash
  bash-scripts/feature-flags.sh migrate-wire-global-state-liq-fields             # dry-run
  bash-scripts/feature-flags.sh migrate-wire-global-state-liq-fields --execute
  ```
  (wire-solana `runbooks/post-launch-cleanup.md`). Until it runs, every typed `GlobalState`
  instruction refuses the short account, `set_frozen` included, so the outpost cannot be frozen; a
  `DESYNDICATE_LIQ` aborts its dispatch with `GlobalStateMigrationRequired` (retried after the GlobalState migration, not
  skipped), so the epoch waits; and the relay treats the yield report as not due and logs a warning.
- **Depot.** Deploy `sysio.andon` and configure native pull/clear permissions before the four contracts that read the cord;
  redeploy all five together whenever the `cord` row layout changes
  ([`contract-upgrade-order.md`](contract-upgrade-order.md)).
- **Ethereum.** Run the permissions steps above with `panicAccount` set.

## Pullers

Manage delegates with native `updateauth` and inspect them with `clio get account sysio.andon`.
Pull and clear have separate authorities, so a delegated puller need not be able to clear.
Privileged contracts bypass inline authorization checks: revoking a permission delegate alone
cannot constrain privileged `sysio.synd`. If that privilege is removed, change its inline
permission to `sysio.andon@pull` and grant `sysio.synd@sysio.code` in that authority.

## Per-transfer maximum

Each outpost refuses a single syndication above its maximum. The halt is the freeze, never a maximum of
0: 0 is the fail-closed value of an outpost nobody configured.

- Solana: `GlobalConfig.max_syndication_per_transfer`, in the smallest liqSOL unit (9 decimals), set
  by the admin only (the panic account cannot):
  ```bash
  MAX_SYNDICATION_PER_TRANSFER=250000000000 anchor run set-max-syndication   # 250 liqSOL
  ```
  (wire-solana `runbooks/syndication-maximum.md`). A refused `synd` is `SyndicationExceedsMaximum`
  (6090), or `SyndicationMaximumUnset` (6091) at 0.
- Ethereum: `SyndicationPool.maxSyndicationPerTransfer`, in wei, set through the `config` role (the
  `panic` role cannot):
  ```bash
  cast send --rpc-url "$ETH_RPC_URL" --account admin "$SYNDICATION_POOL" 'setMaxSyndicationPerTransfer(uint256)' 250000000000000000000
  cast call --rpc-url "$ETH_RPC_URL" "$SYNDICATION_POOL" 'maxSyndicationPerTransfer()(uint256)'
  ```

## What to record

Keep one record per incident, real or false alarm:

- who pulled each cord, when, and the reason given (action traces and transaction signatures identify actors; the depot `cord` row keeps only
  `pulled`, `when`, and the latest transition’s `reason`);
- per chain at the pull and at the clear: the block or slot, the epoch, the last accepted envelope
  digest ([Recording](#recording-the-state-of-each-chain));
- the `mismatch` rows and the custody and outstanding readings of [Comparing totals](#comparing-totals), at the pull and after the
  first envelope from each outpost following the clear; for a custody repair, also the post-repair
  message's chain and sequence, emission and admission evidence, the observed `syndcursors.last_sequence`,
  the absence of its `mismatch` row before clearing, and earlier pre-repair mismatch rows;
- for a real incident: each envelope held or ruled, its request id and outcome, what was burned
  (`envelopes.burned`) and what was returned; each redeploy;
- every deferred desyndication: request id, amount, reason, the transaction that paid it, and any still
  unpaid with its refusal; for a `CustodyShortfall` / `CUSTODY_SHORTFALL` record, the two balances it
  carries;
- every skipped desyndication recredited: request id, holder, quantity, and the reads that showed no
  record and no payment;
- whether the panic account was replaced, and the new one;
- for a false alarm: what triggered the pull and what, if anything, should change so it does not recur.

## Solana commands

The freeze instructions are packaged as wire-solana `scripts/wire-config/emergencyStop.ts`, run from the
root of the wire-solana checkout whose `target/idl/liqsol_core.json` and `target/types/liqsol_core.ts`
match the deployed program (an `anchor build` of the deployed commit writes both). Run it directly with
`npx ts-node`, never with `anchor run`: `anchor run` overrides `ANCHOR_PROVIDER_URL` and `ANCHOR_WALLET`
from `Anchor.toml` (a remote cluster and the deployment wallet), which would send a freeze to the wrong
cluster. The script refuses to run unless both variables are set explicitly:

```bash
ANCHOR_PROVIDER_URL="$SOL_RPC" ANCHOR_WALLET="$SOL_PANIC_KEYPAIR" npx ts-node scripts/wire-config/emergencyStop.ts <command> [arg]
```

Before each command it prints the RPC URL, the cluster's genesis hash and the signer's public key. Check
all three (the genesis hash against the one recorded for the outpost's cluster, the signer against the
keypair you meant) before confirming a write. Even `status` and `pending` need `ANCHOR_WALLET` set, to any
keypair file, because the provider requires one; they sign nothing. Keypair per command:
`$SOL_PANIC_KEYPAIR` (or `$SOL_ADMIN_KEYPAIR`) for `freeze` and `clear`, `$SOL_ADMIN_KEYPAIR` for `set-panic`,
`$SOL_CRANK_KEYPAIR` for `pay`, `pay-all`, `status` and `pending`.

| Command | What it does | Signer |
|---|---|---|
| `status` | flags, panic account, maximum, epoch, sequence, totals and the pool balance | any |
| `freeze` / `clear` | `set_frozen(true)` / `set_frozen(false)` | the panic account or the admin |
| `set-panic <pubkey>` | `set_panic` | the admin |
| `pending` | every stored `PendingPayout`, with a shortfall record's two balances | any |
| `pay <request_id>` | `pay_pending_desyndication` for one request id | any |
| `pay-all` | `pay_pending_desyndication` for every stored record, reporting each refusal | any |
