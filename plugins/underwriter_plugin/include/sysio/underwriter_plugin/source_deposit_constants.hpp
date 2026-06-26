#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

#include <fc/network/solana/solana_types.hpp>

namespace sysio::underwriter {

/// ETH: minimum number of block confirmations required before treating
/// a source-deposit tx as final for verification purposes.
///
/// We don't require `finalized` — the plugin's verification window is
/// bounded by the depot's epoch cycle, and waiting for finality (~12
/// minutes on mainnet ETH) would routinely race past the depot's
/// minimum-epoch-duration boundary. 12 confirmations is the same depth
/// most large-stake protocols use as the "practically irreversible"
/// threshold.
///
/// Increase this if the source chain's reorg history grows; decrease if
/// the deposit-to-race latency window shrinks below tolerance.
inline constexpr uint64_t ETH_MIN_CONFIRMATIONS = 12;

/// SOL: the JSON-RPC commitment level used when fetching a source-deposit
/// tx. `confirmed` = the tx has been voted on by a supermajority of the
/// cluster (one-vote level, ~400ms-2s after submission, typical Solana
/// reorg-immunity threshold for non-critical reads). `processed` is
/// faster (leader-only, can reorg) and `finalized` is slower (~12.8s+
/// for 32-slot finality).
///
/// `confirmed` strikes the balance: as short as reasonable while still
/// guaranteeing the tx will eventually land in the canonical chain
/// barring a catastrophic cluster failure. The single point of
/// configuration here means any future bump (e.g. to `finalized` if
/// reorgs become a real concern) is one edit.
inline constexpr auto SOL_COMMITMENT =
   fc::network::solana::commitment_t::confirmed;

/// SOL: maximum page size accepted by Solana JSON-RPC
/// `getSignaturesForAddress`. The underwriter walks one page per retry
/// callback and persists the `before` cursor between outer scan cycles so
/// unrelated newer program traffic cannot keep an older valid source deposit
/// outside the verifier's search window forever.
inline constexpr size_t SOL_SIGNATURE_SCAN_PAGE_SIZE = 1000;

} // namespace sysio::underwriter
