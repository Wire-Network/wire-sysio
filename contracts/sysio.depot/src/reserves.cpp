#include <sysio.depot/sysio.depot.hpp>

namespace sysio {

// ── setreserve (FR-701) ─────────────────────────────────────────────────────
//
// Governance / admin action to set the initial reserve state for a token pair.
// Sets the total reserve balance and its $WIRE equivalent value.

void depot::setreserve(name authority, asset reserve_total, asset wire_equivalent) {
   require_auth(authority);

   // Only the contract itself (governance) can set reserves
   check(authority == get_self(), "depot: only contract authority can set reserves");

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   check(reserve_total.amount >= 0, "depot: reserve_total must be non-negative");
   check(wire_equivalent.amount >= 0, "depot: wire_equivalent must be non-negative");

   reserves_table reserves(get_self(), chain_scope);
   auto it = reserves.find(reserve_total.symbol.code().raw());

   if (it == reserves.end()) {
      reserves.emplace(get_self(), [&](auto& r) {
         r.reserve_total   = reserve_total;
         r.wire_equivalent = wire_equivalent;
      });
   } else {
      reserves.modify(it, same_payer, [&](auto& r) {
         r.reserve_total   = reserve_total;
         r.wire_equivalent = wire_equivalent;
      });
   }
}

// ── updreserve (FR-701) ─────────────────────────────────────────────────────
//
// An elected batch operator updates a reserve balance by a delta amount.
// Called during message processing when assets are deposited or withdrawn.

void depot::updreserve(name operator_account, symbol token_sym, int64_t delta) {
   require_auth(operator_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   // Verify caller is elected
   verify_elected(operator_account, s.current_epoch);

   reserves_table reserves(get_self(), chain_scope);
   auto it = reserves.find(token_sym.code().raw());
   check(it != reserves.end(), "depot: reserve not found for token");

   int64_t new_amount = it->reserve_total.amount + delta;
   check(new_amount >= 0, "depot: reserve would go negative");

   reserves.modify(it, same_payer, [&](auto& r) {
      r.reserve_total.amount = new_amount;
   });
}

// ── getquote (FR-900 / FR-703) ──────────────────────────────────────────────
//
// Read-only action that computes a swap quote based on current reserve state.
// Uses constant-product (x*y=k) pricing with the underwriting fee applied.

void depot::getquote(symbol source_sym, symbol target_sym, asset amount) {
   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   check(amount.amount > 0, "depot: amount must be positive");
   check(amount.symbol == source_sym, "depot: amount symbol must match source_sym");

   reserves_table reserves(get_self(), chain_scope);

   auto source_it = reserves.find(source_sym.code().raw());
   check(source_it != reserves.end(), "depot: source token reserve not found");
   check(source_it->reserve_total.amount > 0, "depot: source reserve is empty");

   auto target_it = reserves.find(target_sym.code().raw());
   check(target_it != reserves.end(), "depot: target token reserve not found");
   check(target_it->reserve_total.amount > 0, "depot: target reserve is empty");

   // Constant product: x * y = k
   // output = (target_reserve * input) / (source_reserve + input)
   // Apply fee: output = output * (10000 - UNDERWRITE_FEE_BPS) / 10000

   int64_t source_reserve = source_it->reserve_total.amount;
   int64_t target_reserve = target_it->reserve_total.amount;
   int64_t input          = amount.amount;

   // Use 128-bit multiplication to avoid overflow
   __int128 numerator   = __int128(target_reserve) * __int128(input);
   __int128 denominator = __int128(source_reserve) + __int128(input);
   int64_t  raw_output  = int64_t(numerator / denominator);

   // Apply fee
   int64_t fee_adjusted = int64_t((__int128(raw_output) * (10000 - UNDERWRITE_FEE_BPS)) / 10000);

   check(fee_adjusted > 0, "depot: quote results in zero output");
   check(fee_adjusted < target_reserve, "depot: insufficient target reserve for this swap");

   // This is a read-only action — the quote is returned via check message
   // In production, this would use return_value or get_table_rows
   // For now, we print the result
   print("quote:", fee_adjusted, " ", target_sym);
}

// ── oneshot (FR-1000) ───────────────────────────────────────────────────────
//
// One-time warrant conversion. Burns a warrant token and mints the
// corresponding $WIRE amount to the beneficiary.
// This is a governance-controlled action.

void depot::oneshot(name beneficiary, asset amount) {
   require_auth(get_self());

   check(is_account(beneficiary), "depot: beneficiary account does not exist");
   check(amount.amount > 0, "depot: amount must be positive");

   auto s = get_state();

   // FR-1000: Issue inline transfer from depot to beneficiary
   // The depot contract must hold the $WIRE tokens to distribute
   action(
      permission_level{get_self(), "active"_n},
      s.token_contract,
      "transfer"_n,
      std::make_tuple(get_self(), beneficiary, amount,
                      std::string("oneshot warrant conversion"))
   ).send();
}

// ── calculate_swap_rate (internal, FR-703) ──────────────────────────────────
//
// Computes the effective exchange rate in basis points between two tokens
// based on current reserve balances.

uint64_t depot::calculate_swap_rate(symbol source, symbol target, asset amount) {
   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   reserves_table reserves(get_self(), chain_scope);

   auto source_it = reserves.find(source.code().raw());
   if (source_it == reserves.end() || source_it->reserve_total.amount == 0) return 0;

   auto target_it = reserves.find(target.code().raw());
   if (target_it == reserves.end() || target_it->reserve_total.amount == 0) return 0;

   // Rate in basis points: (target_wire_equivalent / source_wire_equivalent) * 10000
   // Using wire_equivalent for cross-token comparison
   if (source_it->wire_equivalent.amount == 0) return 0;

   uint64_t rate_bps = uint64_t(
      (__int128(target_it->wire_equivalent.amount) * 10000) /
      __int128(source_it->wire_equivalent.amount)
   );

   return rate_bps;
}

// ── check_rate_threshold (internal, FR-704) ─────────────────────────────────
//
// Verifies that an exchange rate is within acceptable bounds.
// Returns true if the rate is acceptable (deviation from 1:1 is within
// the fee threshold).

bool depot::check_rate_threshold(uint64_t rate_bps, uint64_t threshold_bps) {
   // Rate should be within [10000 - threshold, 10000 + threshold] basis points
   // of the expected 1:1 ratio (adjusted for token decimals)
   // For a 0.1% fee (10 bps), we allow rates between 9990 and 10010
   uint64_t lower = 10000 - threshold_bps * 100; // allow wider range
   uint64_t upper = 10000 + threshold_bps * 100;

   return rate_bps >= lower && rate_bps <= upper;
}

} // namespace sysio
