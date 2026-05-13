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
                          opp::types::TokenKind     outpost_kind,
                          uint64_t                  outpost_amount,
                          uint64_t                  wire_amount,
                          uint32_t                  connector_weight_bps) {
   require_auth(get_self());
   check(connector_weight_bps > 0 && connector_weight_bps <= MAX_CONNECTOR_WEIGHT_BPS,
         "connector_weight_bps must be in (0, 10000]");
   check(outpost_kind != TokenKind::TOKEN_KIND_WIRE,
         "outpost_kind must not be TOKEN_KIND_WIRE (the WIRE side is implicit)");
   check(!(chain == ChainKind::CHAIN_KIND_WIRE),
         "WIRE chain has no outpost reserve; reserves are per-outpost only");

   reserves_t reserves(get_self());
   auto pk  = reserve_key{pack_chain_token(chain, outpost_kind)};
   auto now = current_time_ms();
   if (reserves.contains(pk)) {
      reserves.modify(same_payer, pk, [&](auto& r) {
         r.reserve_outpost_amount = make_token_amount(outpost_kind, outpost_amount);
         r.reserve_wire_amount    = make_token_amount(TokenKind::TOKEN_KIND_WIRE, wire_amount);
         r.connector_weight_bps   = connector_weight_bps;
         r.last_updated_ms        = now;
      });
   } else {
      reserves.emplace(get_self(), pk, reserve_entry{
         .chain                  = chain,
         .reserve_outpost_amount = make_token_amount(outpost_kind, outpost_amount),
         .reserve_wire_amount    = make_token_amount(TokenKind::TOKEN_KIND_WIRE, wire_amount),
         .connector_weight_bps   = connector_weight_bps,
         .last_updated_ms        = now,
      });
   }
}

// ---------------------------------------------------------------------------
//  swapquote — read-only constant-product quote
// ---------------------------------------------------------------------------
uint64_t reserve::swapquote(opp::types::TokenKind     from_kind,
                             uint64_t                  from_amount,
                             opp::types::ChainKind     to_chain,
                             opp::types::TokenKind     to_token) {
   if (from_amount == 0) return 0;

   reserves_t reserves(get_self());

   // Trivial case: src token already IS WIRE, dst token also WIRE.
   if (from_kind == TokenKind::TOKEN_KIND_WIRE &&
       to_token == TokenKind::TOKEN_KIND_WIRE) {
      return from_amount;
   }

   // Half-hop: src is WIRE — quote WIRE -> outpost token on dst chain.
   if (from_kind == TokenKind::TOKEN_KIND_WIRE) {
      auto pk = reserve_key{pack_chain_token(to_chain, to_token)};
      if (!reserves.contains(pk)) return 0;
      auto r = reserves.get(pk);
      return cp_output(to_unsigned(r.reserve_wire_amount.amount),
                       to_unsigned(r.reserve_outpost_amount.amount),
                       from_amount);
   }

   // Half-hop: dst is WIRE — quote outpost token on src chain -> WIRE.
   if (to_token == TokenKind::TOKEN_KIND_WIRE) {
      auto pk = reserve_key{pack_chain_token(to_chain, from_kind)};
      if (!reserves.contains(pk)) return 0;
      auto r = reserves.get(pk);
      return cp_output(to_unsigned(r.reserve_outpost_amount.amount),
                       to_unsigned(r.reserve_wire_amount.amount),
                       from_amount);
   }

   // Full hop: src token -> WIRE -> dst token. Two reserves consulted.
   // `to_chain` doubles as the hint for the src reserve lookup since every
   // outpost-token uniquely lives on exactly one outpost.
   auto src_pk = reserve_key{pack_chain_token(to_chain, from_kind)};
   auto dst_pk = reserve_key{pack_chain_token(to_chain, to_token)};
   if (!reserves.contains(src_pk) || !reserves.contains(dst_pk)) return 0;
   auto src_r = reserves.get(src_pk);
   auto dst_r = reserves.get(dst_pk);
   uint64_t wire_intermediate = cp_output(to_unsigned(src_r.reserve_outpost_amount.amount),
                                          to_unsigned(src_r.reserve_wire_amount.amount),
                                          from_amount);
   if (wire_intermediate == 0) return 0;
   return cp_output(to_unsigned(dst_r.reserve_wire_amount.amount),
                    to_unsigned(dst_r.reserve_outpost_amount.amount),
                    wire_intermediate);
}

