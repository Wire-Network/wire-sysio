#pragma once
/**
 * @file amm_math.hpp
 * @brief Deterministic weighted constant-product (Bancor/Balancer) swap math.
 *
 * Shared by `sysio.reserv` (reserve books + `swapquote`) and `sysio.uwrit`
 * (the quote mirror) so the depot's books and its quotes ride exactly one
 * curve. The math is **integer-only and self-contained** (no contract
 * intrinsics, no floating point): it is unit-testable on the host and
 * deterministic on-chain, satisfying the consensus-determinism rules in
 * CLAUDE.md (no floating point on the consensus path).
 *
 * ## Pool model
 * Each reserve is a two-sided weighted pool of a token side
 * (`reserve_chain_amount`, weight `WEIGHT_TOTAL_BPS - cw`) and a WIRE side
 * (`reserve_wire_amount`, weight `cw`), where `cw = connector_weight_bps`.
 *
 * **Weight convention:** `connector_weight_bps` is the **WIRE-side** weight; the
 * token side gets the remainder. `cw = 5000` is the symmetric 50/50 case and
 * reduces to pure constant product (`x*y=k`), for which an EXACT integer path is
 * used (bit-identical to the legacy `cp_output`). Non-50/50 weights use the
 * weighted Balancer "out-given-in":
 *
 *     amount_out = balance_out * (1 - (balance_in/(balance_in+amount_in))^(w_in/w_out))
 *
 * ## Accuracy
 * The fractional power is evaluated in Q60 fixed point via integer `log2`/`exp2`
 * (relative error ~1e-13, far tighter than Bancor's ~1e-8 reference). The
 * subtracted term is rounded UP so `out` is floored — the reserve never
 * over-pays.
 */

#include <cstdint>

