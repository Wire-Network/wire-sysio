# Platform bootstrap configuration

Launch-day chains, tokens, reserves, and swap settings are described by a
single human-authored JSON file validated against a protobuf schema. The
bootstrap tool replays that file onto the chain inside the epoch-0 bootstrap
window, replacing the dataset that was previously hard-coded in the cluster
tooling.

> This is the WIRE **platform** bootstrap config — the launch-day registry
> state (chains, tokens, reserves, global swap settings). The swap reserves it
> seeds are one part of that state; it is not itself a DEX. (Earlier drafts
> called the top-level message `DexConfig`; it is now `BootstrapPlatformConfig`.)

This addresses three launch needs:

| Need | Artifact |
|---|---|
| A T5 allocation set aside to back the WIRE side of reserves | `BootstrapPlatformConfig.t5_reserve_allocation` — a genesis-config WIRE amount; config + bootstrap arithmetic, **no contract change** |
| A T5 allocation set aside for launch DEX integration | `BootstrapPlatformConfig.t5_dex_allocation` — reserved earmark (default 0); the on-chain DEX-seeding mechanism is TBD (launch total set by Kyle / Ken) |
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
- `etc/config/dex/dex-config.dev.json` — 1:1 mirror of the dev-cluster dataset
  (9 tokens, 8 reserves), proving the schema carries the full set and driving
  the test.
- `libraries/opp/test/test_bootstrap_platform_config.cpp` — strict-parse +
  invariant test in the `test_opp` binary.

## Schema

`BootstrapPlatformConfig` holds the schema version, a deployment label, the two
T5 earmarks (`t5_reserve_allocation`, `t5_dex_allocation`), and repeated
`ChainSpec` / `TokenSpec` / `ReserveSpec` plus a single `UwritConfig`.

The spec messages deliberately differ from the registry carriers in
`sysio/opp/types/types.proto` (`Chain` / `Token` / `ChainToken` / `Reserve`),
which are wire messages with packed-uint64 codes, raw `bytes` addresses, and
lifecycle fields that are outputs. A hand-authored config wants the opposite:

- **Codes are strings** (`"ETHEREUM"`, `"USDC"`, `"PRIMARY"`); the tool packs
  them via `slug_name` (`[A-Z0-9_]`, ≤ 8 chars).
- **Addresses are strings** in chain-native display form (`0x`-hex for EVM,
  base58 for SVM) so each is verifiable against a block explorer. `bytes` would
  render as base64 in JSON.
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
| `ReserveSpec` | `sysio.reserv::regreserve(chain_code, token_code, reserve_code, name, description, initial_chain_amount, initial_wire_amount, connector_weight_bps, is_private, owner)` |
| `UwritConfig` | `sysio.uwrit::setconfig(fee_bps, collateral_lock_duration_ms)` |
| `t5_reserve_allocation` | none — feeds the `setemitcfg` arithmetic below |
| `t5_dex_allocation` | none yet — reserved earmark; carved out of T5 alongside `t5_reserve_allocation` once the DEX-seeding path lands |

The `regreserve` signature (with `is_private` + `owner`) and the ms-based
`setconfig` are the reserve-and-swap-beta surface; this config slots directly
onto them. `owner` is a WIRE account name (`sysio::name`): empty for public
reserves, the owning account for private ones.

## T5 reserve earmark

With `A` = the launch T5 allotment, `E` = `t5_reserve_allocation`, and
`W` = Σ `reserves[].initial_wire_amount`:

`regreserve` drains each reserve's WIRE side from the `sysio` emissions
treasury at registration (a real inline `sysio.token::transfer` authorized by
`sysio@active`). The emissions formula gates on
`t5_distributable − t5_floor − total_distributed`, and the per-epoch readiness
gate independently checks the real treasury balance, so the earmark must sit
**outside** the distributable pool:

1. Config invariant: `0 < W ≤ E`.
2. Bootstrap sets `setemitcfg.t5_distributable = A − E` (`t5_floor` unchanged —
   the floor is inside the distributable pool).
