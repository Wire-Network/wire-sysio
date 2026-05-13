#include <sysio.reserv/sysio.reserv.hpp>

namespace sysio {

using opp::types::ChainKind;
using opp::types::TokenKind;

namespace {

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

/// Constant-product output from a single hop:
///   dst_amount = (reserve_dst * src_amount) / (reserve_src + src_amount)
///
/// Computed in uint128 to avoid overflow on uint64 reserves * uint64 amounts
/// (max product is 2^128, exactly representable). Returns 0 if either reserve
/// is empty or src_amount is zero.
uint64_t cp_output(uint64_t reserve_src, uint64_t reserve_dst, uint64_t src_amount) {
   if (reserve_src == 0 || reserve_dst == 0 || src_amount == 0) return 0;
   uint128_t numerator   = static_cast<uint128_t>(reserve_dst) * src_amount;
   uint128_t denominator = static_cast<uint128_t>(reserve_src) + src_amount;
   uint128_t result      = numerator / denominator;
   // Saturate at uint64 max — practically unreachable for sane reserves but
   // protects against absurd inputs.
   if (result > static_cast<uint128_t>(std::numeric_limits<uint64_t>::max())) {
      return std::numeric_limits<uint64_t>::max();
   }
   return static_cast<uint64_t>(result);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setlp
// ---------------------------------------------------------------------------
void reserve::setlp(opp::types::ChainKind chain,
                    opp::types::TokenKind paired_token,
                    uint64_t reserve_paired,
                    uint64_t reserve_wire,
                    uint32_t connector_weight_bps) {
   require_auth(get_self());
   check(connector_weight_bps > 0 && connector_weight_bps <= MAX_CONNECTOR_WEIGHT_BPS,
         "connector_weight_bps must be in (0, 10000]");
   // The pure WIRE/WIRE LP is not a thing — every LP is paired (paired_token
   // is the chain's native or wrapped token; the WIRE side is implicit).
   check(!(chain == ChainKind::CHAIN_KIND_WIRE && paired_token == TokenKind::TOKEN_KIND_WIRE),
         "WIRE/WIRE LP is degenerate; nothing to provision");

   lps_t lps(get_self());
   auto pk = lp_key{pack_chain_token(chain, paired_token)};
   auto now = current_time_ms();
   if (lps.contains(pk)) {
      lps.modify(same_payer, pk, [&](auto& l) {
         l.reserve_paired       = reserve_paired;
         l.reserve_wire         = reserve_wire;
         l.connector_weight_bps = connector_weight_bps;
         l.last_updated_ms      = now;
      });
   } else {
      lps.emplace(get_self(), pk, lp_entry{
         .chain                = chain,
         .paired_token         = paired_token,
         .reserve_paired       = reserve_paired,
         .reserve_wire         = reserve_wire,
         .connector_weight_bps = connector_weight_bps,
         .last_updated_ms      = now,
      });
   }
}

// ---------------------------------------------------------------------------
//  quote — read-only constant-product quote
// ---------------------------------------------------------------------------
uint64_t reserve::quote(opp::types::ChainKind src_chain,
                        opp::types::TokenKind src_token,
                        opp::types::ChainKind dst_chain,
                        opp::types::TokenKind dst_token,
                        uint64_t src_amount) {
   if (src_amount == 0) return 0;

   lps_t lps(get_self());

   // Trivial case: src token already IS WIRE, dst token also WIRE.
   if (src_token == TokenKind::TOKEN_KIND_WIRE &&
       dst_token == TokenKind::TOKEN_KIND_WIRE) {
      return src_amount;
   }

   // Half-hop: src is WIRE — quote WIRE -> paired_token on dst chain.
   if (src_token == TokenKind::TOKEN_KIND_WIRE) {
      auto pk = lp_key{pack_chain_token(dst_chain, dst_token)};
      if (!lps.contains(pk)) return 0;
      auto lp = lps.get(pk);
      return cp_output(lp.reserve_wire, lp.reserve_paired, src_amount);
   }

   // Half-hop: dst is WIRE — quote paired_token on src chain -> WIRE.
   if (dst_token == TokenKind::TOKEN_KIND_WIRE) {
      auto pk = lp_key{pack_chain_token(src_chain, src_token)};
      if (!lps.contains(pk)) return 0;
      auto lp = lps.get(pk);
      return cp_output(lp.reserve_paired, lp.reserve_wire, src_amount);
   }

   // Full hop: src token -> WIRE -> dst token. Two LPs consulted.
   auto src_pk = lp_key{pack_chain_token(src_chain, src_token)};
   auto dst_pk = lp_key{pack_chain_token(dst_chain, dst_token)};
   if (!lps.contains(src_pk) || !lps.contains(dst_pk)) return 0;

   auto src_lp = lps.get(src_pk);
   auto dst_lp = lps.get(dst_pk);
   uint64_t wire_intermediate = cp_output(src_lp.reserve_paired, src_lp.reserve_wire, src_amount);
   if (wire_intermediate == 0) return 0;
   return cp_output(dst_lp.reserve_wire, dst_lp.reserve_paired, wire_intermediate);
}

// ---------------------------------------------------------------------------
//  creditlp — grow an LP's reserves from yield / staking-reward attestations
// ---------------------------------------------------------------------------
void reserve::creditlp(opp::types::ChainKind chain,
                       opp::types::TokenKind paired_token,
                       uint64_t paired_amount,
                       uint64_t wire_amount) {
   require_auth(MSGCH_ACCOUNT);

   lps_t lps(get_self());
   auto pk = lp_key{pack_chain_token(chain, paired_token)};
   check(lps.contains(pk), "LP not provisioned for this (chain, paired_token)");

   auto now = current_time_ms();
   lps.modify(same_payer, pk, [&](auto& l) {
      l.reserve_paired += paired_amount;
      l.reserve_wire   += wire_amount;
      l.last_updated_ms = now;
   });
}

} // namespace sysio
