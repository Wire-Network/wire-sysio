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

/// Build a TokenAmount with `kind` and `amount`. amount is int64 on the
/// wire; the upstream callers carry uint64 quantities so the cast is
/// explicit.
TokenAmount make_token_amount(TokenKind kind, uint64_t amount) {
   TokenAmount ta;
   ta.kind   = kind;
   ta.amount = static_cast<int64_t>(amount);
   return ta;
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
   // Pricing lives in the shared header helper so `sysio.cap` (which cannot
   // call this read-only action — sysio/sysio has no synchronous
   // inter-contract call) prices native staking rewards with the exact same
   // math against this contract's published `reserves` table.
   reserves_t reserves(get_self());
   return quote(reserves, from_kind, from_amount, to_chain, to_token);
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
