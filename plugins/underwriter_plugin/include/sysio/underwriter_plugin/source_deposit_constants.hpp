#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <fc/network/solana/solana_types.hpp>

namespace sysio::underwriter {

/// ETH: maximum number of recent blocks one source-deposit `eth_getLogs`
/// lookup may cover.
///
/// The verifier only has the outpost-local `SwapDeposit.id`, so it still
/// needs an event lookup. Keep that lookup bounded to protect underwriter
/// scan cycles and EVM RPC providers from whole-history scans on absent or
/// expensive ids. The window is anchored on Ethereum's `finalized` block so
/// the underwriter never fronts collateral against reversible source-chain
/// history. 7200 mainnet blocks is roughly one day at 12 seconds per block;
/// deployments with a different source-chain cadence can override the plugin
/// option without changing code.
inline constexpr uint64_t ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS = 7200;

/// Configuration option name for the EVM source-deposit log lookup window.
inline constexpr std::string_view ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS_OPTION =
   "underwriter-eth-source-deposit-lookback-blocks";

/// JSON-RPC quantity prefix used for explicit EVM block numbers.
inline constexpr std::string_view ETH_JSON_RPC_QUANTITY_PREFIX = "0x";

/// Parses an Ethereum JSON-RPC quantity into a block number.
///
/// The parser requires the canonical `0x` prefix, a non-empty hex body, full
/// consumption, and no overflow. Malformed provider fields return
/// `std::nullopt` so callers can fail closed without throwing through the scan
/// cycle.
inline std::optional<uint64_t> eth_parse_block_quantity(std::string_view block_quantity) {
   if (block_quantity.size() <= ETH_JSON_RPC_QUANTITY_PREFIX.size()) return std::nullopt;
   if (block_quantity[0] != '0' || (block_quantity[1] != 'x' && block_quantity[1] != 'X')) {
      return std::nullopt;
   }

   block_quantity.remove_prefix(ETH_JSON_RPC_QUANTITY_PREFIX.size());
   uint64_t out = 0;
   const char* const begin = block_quantity.data();
   const char* const end   = begin + block_quantity.size();
   const auto [ptr, ec] = std::from_chars(begin, end, out, 16);
   if (ec != std::errc{} || ptr != end) return std::nullopt;
   return out;
}

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

/// SOL: maximum page size accepted by Solana JSON-RPC
/// `getSignaturesForAddress`. The underwriter walks one page per retry
/// callback and persists the `before` cursor between outer scan cycles so
/// unrelated newer program traffic cannot keep an older valid source deposit
/// outside the verifier's search window forever.
inline constexpr size_t SOL_SIGNATURE_SCAN_PAGE_SIZE = 1000;

} // namespace sysio::underwriter
