#pragma once

#include <charconv>
#include <cstdint>
#include <string>
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

/// Configuration option name for the EVM source-deposit confirmation depth.
inline constexpr std::string_view ETH_MIN_CONFIRMATIONS_OPTION =
   "underwriter-eth-min-confirmations";

/// ETH: maximum number of recent blocks one source-deposit `eth_getLogs`
/// lookup may cover.
///
/// The verifier only has the outpost-local `SwapDeposit.id`, so it still
/// needs an event lookup. Keep that lookup bounded to protect underwriter
/// scan cycles and EVM RPC providers from whole-history scans on absent or
/// expensive ids. 7200 mainnet blocks is roughly one day at 12 seconds per
/// block; deployments with a different source-chain cadence can override the
/// plugin option without changing code.
inline constexpr uint64_t ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS = 7200;

/// Configuration option name for the EVM source-deposit log lookup window.
inline constexpr std::string_view ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS_OPTION =
   "underwriter-eth-source-deposit-lookback-blocks";

/// JSON-RPC quantity prefix used for explicit EVM block numbers.
inline constexpr std::string_view ETH_JSON_RPC_QUANTITY_PREFIX = "0x";

/// Returns the inclusive lower block bound for a bounded EVM source-deposit
/// log lookup.
///
/// `lookback_blocks` is the maximum inclusive span. A one-block lookback
/// searches only `head_block`; larger values walk backward from the captured
/// head without underflowing below genesis. A zero value is invalid at the
/// plugin option boundary, but this helper defensively collapses it to the
/// current head so arithmetic remains well-defined in tests and callers.
inline constexpr uint64_t eth_source_deposit_from_block(uint64_t head_block, uint64_t lookback_blocks) {
   if (lookback_blocks <= 1) return head_block;
   const uint64_t trailing_blocks = lookback_blocks - 1;
   return head_block > trailing_blocks ? head_block - trailing_blocks : 0;
}

/// Returns whether a lookback window can contain a log old enough to satisfy
/// the configured ETH confirmation depth.
///
/// The lookup range is inclusive and ends at the captured head block. A
/// receipt with exactly `min_confirmations` confirmations is at
/// `head_block - min_confirmations`, so the window must contain at least
/// `min_confirmations + 1` blocks.
inline constexpr bool eth_source_deposit_window_can_satisfy_confirmations(
   uint64_t lookback_blocks, uint64_t min_confirmations) {
   return lookback_blocks > min_confirmations;
}

/// Formats an EVM block number as a JSON-RPC quantity string.
///
/// Ethereum JSON-RPC expects explicit block numbers as lowercase hex with a
/// `0x` prefix and no leading zeroes, e.g. `0x0`, `0xf`, `0x10`.
inline std::string eth_block_quantity(uint64_t block_num) {
   char digits[sizeof(uint64_t) * 2]{};
   auto [ptr, ec] = std::to_chars(digits, digits + sizeof(digits), block_num, 16);
   (void)ec;

   std::string out{ETH_JSON_RPC_QUANTITY_PREFIX.data(), ETH_JSON_RPC_QUANTITY_PREFIX.size()};
   out.append(digits, ptr);
   return out;
}

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

} // namespace sysio::underwriter