// ---------------------------------------------------------------------------
//  onreward — STAKING_REWARD attestation credits the outpost-side reserve
// ---------------------------------------------------------------------------
void reserve::onreward(opp::types::ChainKind     chain,
                        opp::types::TokenKind     outpost_kind,
                        uint64_t                  outpost_amount) {
   require_auth(MSGCH_ACCOUNT);
   check(outpost_amount > 0, "outpost_amount must be positive");
   check(outpost_kind != TokenKind::TOKEN_KIND_WIRE,
         "STAKING_REWARD credits the outpost-side reserve only; WIRE-side payout is a separate action");

   reserves_t reserves(get_self());
   auto pk = reserve_key{pack_chain_token(chain, outpost_kind)};
   check(reserves.contains(pk),
         "reserve not provisioned for this (chain, outpost_token); call setreserve first");

   auto now = current_time_ms();
   reserves.modify(same_payer, pk, [&](auto& r) {
      check(r.reserve_outpost_amount.kind == outpost_kind,
            "outpost_kind mismatches reserve_outpost_amount.kind");
      r.reserve_outpost_amount.amount += static_cast<int64_t>(outpost_amount);
      r.last_updated_ms = now;
   });
}

// ---------------------------------------------------------------------------
//  debit — SWAP_REMIT emit-time debit (auth=sysio.uwrit)
// ---------------------------------------------------------------------------
void reserve::debit(opp::types::ChainKind     chain,
                     opp::types::TokenKind     outpost_kind,
                     uint64_t                  outpost_amount) {
   require_auth(UWRIT_ACCOUNT);
   check(outpost_amount > 0, "outpost_amount must be positive");
   check(outpost_kind != TokenKind::TOKEN_KIND_WIRE,
         "debit targets the outpost-side reserve only; WIRE-side debits "
         "are owned by the staker-payout path");

   reserves_t reserves(get_self());
   auto pk = reserve_key{pack_chain_token(chain, outpost_kind)};
   check(reserves.contains(pk),
         "reserve not provisioned for this (chain, outpost_token); "
         "cannot debit");

   auto now = current_time_ms();
   reserves.modify(same_payer, pk, [&](auto& r) {
      check(r.reserve_outpost_amount.kind == outpost_kind,
            "outpost_kind mismatches reserve_outpost_amount.kind");
      check(to_unsigned(r.reserve_outpost_amount.amount) >= outpost_amount,
            "insufficient reserve_outpost_amount for SWAP_REMIT debit");
      r.reserve_outpost_amount.amount -= static_cast<int64_t>(outpost_amount);
      r.last_updated_ms = now;
   });
}

// ---------------------------------------------------------------------------
//  onreject — outpost couldn't pay SwapRemit; depot's reserve view re-adds
//             the unremitted amount so accounting reconciles
// ---------------------------------------------------------------------------
void reserve::onreject(checksum256              /*original_swap_remit_id*/,
                        opp::types::ChainKind    recipient_kind,
                        std::vector<char>        /*recipient_address*/,
                        opp::types::TokenKind    unremitted_kind,
                        uint64_t                 unremitted_amount,
                        std::string              /*reason*/) {
   require_auth(MSGCH_ACCOUNT);
   check(unremitted_amount > 0, "unremitted_amount must be positive");
   check(unremitted_kind != TokenKind::TOKEN_KIND_WIRE,
         "SwapRejected reconciles the outpost-side reserve; WIRE-side has no outpost balance");

   // The recipient's chain identifies which outpost reserve the failed
   // SwapRemit was drawn from.
   reserves_t reserves(get_self());
   auto pk = reserve_key{pack_chain_token(recipient_kind, unremitted_kind)};
   check(reserves.contains(pk),
         "reserve not provisioned for this (chain, outpost_token); cannot reconcile SwapRejected");

   auto now = current_time_ms();
   reserves.modify(same_payer, pk, [&](auto& r) {
      check(r.reserve_outpost_amount.kind == unremitted_kind,
            "unremitted_kind mismatches reserve_outpost_amount.kind");
      r.reserve_outpost_amount.amount += static_cast<int64_t>(unremitted_amount);
      r.last_updated_ms = now;
   });
}

} // namespace sysio
