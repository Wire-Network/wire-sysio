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
      if (it == remaining.end() || it->second < src_req + dst_req) return false;
      it->second -= (src_req + dst_req);
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

} // namespace sysio::underwriter_detail
