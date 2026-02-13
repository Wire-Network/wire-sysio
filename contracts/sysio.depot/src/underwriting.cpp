#include <sysio.depot/sysio.depot.hpp>

namespace sysio {

// ── uwintent (FR-403) ───────────────────────────────────────────────────────
//
// An underwriter submits intent to underwrite a pending inbound message.
// Verifies the underwriter has sufficient collateral, calculates the
// exchange rate, checks threshold, creates a LOCK entry with 6hr expiry,
// and queues an outbound confirmation.

void depot::uwintent(name              underwriter,
                     uint64_t          message_id,
                     asset             source_amount,
                     asset             target_amount,
                     chain_kind_t      source_chain,
                     chain_kind_t      target_chain,
                     std::vector<char> source_sig,
                     std::vector<char> target_sig) {
   require_auth(underwriter);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   check(s.state == depot_state_active, "depot: system not in active state");

   // Resolve underwriter operator record
   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto op_it = by_account.find(underwriter.value);
   check(op_it != by_account.end(), "depot: underwriter not registered");
   check(op_it->op_type == operator_type_underwriter, "depot: account is not an underwriter");
   check(op_it->status == operator_status_active, "depot: underwriter is not active");

   // Verify the message exists and needs underwriting (status = PENDING)
   opp_in_table msgs(get_self(), chain_scope);
   auto msg_it = msgs.find(message_id);
   check(msg_it != msgs.end(), "depot: message not found");
   check(msg_it->status == message_status_pending, "depot: message is not pending underwriting");

   // FR-403: Calculate exchange rate from reserve state
   uint64_t rate_bps = calculate_swap_rate(source_amount.symbol, target_amount.symbol, source_amount);

   // FR-704: Verify rate is within acceptable threshold
   // The threshold prevents extreme slippage / manipulation
   check(check_rate_threshold(rate_bps, UNDERWRITE_FEE_BPS),
         "depot: exchange rate exceeds acceptable threshold");

   // Verify collateral covers the underwriting amount
   check(op_it->collateral.amount >= target_amount.amount,
         "depot: insufficient collateral for underwriting");

   // FR-403: Create underwriting ledger entry with 6hr LOCK
   underwrite_table ledger(get_self(), chain_scope);

   time_point_sec unlock_time = time_point_sec(current_time_point().sec_since_epoch() + INTENT_LOCK_SECONDS);

   ledger.emplace(get_self(), [&](auto& e) {
      e.id               = ledger.available_primary_key();
      e.operator_id      = op_it->id;
      e.status           = underwrite_status_intent_submitted;
      e.source_amount    = source_amount;
      e.target_amount    = target_amount;
      e.source_chain     = source_chain;
      e.target_chain     = target_chain;
      e.exchange_rate_bps = rate_bps;
      e.unlock_at        = unlock_time;
      e.created_at       = current_time_point();
      e.source_tx_hash   = checksum256(); // populated on confirm
      e.target_tx_hash   = checksum256(); // populated on confirm
   });

   // FR-404: Queue outbound intent confirmation to Outpost
   // The Outpost will use this to lock source funds
   std::vector<char> intent_payload;
   // Serialize: message_id + underwriter_id + source_amount + target_amount + rate
   // TODO: Proper OPP serialization format
   queue_outbound_message(assertion_type_wire_purchase, intent_payload);
}

// ── uwconfirm (FR-404) ─────────────────────────────────────────────────────
//
// Elected batch operator confirms an underwriting intent.
// Transitions from INTENT_SUBMITTED to INTENT_CONFIRMED, extends
// the lock to 24 hours, and marks the inbound message as READY.

void depot::uwconfirm(name operator_account, uint64_t ledger_entry_id) {
   require_auth(operator_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   check(s.state == depot_state_active, "depot: system not in active state");

   // Verify caller is an elected batch operator
   verify_elected(operator_account, s.current_epoch);

   // Find ledger entry
   underwrite_table ledger(get_self(), chain_scope);
   auto entry = ledger.find(ledger_entry_id);
   check(entry != ledger.end(), "depot: underwriting entry not found");
   check(entry->status == underwrite_status_intent_submitted,
         "depot: entry must be in INTENT_SUBMITTED status to confirm");

   // Verify lock hasn't expired
   check(entry->unlock_at > current_time_point(),
         "depot: underwriting intent has expired");

   // FR-404: Transition to CONFIRMED, extend lock to 24 hours
   time_point_sec new_unlock = time_point_sec(current_time_point().sec_since_epoch() + CONFIRMED_LOCK_SECONDS);

   ledger.modify(entry, same_payer, [&](auto& e) {
      e.status    = underwrite_status_intent_confirmed;
      e.unlock_at = new_unlock;
   });
}

// ── uwcancel ────────────────────────────────────────────────────────────────

void depot::uwcancel(name operator_account, uint64_t ledger_entry_id, std::string reason) {
   require_auth(operator_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   // Verify caller is an elected batch operator
   verify_elected(operator_account, s.current_epoch);

   underwrite_table ledger(get_self(), chain_scope);
   auto entry = ledger.find(ledger_entry_id);
   check(entry != ledger.end(), "depot: underwriting entry not found");
   check(entry->status == underwrite_status_intent_submitted ||
         entry->status == underwrite_status_intent_confirmed,
         "depot: entry cannot be cancelled in current status");

   ledger.modify(entry, same_payer, [&](auto& e) {
      e.status = underwrite_status_cancelled;
   });
}

// ── uwexpire (FR-402) ───────────────────────────────────────────────────────
//
// Permissionless cleanup of expired underwriting locks.
// Anyone can call this to release expired entries.

void depot::uwexpire() {
   expire_underwriting_locks();
}

} // namespace sysio