namespace sysio::opp::amm {

using u128 = unsigned __int128;

/// Sum of the two pool-side weights, in basis points.
inline constexpr uint32_t WEIGHT_TOTAL_BPS = 10000;

/// Fixed-point fractional bits for the log2/exp2 internals (Q60). 60 bits keeps
/// every intermediate product inside `u128` (`x*x` and `y*tbl` peak at ~2^122)
/// while making the worst-case quote error a handful of subunits even at extreme
/// connector weights.
inline constexpr int  FP_BITS = 60;
inline constexpr u128 FP_ONE  = (static_cast<u128>(1) << FP_BITS);

/// `2^(2^-i)` in Q60, for i = 1..60 (generated at full precision). Used by
/// `exp2_frac` to build `2^f` from the binary fraction of `f`.
inline constexpr uint64_t EXP2_FRAC_TBL[FP_BITS] = {
   0x16a09e667f3bcc91ULL, 0x1306fe0a31b7152eULL, 0x1172b83c7d517addULL, 0x10b5586cf9890f63ULL,
   0x1059b0d31585743bULL, 0x102c9a3e778060eeULL, 0x10163da9fb33356eULL, 0x100b1afa5abcbed6ULL,
   0x10058c86da1c09eaULL, 0x1002c605e2e8cec5ULL, 0x100162f3904051faULL, 0x1000b175effdc76cULL,
   0x100058ba01fb9f97ULL, 0x10002c5cc37da949ULL, 0x1000162e525ee054ULL, 0x10000b17255775c0ULL,
   0x1000058b91b5bc9bULL, 0x100002c5c89d5ec7ULL, 0x10000162e43f4f83ULL, 0x100000b1721bcfcaULL,
   0x10000058b90cf1e7ULL, 0x1000002c5c863b74ULL, 0x100000162e430e5aULL, 0x1000000b17218355ULL,
   0x100000058b90c0b5ULL, 0x10000002c5c8601dULL, 0x1000000162e42fffULL, 0x10000000b17217fcULL,
   0x1000000058b90bfdULL, 0x100000002c5c85feULL, 0x10000000162e42ffULL, 0x100000000b172180ULL,
   0x10000000058b90c0ULL, 0x1000000002c5c860ULL, 0x100000000162e430ULL, 0x1000000000b17218ULL,
   0x100000000058b90cULL, 0x10000000002c5c86ULL, 0x1000000000162e43ULL, 0x10000000000b1721ULL,
   0x1000000000058b91ULL, 0x100000000002c5c8ULL, 0x10000000000162e4ULL, 0x100000000000b172ULL,
   0x10000000000058b9ULL, 0x1000000000002c5dULL, 0x100000000000162eULL, 0x1000000000000b17ULL,
   0x100000000000058cULL, 0x10000000000002c6ULL, 0x1000000000000163ULL, 0x10000000000000b1ULL,
   0x1000000000000059ULL, 0x100000000000002cULL, 0x1000000000000016ULL, 0x100000000000000bULL,
   0x1000000000000006ULL, 0x1000000000000003ULL, 0x1000000000000001ULL, 0x1000000000000001ULL
};

/// `log2(x)` for a Q60 value `x_fp >= FP_ONE` (real `x >= 1`). Returns
/// `log2(x)` in Q60 (`>= 0`). Bit-by-bit mantissa squaring.
inline u128 log2_fp(u128 x_fp) {
   u128 result = 0;
   // Integer part: bring the mantissa into [FP_ONE, 2*FP_ONE).
   while (x_fp >= (FP_ONE << 1)) { x_fp >>= 1; result += FP_ONE; }
   // Fractional part: square repeatedly; each carry past 2 contributes a bit.
   u128 b = FP_ONE >> 1; // 0.5 in Q60
   for (int i = 0; i < FP_BITS; ++i) {
      x_fp = (x_fp * x_fp) >> FP_BITS;
      if (x_fp >= (FP_ONE << 1)) { result += b; x_fp >>= 1; }
      b >>= 1;
   }
   return result;
}

/// `2^f` for `f` in `[0, FP_ONE)` (real fraction `[0,1)`). Returns Q60 in
/// `[FP_ONE, 2*FP_ONE)`. Multiplies the precomputed `2^(2^-i)` for each set
/// fractional bit of `f`.
inline u128 exp2_frac(u128 f) {
   u128 y = FP_ONE;
   for (int i = 0; i < FP_BITS; ++i) {
      f <<= 1;
      if (f >= FP_ONE) {
         f -= FP_ONE;
         y = (y * EXP2_FRAC_TBL[i]) >> FP_BITS;
      }
   }
   return y;
}

/// `base^e` where `base = num/den` in `(0,1]` (`num <= den`, both `> 0`) and
/// `e = exp_num/exp_den > 0`. Returns the Q60 value of `base^e` in `(0, FP_ONE]`.
///
/// `base^e = 2^(-e * log2(den/num))`. Computed as: `lr = log2(den/num) >= 0`,
/// `g = e*lr >= 0`, then `2^(-g) = (1/2^gi) * (1/2^gf)`.
inline u128 pow_frac_fp(u128 num, u128 den, uint64_t exp_num, uint64_t exp_den) {
   if (exp_den == 0) return FP_ONE;
   if (num >= den)   return FP_ONE;            // base >= 1 -> 1 (only base==1 reaches here)

   const u128 x_fp = (den << FP_BITS) / num;   // den/num >= 1, Q60 (den < 2^65 -> safe)
   const u128 lr   = log2_fp(x_fp);            // log2(den/num) >= 0, Q60
   const u128 g    = (lr * exp_num) / exp_den; // e * log2(den/num), Q60

   const u128 gi = g >> FP_BITS;               // integer part of g
   if (gi >= static_cast<u128>(FP_BITS) + 2) return 0; // 2^(-g) below 1 ULP -> 0
   const u128 gf  = g - (gi << FP_BITS);       // fractional part [0, FP_ONE)
   const u128 e2  = exp2_frac(gf);             // 2^gf in [FP_ONE, 2*FP_ONE)
   const u128 inv = (FP_ONE * FP_ONE) / e2;    // 2^(-gf) in (FP_ONE/2, FP_ONE], Q60
   return inv >> gi;                           // * 2^(-gi)
}

/// Weighted constant-product output. `balance_in`/`balance_out` are the pool
/// side balances, `weight_in`/`weight_out` their bps weights, `amount_in` the
/// input. Returns `floor(amount_out)`, never exceeding `balance_out`; returns 0
/// on any degenerate input. Equal weights take the exact integer constant-product
/// path (no fixed-point error).
inline uint64_t out_given_in(uint64_t balance_in,  uint64_t weight_in,
                             uint64_t balance_out, uint64_t weight_out,
                             uint64_t amount_in) {
   if (balance_in == 0 || balance_out == 0 || amount_in == 0) return 0;
   if (weight_in == 0 || weight_out == 0) return 0;

   if (weight_in == weight_out) {
      // Pure constant product: out = balance_out * amount_in / (balance_in + amount_in).
      const u128 num = static_cast<u128>(balance_out) * amount_in;
      const u128 den = static_cast<u128>(balance_in) + amount_in;
      const u128 out = num / den;
      return out >= balance_out ? balance_out : static_cast<uint64_t>(out);
   }

   const u128 num  = balance_in;
   const u128 den  = static_cast<u128>(balance_in) + amount_in;
   const u128 bpow = pow_frac_fp(num, den, weight_in, weight_out); // base^(w_in/w_out), Q60
   // out = balance_out * (1 - base^e). Round the subtracted term UP so out is
   // floored: the reserve never over-pays on rounding.
   const u128 term = (static_cast<u128>(balance_out) * bpow + (FP_ONE - 1)) >> FP_BITS;
   if (term >= balance_out) return 0;
   return static_cast<uint64_t>(balance_out - term);
}

/// Convenience: quote `amount_token` of the reserve's token side INTO WIRE.
/// `cw = connector_weight_bps` (the WIRE-side weight).
inline uint64_t token_to_wire(uint64_t reserve_chain_amount,
                              uint64_t reserve_wire_amount,
                              uint32_t cw,
                              uint64_t amount_token) {
   return out_given_in(reserve_chain_amount, WEIGHT_TOTAL_BPS - cw,
                       reserve_wire_amount,  cw,
                       amount_token);
}

/// Convenience: quote `amount_wire` of the reserve's WIRE side INTO the token.
/// `cw = connector_weight_bps` (the WIRE-side weight).
inline uint64_t wire_to_token(uint64_t reserve_wire_amount,
                              uint64_t reserve_chain_amount,
                              uint32_t cw,
                              uint64_t amount_wire) {
   return out_given_in(reserve_wire_amount,  cw,
                       reserve_chain_amount, WEIGHT_TOTAL_BPS - cw,
                       amount_wire);
}

/// Basis-points denominator (10000 = 100%).
inline constexpr uint32_t BPS_TOTAL = 10000;

/// Decomposition of EVERY fee taken out of a swap's WIRE leg.
///
/// Two independent fees ride the same leg (WIRE-281): the NETWORK fee
/// (`sysio.uwrit::fee_bps`, split between the winning underwriter, batch
/// operators and optionally the emissions treasury) and each participating
/// RESERVE's own owner fee. A chain-to-chain swap therefore pays three fees —
/// two reserve owners plus the network — while a WIRE-endpoint swap touches one
/// reserve and pays two.
struct wire_fee {
   uint64_t fee               = 0; ///< TOTAL of every fee charged on the leg
   uint64_t underwriter_share = 0; ///< network fee: the swap's winning underwriter
   uint64_t reward_share      = 0; ///< network fee: rewards bucket (batch operators)
   uint64_t emissions_share   = 0; ///< network fee: the `sysio` emissions treasury
   uint64_t src_reserve_share = 0; ///< the SOURCE reserve owner's fee
   uint64_t dst_reserve_share = 0; ///< the DESTINATION reserve owner's fee
   uint64_t net               = 0; ///< wire_amount - fee (continues through the swap)
};

/// The ONE fee decomposition for a swap's WIRE leg — used by BOTH the read-only
/// quote and settlement, so the two can never drift by a rounding subunit.
///
/// Every rate is applied to the SAME gross `wire_amount`, so the fees are purely
/// additive and order-independent — a user pays `fee_bps + src + dst` of the
/// leg, and no party's cut depends on who is computed first:
///
///   * **Reserve fees** — `src_reserve_fee_bps` / `dst_reserve_fee_bps`, each
///     the owner fee of the reserve supplying that leg (0 for a WIRE endpoint,
///     which has no reserve). Accrue to those reserves' owners.
///   * **Network fee** — `fee_bps`, itself split in two stages:
///     1. `underwriter_share_bps` of it goes to the winning underwriter; the
///        remainder is the **rewards pool**.
///     2. `emissions_share_bps` of that POOL (not of the whole fee) goes to the
///        `sysio` emissions treasury; the rest is the batch-operator share.
///
/// All integer, exact: the five shares sum to `fee` and `net + fee ==
/// wire_amount`, because every stage takes a REMAINDER rather than a second
/// floored product. Computed in `u128` to avoid overflow.
///
/// **Callers must check `net > 0`.** With enough stacked rates the total can
/// reach or exceed the leg. When it does, `fee` is CLAMPED to `wire_amount` and
/// `net` is 0 — so in THAT case alone the per-share fields sum to more than
/// `fee`, which is harmless because the caller rejects the swap and no share is
/// ever accrued. The settlement paths already assert this, and
/// `sysio.reserv::setrsvfee` / `sysio.uwrit::setconfig` cap each rate so a
/// realistic combination cannot get there.
///
/// Paths with no winning underwriter (a revert refund) pass 0 for
/// `underwriter_share_bps`, sending the whole network fee into the rewards pool.
/// The trailing rates default to 0, so a caller that charges only the network
/// fee — a revert, or a quote against a fee-free reserve — omits them.
inline wire_fee split_wire_fee(uint64_t wire_amount,
                               uint32_t fee_bps,
                               uint32_t underwriter_share_bps,
                               uint32_t emissions_share_bps  = 0,
                               uint32_t src_reserve_fee_bps  = 0,
                               uint32_t dst_reserve_fee_bps  = 0) {
   wire_fee r;
   if (fee_bps > BPS_TOTAL) fee_bps = BPS_TOTAL;
   if (underwriter_share_bps > BPS_TOTAL) underwriter_share_bps = BPS_TOTAL;
   if (emissions_share_bps > BPS_TOTAL) emissions_share_bps = BPS_TOTAL;
   if (src_reserve_fee_bps > BPS_TOTAL) src_reserve_fee_bps = BPS_TOTAL;
   if (dst_reserve_fee_bps > BPS_TOTAL) dst_reserve_fee_bps = BPS_TOTAL;

   // Each reserve owner's cut, off the gross leg.
   r.src_reserve_share = static_cast<uint64_t>((static_cast<u128>(wire_amount) * src_reserve_fee_bps) / BPS_TOTAL);
   r.dst_reserve_share = static_cast<uint64_t>((static_cast<u128>(wire_amount) * dst_reserve_fee_bps) / BPS_TOTAL);

   // The network fee, off the same gross leg, then its own two-stage split.
   const uint64_t network_fee = static_cast<uint64_t>((static_cast<u128>(wire_amount) * fee_bps) / BPS_TOTAL);
   r.underwriter_share = static_cast<uint64_t>((static_cast<u128>(network_fee) * underwriter_share_bps) / BPS_TOTAL);
   const uint64_t rewards_pool = network_fee - r.underwriter_share;
   r.emissions_share   = static_cast<uint64_t>((static_cast<u128>(rewards_pool) * emissions_share_bps) / BPS_TOTAL);
   r.reward_share      = rewards_pool - r.emissions_share;

   // Sum in u128. Three stacked rates carry past uint64 on an extreme leg, and a
   // WRAPPED total is worse than a saturated one: it reports a small positive
   // `net` for a swap that consumed the whole leg, so the caller's `net > 0`
   // gate waves it through. Compare in u128 and narrow only when the total
   // genuinely fits; otherwise clamp to the leg so a fully-consumed leg is
   // reported as exactly that.
   const u128 total_fee = static_cast<u128>(network_fee)
                        + static_cast<u128>(r.src_reserve_share)
                        + static_cast<u128>(r.dst_reserve_share);
   if (total_fee >= static_cast<u128>(wire_amount)) {
      r.fee = wire_amount;
      r.net = 0;
   } else {
      r.fee = static_cast<uint64_t>(total_fee);
      r.net = wire_amount - r.fee;
   }
   return r;
}

/// Post-fee swap quote along the depot curve. A WIRE endpoint (`src_is_wire` /
/// `dst_is_wire`) skips that side's reserve — the depot IS the WIRE side. For a
/// non-WIRE side pass that reserve's `(chain_amount, wire_amount, cw)` AND its
/// `owner_fee_bps`; a WIRE endpoint has no reserve, so pass 0 for its fee.
///
/// Every fee — the network `fee_bps` plus each participating reserve's owner fee
/// — is charged on the WIRE leg through the SAME `split_wire_fee` the settlement
/// paths use, so quotes and books agree by construction. Returns the post-fee
/// output the recipient could receive, or 0 on degenerate input (including a
/// stacked-fee combination that leaves nothing).
inline uint64_t quote_swap(bool src_is_wire, uint64_t src_chain, uint64_t src_wire, uint32_t src_cw,
                           bool dst_is_wire, uint64_t dst_chain, uint64_t dst_wire, uint32_t dst_cw,
                           uint64_t amount_in, uint32_t fee_bps,
                           uint32_t src_reserve_fee_bps = 0, uint32_t dst_reserve_fee_bps = 0) {
   if (amount_in == 0) return 0;
   if (src_is_wire && dst_is_wire) return amount_in; // WIRE->WIRE is a plain transfer

   // WIRE produced at / provided to the WIRE leg.
   uint64_t wire_leg = src_is_wire ? amount_in
                                   : token_to_wire(src_chain, src_wire, src_cw, amount_in);
   if (wire_leg == 0) return 0;

   // A WIRE endpoint has no reserve on that side and therefore charges no
   // reserve fee, whatever the caller passed.
   const uint64_t wire_net = split_wire_fee(wire_leg, fee_bps, /*underwriter_share_bps*/0,
                                            /*emissions_share_bps*/0,
                                            src_is_wire ? 0 : src_reserve_fee_bps,
                                            dst_is_wire ? 0 : dst_reserve_fee_bps).net;
   if (wire_net == 0) return 0;
   if (dst_is_wire) return wire_net; // user receives WIRE directly
   return wire_to_token(dst_wire, dst_chain, dst_cw, wire_net);
}

} // namespace sysio::opp::amm
