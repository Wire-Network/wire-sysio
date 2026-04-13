# sysio.uwrit

Underwriting ledger, intent/confirmation flow, and fee distribution contract.

## Responsibility

- Tracks underwriter collateral per chain (staked, locked, available)
- Manages the underwriting lifecycle: intent → confirmation → remit → completion
- Enforces 24-hour challenge window hold on committed funds
- Distributes fees (0.1% per spoke) split 50/25/25 among underwriter, other underwriters, batch operators
- Handles collateral updates from outpost attestations
- Executes slashing on underwriters (called by `sysio.chalg`)

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `uwconfig` | Singleton | Fee basis points, lock duration, fee share percentages |
| `collateral` | Multi-index | Per-underwriter per-chain collateral tracking |
| `uwledger` | Multi-index | Underwriting entries with status lifecycle |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `setconfig` | `sysio.uwrit` | Set fee/lock configuration |
| `submituw` | underwriter | Submit intent to underwrite a message |
| `confirmuw` | `sysio.uwrit` | Confirm after both outposts acknowledge |
| `expirelock` | permissionless | Release expired locks |
| `distfee` | `sysio.uwrit` | Distribute fees after completion |
| `updcltrl` | `sysio.uwrit` | Update collateral from outpost attestations |
| `slash` | `sysio.chalg` | Seize all collateral from slashed underwriter |

## Dependencies

- Queues outbound attestations via `sysio.msgch`
- Slash called by `sysio.chalg`
