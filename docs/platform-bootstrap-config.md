# Platform bootstrap configuration

Launch-day chains, tokens, shadow-liq yield pools and pre-launch syndicated
positions are described by a single human-authored JSON file validated against
a protobuf schema. The bootstrap tool replays that file onto the chain inside
the epoch-0 bootstrap window (`sysio.epoch` `current_epoch_index == 0`), where
every registration is privilege-gated and lands active inline.

| Need | Artifact |
|---|---|
| A T5 allocation set aside to back the WIRE side of the launch yield pools | `BootstrapPlatformConfig.t5_dex_allocation` — drained by `sysio.liq::regliqpool` |
| A file the bootstrap tool reads for these settings | `etc/config/dex/dex-config.*.json` |
| A defined input shape for that file | `libraries/opp/proto/sysio/opp/bootstrap/bootstrap.proto` → message `BootstrapPlatformConfig` |

The schema lives with the other OPP protos, so the existing codegen pipelines
emit the host C++ (`bootstrap.pb.h`), contract (`bootstrap.pb.hpp`), and
TS/Solidity/Solana models with no build-system changes. The JSON file is the
canonical protobuf-JSON encoding of `BootstrapPlatformConfig`.

## Files

- `libraries/opp/proto/sysio/opp/bootstrap/bootstrap.proto` — the schema.
- `etc/config/dex/dex-config.launch.example.json` — strawman mainnet launch
  config (placeholder economics; mainnet token addresses marked
  VERIFY-BEFORE-LAUNCH).
- `etc/config/dex/dex-config.dev.json` — the dev-cluster dataset (3 chains,
  9 tokens, 2 liq pools, 3 syndications).
- `libraries/opp/test/test_bootstrap_platform_config.cpp` — strict-parse +
  invariant test in the `test_opp` binary.
- `contracts/tests/sysio.synd_tests.cpp` — parses the dev config the same
  strict way to drive its launch-replay cases.

## Schema

`BootstrapPlatformConfig` holds the schema version, a deployment label, the T5
earmarks, and repeated `ChainSpec` / `TokenSpec` / `LiqPoolSpec` /
`SyndicationSpec`.

The spec messages deliberately differ from the registry carriers in
`sysio/opp/types/types.proto` (`Chain` / `Token` / `ChainToken`), which are wire
messages with packed-uint64 codes, raw `bytes` addresses, and lifecycle fields
that are outputs. A hand-authored config wants the opposite:

- **Codes are strings** (`"ETHEREUM"`, `"USDC"`, `"LIQETH"`); the tool packs
  them via `slug_name` (`[A-Z][A-Z0-9_]{0,7}` -- a code must START with a
  letter, ≤ 8 chars).
- **Addresses and pubkeys are strings** in chain-native display form (`0x`-hex
  for EVM, base58 for SVM) so each is verifiable against a block explorer.
  `bytes` would render as base64 in JSON.
- **Amounts are uint64 subunits** (9-decimal unless `precision` says
  otherwise). Proto3 canonical JSON renders 64-bit integers as **quoted
  strings** — amounts must be quoted in authored JSON, since launch-scale
  values exceed 2^53.
- **Enums appear by full value name** (`"TOKEN_KIND_ERC20"`), reusing the
  `ChainKind` / `TokenKind` enums via import.
- `ChainToken` binding is folded into `TokenSpec` (a token binds to exactly one
  chain; multi-chain assets use distinct codes such as `USDC` vs `USDCSOL`).
- There is no `is_depot` field — the depot chain is the single
  `CHAIN_KIND_WIRE` entry.

### Mapping to bootstrap actions

| Spec | Action(s) |
|---|---|
| `ChainSpec` | `sysio.chains::regchain(kind, code, external_chain_id, name, description)` |
| `TokenSpec` | `sysio.tokens::regtoken(kind, code, symbol_name, description, precision, address)` then `sysio.tokens::regctok(chain_code, token_code, contract_addr, is_native)` |
| `LiqPoolSpec` | `sysio.liq::regliqpool(chain_code, token_code, pair_symbol, initial_chain_amount, initial_wire_amount, fee, locked_shares, conversion_horizon_sec, depth_cap_bps, clip_floor)` — mints the LCO shadow to `sysio`, deposits it with the WIRE side from the `sysio` treasury into `sysio.swap`, and creates the pair with the shadow as its yield leg. The shadow symbol must already exist; exactly one pool per shadow |
| `SyndicationSpec` | `sysio.synd::importsynd(chain_code, token_code, credits[] {pubkey, amount})`, batched by the tool; `sysio.synd::importdone` closes the import |
| `t5_dex_allocation` | none — feeds the `setemitcfg` arithmetic below |

`TokenSpec.precision` is the **depot-frame precision**, `min(native precision,
9)`. `sysio.tokens::regtoken` rejects anything above 9, which is why V4 bounds
`precision` at 9 rather than 18: a token declared at, say, 18 would satisfy a
wider validator and then abort the irreversible bootstrap at `regtoken`. Tokens
whose native precision exceeds the frame (ETH at 18) declare the frame value and
are downscaled at the outpost boundary.

`SyndicationSpec.pubkey` is the identity `SyndicateLIQ.user` carries: base58 of
the 32-byte Ed25519 key on SVM, 0x-hex of the 33-byte **compressed** secp256k1
point on EVM (never the 20-byte address). Each credit mints to the account that
pubkey has linked through `sysio.authex`, or parks it until the link is made.

### Launch schema

