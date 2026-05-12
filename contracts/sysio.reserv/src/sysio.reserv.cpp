#include <sysio.reserv/sysio.reserv.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

namespace sysio {

using opp::types::ChainKind;
using opp::types::TokenKind;
using opp::types::TokenAmount;

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

/// Build a TokenAmount with `kind` and `amount`. amount is int64 on the
/// wire; the upstream callers carry uint64 quantities so the cast is
/// explicit.
TokenAmount make_token_amount(TokenKind kind, uint64_t amount) {
   TokenAmount ta;
   ta.kind   = kind;
   ta.amount = static_cast<int64_t>(amount);
   return ta;
}

/// Pull the unsigned magnitude out of a TokenAmount. Negative on-wire
/// amounts (which are not valid in reserve accounting) saturate at 0.
uint64_t to_unsigned(int64_t amount) {
   return amount < 0 ? 0 : static_cast<uint64_t>(amount);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setreserve
// ---------------------------------------------------------------------------
void reserve::setreserve(opp::types::ChainKind     chain,
                          opp::types::TokenAmount   outpost_amount,
                          opp::types::TokenAmount   wire_amount,
                          uint32_t                  connector_weight_bps) {
   require_auth(get_self());
   check(connector_weight_bps > 0 && connector_weight_bps <= MAX_CONNECTOR_WEIGHT_BPS,
         "connector_weight_bps must be in (0, 10000]");
   check(wire_amount.kind == TokenKind::TOKEN_KIND_WIRE,
         "wire_amount.kind must be TOKEN_KIND_WIRE");
   check(outpost_amount.kind != TokenKind::TOKEN_KIND_WIRE,
         "outpost_amount.kind must not be TOKEN_KIND_WIRE (the WIRE side is implicit)");
   // The pure WIRE/WIRE LP is not a thing — the previous check already
   // forbids that, but keep an explicit guard for the chain-side too.
   check(!(chain == ChainKind::CHAIN_KIND_WIRE),
         "WIRE chain has no outpost reserve; reserves are per-outpost only");

   reserves_t reserves(get_self());
   auto pk  = reserve_key{pack_chain_token(chain, outpost_amount.kind)};
   auto now = current_time_ms();
   if (reserves.contains(pk)) {
      reserves.modify(same_payer, pk, [&](auto& r) {
         r.reserve_outpost_amount = outpost_amount;
         r.reserve_wire_amount    = wire_amount;
         r.connector_weight_bps   = connector_weight_bps;
         r.last_updated_ms        = now;
      });
   } else {
      reserves.emplace(get_self(), pk, reserve_entry{
         .chain                  = chain,
         .reserve_outpost_amount = outpost_amount,
         .reserve_wire_amount    = wire_amount,
         .connector_weight_bps   = connector_weight_bps,
         .last_updated_ms        = now,
      });
   }
}

// ---------------------------------------------------------------------------
//  swapquote — read-only constant-product quote
// ---------------------------------------------------------------------------
opp::types::TokenAmount reserve::swapquote(opp::types::TokenAmount   from_amount,
                                            opp::types::ChainKind     to_chain,
                                            opp::types::TokenKind     to_token) {
   if (from_amount.amount <= 0) {
      return make_token_amount(to_token, 0);
   }
   const uint64_t  src_amount = to_unsigned(from_amount.amount);
   const TokenKind src_token  = from_amount.kind;
   // src chain isn't on the input — the (chain, kind) packed key uses the
   // outpost-side TokenKind only; the chain comes from `to_chain` for the
   // dst hop and is implicit on the src hop because every cross-chain
   // src token is uniquely owned by exactly one outpost reserve.
   //
   // For the src hop we look the reserve up by walking the table. To
   // avoid a scan, callers that know the src chain can use the
   // `swapquote_explicit` overload (not implemented yet); for the
   // common case of contract callers (uwrit's variance check) the
   // src chain is known and is passed as the `from_amount`'s chain
   // through a new field. v1: assume the caller passes a chain hint
   // by using the same chain on both sides when src and dst differ
   // — we relax this when the actual variance flow lands.
   //
   // To stay backward-compatible with uwrit's variance check while the
   // signature is in flux, treat `to_chain` as the hint for both the
   // src and dst lookups when src_token != WIRE && dst_token != WIRE.
   // For half-hops (one side is WIRE), only one reserve is consulted
   // and the chain is unambiguous.

   reserves_t reserves(get_self());

   // Trivial case: src token already IS WIRE, dst token also WIRE.
   if (src_token == TokenKind::TOKEN_KIND_WIRE &&
       to_token == TokenKind::TOKEN_KIND_WIRE) {
      return make_token_amount(to_token, src_amount);
   }

   // Half-hop: src is WIRE — quote WIRE -> outpost token on dst chain.
   if (src_token == TokenKind::TOKEN_KIND_WIRE) {
      auto pk = reserve_key{pack_chain_token(to_chain, to_token)};
      if (!reserves.contains(pk)) return make_token_amount(to_token, 0);
      auto r = reserves.get(pk);
      uint64_t out = cp_output(to_unsigned(r.reserve_wire_amount.amount),
                               to_unsigned(r.reserve_outpost_amount.amount),
                               src_amount);
      return make_token_amount(to_token, out);
   }

   // Half-hop: dst is WIRE — quote outpost token on src chain -> WIRE.
   if (to_token == TokenKind::TOKEN_KIND_WIRE) {
      auto pk = reserve_key{pack_chain_token(to_chain, src_token)};
      if (!reserves.contains(pk)) return make_token_amount(to_token, 0);
      auto r = reserves.get(pk);
      uint64_t out = cp_output(to_unsigned(r.reserve_outpost_amount.amount),
                               to_unsigned(r.reserve_wire_amount.amount),
                               src_amount);
      return make_token_amount(to_token, out);
   }

   // Full hop: src token -> WIRE -> dst token. Two reserves consulted.
   auto src_pk = reserve_key{pack_chain_token(to_chain, src_token)};
   auto dst_pk = reserve_key{pack_chain_token(to_chain, to_token)};
   if (!reserves.contains(src_pk) || !reserves.contains(dst_pk)) {
      return make_token_amount(to_token, 0);
   }
   auto src_r = reserves.get(src_pk);
   auto dst_r = reserves.get(dst_pk);
   uint64_t wire_intermediate = cp_output(to_unsigned(src_r.reserve_outpost_amount.amount),
                                          to_unsigned(src_r.reserve_wire_amount.amount),
                                          src_amount);
   if (wire_intermediate == 0) return make_token_amount(to_token, 0);
   uint64_t out = cp_output(to_unsigned(dst_r.reserve_wire_amount.amount),
                            to_unsigned(dst_r.reserve_outpost_amount.amount),
                            wire_intermediate);
   return make_token_amount(to_token, out);
}

// ---------------------------------------------------------------------------
//  onreward — STAKING_REWARD attestation credits the outpost-side reserve
// ---------------------------------------------------------------------------
void reserve::onreward(opp::types::ChainKind     chain,
                        opp::types::TokenAmount   outpost_amount) {
   require_auth(MSGCH_ACCOUNT);
   check(outpost_amount.amount > 0, "outpost_amount must be positive");
   check(outpost_amount.kind != TokenKind::TOKEN_KIND_WIRE,
         "STAKING_REWARD credits the outpost-side reserve only; WIRE-side payout is a separate action");

   reserves_t reserves(get_self());
   auto pk = reserve_key{pack_chain_token(chain, outpost_amount.kind)};
   check(reserves.contains(pk),
         "reserve not provisioned for this (chain, outpost_token); call setreserve first");

   auto now = current_time_ms();
   reserves.modify(same_payer, pk, [&](auto& r) {
      check(r.reserve_outpost_amount.kind == outpost_amount.kind,
            "outpost_amount.kind mismatches reserve_outpost_amount.kind");
      r.reserve_outpost_amount.amount += outpost_amount.amount;
      r.last_updated_ms = now;
   });
}

// ---------------------------------------------------------------------------
//  onreject — outpost couldn't pay SwapRemit; depot's reserve view re-adds
//             the unremitted amount so accounting reconciles
// ---------------------------------------------------------------------------
void reserve::onreject(opp::attestations::SwapRejected rejected) {
   require_auth(MSGCH_ACCOUNT);
   const auto& unremitted = rejected.unremitted_amount;
   check(unremitted.amount > 0, "unremitted_amount must be positive");
   check(unremitted.kind != TokenKind::TOKEN_KIND_WIRE,
         "SwapRejected reconciles the outpost-side reserve; WIRE-side has no outpost balance");

   // The recipient's chain identifies which outpost reserve the failed
   // SwapRemit was drawn from.
   const ChainKind chain = rejected.recipient.kind;

   reserves_t reserves(get_self());
   auto pk = reserve_key{pack_chain_token(chain, unremitted.kind)};
   check(reserves.contains(pk),
         "reserve not provisioned for this (chain, outpost_token); cannot reconcile SwapRejected");

   auto now = current_time_ms();
   reserves.modify(same_payer, pk, [&](auto& r) {
      check(r.reserve_outpost_amount.kind == unremitted.kind,
            "unremitted_amount.kind mismatches reserve_outpost_amount.kind");
      r.reserve_outpost_amount.amount += unremitted.amount;
      r.last_updated_ms = now;
   });
}

} // namespace sysio