3. End-of-bootstrap `sysio` balance ≥ `t5_distributable + E`; after reserves
   drain `W`, balance ≥ `t5_distributable`, so the readiness gate never trips
   because of reserve funding.
4. Remainder `E − W` stays in the treasury, inert and outside emissions
   accounting.

Putting `E` *inside* `t5_distributable` instead would make the emissions math
count WIRE that has physically left the treasury — the readiness gate blocks at
launch scale, and effective emissions headroom silently shrinks by `W`. Hence
the outside-the-pool earmark.

Each bootstrap `regreserve` transaction must carry `sysio@active` (for the
treasury drain) alongside `sysio.reserv@active`.

## T5 DEX earmark

`t5_dex_allocation` (`D`) is a second, parallel carve-out of the T5 allotment,
reserved for launch DEX integration — at least one DEX is seeded at launch, with
more added later by council vote. It follows the same outside-the-pool rule as
the reserve earmark, so the full T5 carve-out is `E + D` and
`t5_distributable = A − E − D`.

It is **reserved**: the on-chain DEX-seeding mechanism is not finalized, so the
bootstrap tool does not yet drain `D` — the field is carried for forward
compatibility and defaults to `0` (earmark disabled). The launch total is a
per-deployment economics decision (Kyle / Ken own the figure). When the seeding
path lands, the `A − E` arithmetic above generalizes to `A − E − D` with no
shape change to this config.

## Validation

Parsing is **strict**: unknown / misspelled keys are rejected, not dropped,
because the file is hand-authored and drives irreversible actions. A validator
(reproduced in the test, and to be shared with the bootstrap tool) enforces:

| # | Invariant |
|---|---|
| V1 | `schema_version == 1`; `network` non-empty |
| V2 | every code is a valid slug (`[A-Z0-9_]`, ≤ 8 chars) |
| V3 | chain codes unique; exactly one `CHAIN_KIND_WIRE` chain, code `WIRE` |
| V4 | token codes unique; `chain_code` declared; `precision` ∈ 1..18; native ⇔ kind `NATIVE` + empty address; non-native address well-formed for the chain kind (EVM `0x`+40 hex; SVM base58 → 32 bytes) |
| V5 | exactly one native token per non-depot chain |
| V6 | reserve `(chain, token, code)` unique; references a declared binding; not on the depot; `0 < connector_weight_bps ≤ 10000`; amounts > 0 |
| V7 | Σ `initial_wire_amount` ≤ `t5_reserve_allocation` > 0 |
| V8 | `is_private` ⇒ `owner` a valid account name; else `owner` empty |
| V9 | `uwrit` present; `fee_bps ≤ 10000`; lock duration > 0 |

Run the test:

```bash
ninja -C build test_opp
./build/libraries/opp/test_opp --run_test=bootstrap_platform_config
```

## Follow-ups

- Wire the bootstrap tool to consume `BootstrapPlatformConfig` (the cluster phases become a
  loop over the parsed config, same order: chains → tokens → bindings →
  reserves → uwrit), injecting per-deployment contract addresses from
  deployment artifacts into the dev config.
- Regenerate the TS/Solidity/Solana model bundles so the tool imports the
  generated `BootstrapPlatformConfig` type.
- Fill in launch economics (`t5_reserve_allocation` + per-reserve amounts) and
  verified mainnet addresses; freeze `dex-config.mainnet.json`.

## Open questions

- The earmark `E` and per-reserve amounts are economics decisions
  (placeholders today).
- Launch token set: Solana SPL stables? Liquid-staking tokens (addresses exist
  only once those contracts deploy)?
- Whether to seed any private reserves directly at launch, or create them all
  post-launch via the authex-gated outpost flow (default assumption: all
  post-launch).
- Whether to unify the emissions genesis numbers (`A`, `t5_distributable`,
  `t5_floor`) with this config so the `A − E` arithmetic is checkable in one
  place.