`ReserveSpec`, `UwritConfig`, `reserves`, `uwrit` and `t5_reserve_allocation` are removed from the
pre-launch schema. Authored JSON uses schema version 1; strict parsing rejects removed keys.
The schema carries only the DEX earmark. Subtract any separately configured kicker earmark
as described below. Regenerate matching OPP models and bootstrap clients for a fresh deployment.

## T5 DEX earmark

With `A` = the launch T5 allotment, `D` = `t5_dex_allocation`, and
`W` = Σ `liq_pools[].initial_wire_amount`:

`regliqpool` drains each pool's WIRE side from the `sysio` emissions treasury at
registration. The emissions formula gates on
`t5_distributable − t5_floor − total_distributed`, and the per-epoch readiness
gate independently checks the real treasury balance, so the earmark must sit
**outside** the distributable pool:

1. Config invariant: `W ≤ D`, and `D > 0` whenever any pool is seeded.
2. Bootstrap sets `setemitcfg.t5_distributable = A − D` (`t5_floor` unchanged —
   the floor is inside the distributable pool).
3. End-of-bootstrap `sysio` balance ≥ `t5_distributable + D`; after the pools
   drain `W`, balance ≥ `t5_distributable`, so the readiness gate never trips
   because of pool funding.
4. Remainder `D − W` stays in the treasury, inert and outside emissions
   accounting.

Putting `D` *inside* `t5_distributable` instead would make the emissions math
count WIRE that has physically left the treasury — the readiness gate blocks at
launch scale, and effective emissions headroom silently shrinks by `W`. Hence
the outside-the-pool earmark.

## Kicker deployment and earmark

Deploy `sysio.kicker` privileged with `sysio.roa::setsyscode`, alongside LIQ and swap.
After `setemitcfg`/`initt5` and each `regliqpool`, governance (`sysio`) calls
`sysio.kicker::setconfig({budget_remaining, min_interval_sec})`, then
`addpool(sym, rate_bps, min_gift, max_gift_per_day)` for each LIQ token. The
C++ defaults are 200 bps and 1,000,000,000 WIRE subunits; all action fields must
be supplied by ABI clients. Zero disables the per-pool daily cap. A third LIQ
requires these same configuration calls and no kicker code change. Any keeper
may call `kick(sym)`; `setpool` changes the whole open interval's rate and
`rmpool` stops accrual. Re-adding starts a new clock and pays no history.

The kicker earmark `K` uses a governance action, **not a new bootstrap proto field**.
Set `t5_distributable = A - D - K` and retain enough WIRE to back both earmarks.
`setconfig` replaces the remaining budget; it is not an additive top-up and does
not validate the off-chain earmark arithmetic. Governance must back every replacement
with treasury funds outside emissions. The reserve guard protects pending emissions,
claims, and the remaining emission ceiling; it does not reserve node-owner vest. Partial payments advance
a floored pro-rata clock. Arrange and monitor actual kicks: the minimum interval
is a floor, and spot-price manipulation exposure scales with the unpaid interval.
The legacy Python boot tool does not deploy the LIQ/swap subsystem; the platform
bootstrap client must create/deploy/configure the kicker in its LIQ flow.

## Validation

Parsing is **strict**: unknown / misspelled keys are rejected, not dropped,
because the file is hand-authored and drives irreversible actions. The
validator in `test_bootstrap_platform_config.cpp` enforces:

| # | Invariant |
|---|---|
| V1 | `schema_version == 1`; `network` non-empty |
| V2 | every code is a valid slug: `[A-Z][A-Z0-9_]{0,7}` -- leading character must be a letter, ≤ 8 chars |
| V3 | chain codes unique; exactly one `CHAIN_KIND_WIRE` chain, code `WIRE` |
| V4 | token codes unique; `chain_code` declared; `precision` ∈ 1..9 (the depot frame); native ⇔ kind `NATIVE` + empty address; non-native address well-formed for the chain kind (EVM `0x`+40 hex; SVM base58 → 32 bytes) |
| V5 | exactly one native token per non-depot chain |
| V6–V9 | retired with the external-reserve and underwriting configuration |
| V10 | `t5_dex_allocation > 0` when any liq pool is seeded; Σ `liq_pools[].initial_wire_amount` ≤ `t5_dex_allocation` |
| V11 | each liq pool references a declared `TOKEN_KIND_LIQ` token on its chain and is unique; `pair_symbol` 1..7 characters `[A-Z]`; seeds > 0; `fee ≤ 9999`; `conversion_horizon_sec > 0`; `depth_cap_bps` ∈ 1..10000; `clip_floor > 0` |
| V12 | each syndication references a declared liq token on its chain; `pubkey` fits the chain family; `amount > 0` |
| V13 | a non-zero `custody_total` equals the pool's `initial_chain_amount` plus the sum of that token's `syndications` |

Run the test:

```bash
ninja -C build test_opp
./build/libraries/opp/test_opp --run_test=bootstrap_platform_config
```

## Follow-ups

- Wire the bootstrap tool to consume `BootstrapPlatformConfig` (the cluster
  phases become a loop over the parsed config, in order: chains → tokens →
  bindings → liq pools → syndication import), injecting per-deployment contract
  addresses from deployment artifacts into the dev config.
- Fill in launch economics (`t5_dex_allocation` + per-pool amounts) and verified
  mainnet addresses; freeze `dex-config.mainnet.json`.

## Open questions

- The earmark `D` and per-pool amounts are economics decisions (placeholders
  today).
- Whether to unify the emissions genesis numbers (`A`, `t5_distributable`,
  `t5_floor`) with this config so the `A − D` arithmetic is checkable in one
  place.
