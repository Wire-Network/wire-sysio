#pragma once
/**
 * @file routing_detail.hpp
 * @brief Pure, side-effect-free routing/accounting helpers for the underwriter
 *        plugin, lifted out of the `.cpp`-private `impl` so they are unit-testable
 *        without standing up a chain.
 *
 * SEC-13 / WSA-027: the v6 depot identifies every value-bearing underwrite leg by
 * its EXACT slug codes — `chain_code`, `token_code`, `reserve_code` — and an
 * underwriter's collateral is posted per exact `(chain_code, token_code)` on every
 * registered outpost. Two active chains of the same VM family (e.g. two EVM
 * outposts) therefore hold INDEPENDENT collateral and back DISTINCT legs. The
 * keys below are exact slug values (the packed `fc::slug_name` `uint64`), never the
 * coarse `ChainKind`/`TokenKind` family enums, so same-family chains never collide.
 *
 * Codes are carried here as the raw `uint64` `fc::slug_name::value` (not
 * `fc::slug_name`) to keep this header free of fc/opp dependencies — the plugin
 * converts at the boundary, and tests can construct keys from plain integers.
 */

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace sysio::underwriter_detail {

/// Identity of a collateral / credit bucket: the exact `(chain_code, token_code)`
/// slug pair. Collateral is posted per chain+token (NOT per reserve — multiple
/// reserves on one `(chain, token)` draw from the same bond), mirroring the
/// `sysio.opreg::operators.balances` row key.
struct bucket_key {
   uint64_t chain_code = 0;   ///< `fc::slug_name::value` of the leg's chain.
   uint64_t token_code = 0;   ///< `fc::slug_name::value` of the leg's token.
   friend auto operator<=>(const bucket_key&, const bucket_key&) = default;
};

/// One swap leg's bond requirement for the bucket solver. `require == 0` marks a
/// WIRE depot leg (no outpost, no bond, no bucket consulted).
struct leg_bond {
   bucket_key bucket{};
   uint64_t   require = 0;
};

/// Per-bucket spendable credit: exact `(chain_code, token_code)` → balance.
using credit_buckets = std::map<bucket_key, uint64_t>;

/// Attempt to debit a request's two legs from `remaining`. Returns true (and
/// mutates `remaining`) iff every required leg fits; leaves `remaining` untouched
/// on failure. A request with both legs requiring zero (degenerate depot/depot)
/// returns false. Same-bucket dual legs (e.g. ERC20 → native on one chain) debit
/// both from the single shared row, so a same-`(chain, token)` swap needs the sum.
inline bool try_debit_buckets(credit_buckets& remaining,
                              const leg_bond& src, const leg_bond& dst) {
   const uint64_t src_req = src.require;
   const uint64_t dst_req = dst.require;
   if (src_req == 0 && dst_req == 0) return false;

   // Same bucket on both legs: a single row must cover the combined draw.
   if (src_req > 0 && dst_req > 0 && src.bucket == dst.bucket) {
      auto it = remaining.find(src.bucket);
      // Sum the two user-controlled requirements in 128-bit width: a pair
      // whose true sum exceeds UINT64_MAX (e.g. UINT64_MAX + 1) must never
      // wrap to a small uint64 and make an impossible request look coverable.
      const __uint128_t combined = static_cast<__uint128_t>(src_req) + dst_req;
      if (it == remaining.end() || it->second < combined) return false;
      // Reached only when combined <= it->second <= UINT64_MAX, so the draw
      // fits a uint64 exactly and the debit cannot underflow.
      it->second -= static_cast<uint64_t>(combined);
      return true;
   }

   // Distinct buckets: check both fit BEFORE mutating either, so a partial
   // failure never leaves `remaining` half-debited.
   if (src_req > 0) {
      auto it = remaining.find(src.bucket);
      if (it == remaining.end() || it->second < src_req) return false;
   }
   if (dst_req > 0) {
      auto it = remaining.find(dst.bucket);
      if (it == remaining.end() || it->second < dst_req) return false;
   }
   if (src_req > 0) remaining[src.bucket] -= src_req;
   if (dst_req > 0) remaining[dst.bucket] -= dst_req;
   return true;
}

/// Local commit de-dup key — one CONFIRMED leg. Keyed by the exact v6 leg
/// identity `(uwreq_id, chain_code, token_code, reserve_code)` so two legs that
/// differ only by chain OR reserve (e.g. a same-`(chain, token)` swap with two
/// reserves) are tracked independently and never suppress each other.
struct commit_key {
   uint64_t uwreq_id     = 0;
   uint64_t chain_code   = 0;   ///< `fc::slug_name::value`
   uint64_t token_code   = 0;   ///< `fc::slug_name::value`
   uint64_t reserve_code = 0;   ///< `fc::slug_name::value`
   friend auto operator<=>(const commit_key&, const commit_key&) = default;
};

/// One outpost chain whose active registry row and configured endpoint disagree.
struct endpoint_coverage_gap {
   uint64_t           chain_code = 0;  ///< `fc::slug_name::value` of the offending chain.
   std::optional<int> registry_kind;   ///< Registry kind, or empty when inactive/unregistered.
   std::optional<int> config_kind;     ///< Configured kind, or empty when unconfigured.
};

/// Verify a one-to-one match between active registry chains and configured endpoints.
///
/// SEC-13 / WSA-027: the underwriter derives its served set from the on-chain
/// registry (`sysio.chains`) but builds its outpost_client handles only from
/// operator config. A registered chain that is unconfigured, or configured
/// under the wrong VM family, lets the scan loop select a request whose leg has
/// no (or a wrong-kind) client and land only the OTHER leg. Preflight uses this
/// to fail closed before scheduling the scan job. Kinds are compared as raw
/// integers so this header stays free of opp/fc dependencies (the plugin passes
/// `magic_enum::enum_integer(kind)` at the boundary).
inline std::optional<endpoint_coverage_gap>
find_endpoint_coverage_gap(const std::map<uint64_t, int>& registered_kinds,
                           const std::map<uint64_t, int>& configured_kinds) {
   for (const auto& [chain_code, reg_kind] : registered_kinds) {
      auto it = configured_kinds.find(chain_code);
      if (it == configured_kinds.end())
         return endpoint_coverage_gap{chain_code, reg_kind, std::nullopt};
      if (it->second != reg_kind)
         return endpoint_coverage_gap{chain_code, reg_kind, it->second};
   }
   for (const auto& [chain_code, config_kind] : configured_kinds) {
      if (!registered_kinds.contains(chain_code))
         return endpoint_coverage_gap{chain_code, std::nullopt, config_kind};
   }
   return std::nullopt;
}

// ---------------------------------------------------------------------------
//  Source-deposit hash preimage
// ---------------------------------------------------------------------------

/// The swap terms the SOURCE OUTPOST hashed when it accepted the deposit, in the
/// order it hashed them. Both outposts commit to the same seven values plus the
/// depositor: `ReserveManager.requestSwap`'s `abi.encodePacked(...)` on EVM and
/// `request_swap.rs::correlation_hash` on SVM.
struct swap_deposit_terms {
   uint64_t src_amount             = 0;
   uint64_t src_token_code         = 0;   ///< `fc::slug_name::value`
   uint64_t src_reserve_code       = 0;   ///< `fc::slug_name::value`
   uint64_t dst_chain_code         = 0;   ///< `fc::slug_name::value`
   uint64_t dst_token_code         = 0;   ///< `fc::slug_name::value`
   uint64_t dst_reserve_code       = 0;   ///< `fc::slug_name::value`
   /// The destination amount the CALLER asked for (`SwapRequest.target_amount`).
   ///
   /// This is the field the outpost hashed, and it is deliberately NOT the
   /// UWREQ's `dst_amount`: the depot overwrites that with its own AMM quote
   /// (WNS-02), a number minted AFTER the deposit was hashed and one the outpost
   /// never saw. Packing the quote here reproduces the wrong preimage for every
   /// swap whose quote differs from the target -- i.e. every swap that pays a
   /// WIRE-leg fee -- and the underwriter then silently declines to commit.
   uint64_t target_amount          = 0;
   uint32_t variance_tolerance_bps = 0;
};

/// Big-endian preimage of the source-deposit hash: `depositor || 7 x u64 || u32`.
///
/// The depositor rides in as raw bytes because it is the ONLY per-chain
/// difference -- a 20-byte EVM address or a 32-byte Ed25519 pubkey -- so both
/// verifiers share this one packing and cannot drift apart. Callers validate the
/// depositor width (and therefore the total size) for their chain.
inline std::vector<uint8_t> pack_swap_deposit_preimage(const std::vector<char>& depositor,
                                                       const swap_deposit_terms& terms) {
   std::vector<uint8_t> packed;
   packed.reserve(depositor.size() + 8 * 7 + 4);
   for (char byte : depositor) packed.push_back(static_cast<uint8_t>(byte));

   auto append_u64_be = [&](uint64_t v) {
      for (int shift = 56; shift >= 0; shift -= 8)
         packed.push_back(static_cast<uint8_t>((v >> shift) & 0xff));
   };
   auto append_u32_be = [&](uint32_t v) {
      for (int shift = 24; shift >= 0; shift -= 8)
         packed.push_back(static_cast<uint8_t>((v >> shift) & 0xff));
   };

   append_u64_be(terms.src_amount);
   append_u64_be(terms.src_token_code);
   append_u64_be(terms.src_reserve_code);
   append_u64_be(terms.dst_chain_code);
   append_u64_be(terms.dst_token_code);
   append_u64_be(terms.dst_reserve_code);
   append_u64_be(terms.target_amount);
   append_u32_be(terms.variance_tolerance_bps);
   return packed;
}

} // namespace sysio::underwriter_detail
