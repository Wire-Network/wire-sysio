#include <sysio.uwrit/sysio.uwrit.hpp>

namespace sysio {

using opp::types::UnderwriteStatus;

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void uwrit::setconfig(uint32_t fee_bps,
                      uint32_t confirm_lock_sec,
                      uint32_t uw_fee_share_pct,
                      uint32_t other_uw_share_pct,
                      uint32_t batch_op_share_pct) {
   require_auth(get_self());

   check(uw_fee_share_pct + other_uw_share_pct + batch_op_share_pct == 100,
         "fee share percentages must sum to 100");
   check(fee_bps <= 10000, "fee_bps cannot exceed 10000 (100%)");
   check(confirm_lock_sec > 0, "confirm_lock_sec must be positive");

   uwconfig_t cfg_tbl(get_self());
   uw_config cfg = cfg_tbl.get_or_default(uw_config{});
   cfg.fee_bps = fee_bps;
   cfg.confirm_lock_sec = confirm_lock_sec;
   cfg.uw_fee_share_pct = uw_fee_share_pct;
   cfg.other_uw_share_pct = other_uw_share_pct;
   cfg.batch_op_share_pct = batch_op_share_pct;
   cfg_tbl.set(cfg, get_self());
}

// ---------------------------------------------------------------------------
//  submituw
// ---------------------------------------------------------------------------
void uwrit::submituw(name underwriter, uint64_t msg_id,
                     checksum256 source_sig, checksum256 target_sig) {
   require_auth(underwriter);

   uwconfig_t cfg_tbl(get_self());
   check(cfg_tbl.exists(), "underwriting config not initialized");
   auto cfg = cfg_tbl.get();

   // Check no existing underwriting for this message
   uwledger_t ledger(get_self());
   auto msg_idx = ledger.get_index<"bymessage"_n>();
   auto existing = msg_idx.find(msg_id);
   check(existing == msg_idx.end(), "message already has underwriting entry");

   // TODO: Verify underwriter has sufficient uncommitted collateral on BOTH
   //       source and target chains by reading collateral table.
   //       For now, create the ledger entry.

   auto now = current_time_point();
   auto unlock = now + microseconds(static_cast<int64_t>(cfg.confirm_lock_sec) * 1'000'000);

   uint64_t next_id = ledger.available_primary_key();

   underwriting_entry e{};
   e.id          = next_id;
   e.underwriter = underwriter;
   e.message_id  = msg_id;
   e.status      = UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED;
   e.intent_time = now;
   e.unlock_time = unlock;
   e.source_sig  = source_sig;
   e.target_sig  = target_sig;
   ledger.emplace(underwriter, id_key{next_id}, e);

   // TODO: Queue ATTESTATION_TYPE_UNDERWRITE_INTENT to BOTH outposts
   //       via sysio.msgch::queueout inline action.
}

// ---------------------------------------------------------------------------
//  confirmuw
// ---------------------------------------------------------------------------
void uwrit::confirmuw(uint64_t uw_entry_id) {
   require_auth(get_self());

   uwledger_t ledger(get_self());
   auto pk = id_key{uw_entry_id};
   auto row = ledger.get(pk, "underwriting entry not found");
   check(row.status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED,
         "entry not in INTENT_SUBMITTED state");

   // TODO: Verify BOTH outpost confirmations have been received.
   //       Calculate exchange rate via reserve balances, verify threshold.

   ledger.modify(same_payer, pk, [&](auto& e) {
      e.status = UnderwriteStatus::UNDERWRITE_STATUS_INTENT_CONFIRMED;
   });

   // TODO: Queue ATTESTATION_TYPE_REMIT to target outpost via sysio.msgch::queueout.
}

// ---------------------------------------------------------------------------
//  expirelock
// ---------------------------------------------------------------------------
void uwrit::expirelock(uint64_t uw_entry_id) {
   // Permissionless — anyone can call to expire stale locks.
   uwledger_t ledger(get_self());
   auto pk = id_key{uw_entry_id};
   auto row = ledger.get(pk, "underwriting entry not found");
   check(row.status != UnderwriteStatus::UNDERWRITE_STATUS_RELEASED &&
         row.status != UnderwriteStatus::UNDERWRITE_STATUS_SLASHED,
         "entry already finalized");

   auto now = current_time_point();
   check(now >= row.unlock_time, "lock has not expired yet");

   ledger.modify(same_payer, pk, [&](auto& e) {
      e.status = UnderwriteStatus::UNDERWRITE_STATUS_RELEASED;
   });

   // Release committed collateral
   collateral_t collateral(get_self());
   auto uw_idx = collateral.get_index<"byuw"_n>();
   for (auto c_it = uw_idx.lower_bound(row.underwriter.value);
        c_it != uw_idx.end() && c_it->underwriter == row.underwriter; ++c_it) {
      auto col_pk = c_it.key();
      if (c_it->chain_kind == row.source_chain) {
         collateral.modify(same_payer, col_pk, [&](auto& c) {
            c.locked_amount    -= row.source_amount;
            c.available_amount += row.source_amount;
         });
      }
      if (c_it->chain_kind == row.target_chain) {
         collateral.modify(same_payer, col_pk, [&](auto& c) {
            c.locked_amount    -= row.target_amount;
            c.available_amount += row.target_amount;
         });
      }
   }
}

// ---------------------------------------------------------------------------
//  distfee
// ---------------------------------------------------------------------------
void uwrit::distfee(uint64_t uw_entry_id) {
   require_auth(get_self());

   uwledger_t ledger(get_self());
   auto pk = id_key{uw_entry_id};
   auto row = ledger.get(pk, "underwriting entry not found");
   check(row.status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_CONFIRMED,
         "entry not confirmed");

   uwconfig_t cfg_tbl(get_self());
   auto cfg = cfg_tbl.get();

   // TODO: Calculate fee based on fee_bps applied to both source and target amounts.
   //       Split according to uw_fee_share_pct / other_uw_share_pct / batch_op_share_pct.
   //       Transfer fee shares to respective accounts.
   //       For now, mark as completed.

   ledger.modify(same_payer, pk, [&](auto& e) {
      e.status = UnderwriteStatus::UNDERWRITE_STATUS_RELEASED;
   });
}

// ---------------------------------------------------------------------------
//  updcltrl
// ---------------------------------------------------------------------------
void uwrit::updcltrl(name underwriter, fc::crypto::chain_kind_t chain_kind,
                     asset amount, bool is_increase) {
   require_auth(get_self());

   collateral_t collateral(get_self());

   // Find existing entry for this underwriter + chain_kind
   auto uw_idx = collateral.get_index<"byuwchain"_n>();
   uint128_t composite = (static_cast<uint128_t>(underwriter.value) << 64) |
                          static_cast<uint64_t>(chain_kind);
   auto it = uw_idx.find(composite);

   if (it == uw_idx.end()) {
      check(is_increase, "cannot decrease non-existent collateral");

      uint64_t next_id = collateral.available_primary_key();

      collateral_entry c{};
      c.id               = next_id;
      c.underwriter      = underwriter;
      c.chain_kind       = chain_kind;
      c.staked_amount    = amount;
      c.locked_amount    = asset(0, amount.symbol);
      c.available_amount = amount;
      collateral.emplace(get_self(), id_key{next_id}, c);
   } else {
      collateral.modify(same_payer, it.key(), [&](auto& c) {
         if (is_increase) {
            c.staked_amount    += amount;
            c.available_amount += amount;
         } else {
            check(c.available_amount >= amount, "insufficient available collateral");
            c.staked_amount    -= amount;
            c.available_amount -= amount;
         }
      });
   }
}

// ---------------------------------------------------------------------------
//  slash
// ---------------------------------------------------------------------------
void uwrit::slash(name underwriter, std::string reason) {
   require_auth(CHALG_ACCOUNT);

   // Seize ALL collateral from slashed underwriter
   collateral_t collateral(get_self());
   {
      auto uw_idx = collateral.get_index<"byuw"_n>();
      std::vector<id_key> to_zero;
      for (auto it = uw_idx.lower_bound(underwriter.value);
           it != uw_idx.end() && it->underwriter == underwriter; ++it) {
         to_zero.push_back(it.key());
      }
      for (const auto& pk : to_zero) {
         collateral.modify(same_payer, pk, [&](auto& c) {
            c.staked_amount    = asset(0, c.staked_amount.symbol);
            c.locked_amount    = asset(0, c.locked_amount.symbol);
            c.available_amount = asset(0, c.available_amount.symbol);
         });
      }
   }

   // Mark all active underwriting entries as slashed
   uwledger_t ledger(get_self());
   {
      auto uw_ledger_idx = ledger.get_index<"byuw"_n>();
      std::vector<id_key> to_slash;
      for (auto it = uw_ledger_idx.lower_bound(underwriter.value);
           it != uw_ledger_idx.end() && it->underwriter == underwriter; ++it) {
         if (it->status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED ||
             it->status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_CONFIRMED) {
            to_slash.push_back(it.key());
         }
      }
      for (const auto& pk : to_slash) {
         ledger.modify(same_payer, pk, [&](auto& e) {
            e.status = UnderwriteStatus::UNDERWRITE_STATUS_SLASHED;
         });
      }
   }

   // TODO: Distribute seized collateral per slash distribution rules:
   //       50% to challenger, 25% to other underwriters, 25% to batch operators.
   //       Queue ATTESTATION_TYPE_SLASH_OPERATOR to all outposts.
}

// ---------------------------------------------------------------------------
//  createuwreq — create underwrite request (called inline from sysio.msgch)
// ---------------------------------------------------------------------------
void uwrit::createuwreq(uint64_t attestation_id,
                         opp::types::AttestationType type,
                         std::vector<char> data) {
   require_auth(MSGCH_ACCOUNT);

   uwreqs_t reqs(get_self());
   auto pk = id_key{attestation_id};
   check(!reqs.contains(pk),
         "underwrite request already exists for this attestation");

   uw_request_t r{};
   r.id                 = attestation_id;
   r.type               = type;
   r.status             = opp::types::UNDERWRITE_REQUEST_STATUS_PENDING;
   r.uw_name            = name{};
   r.locked_amounts     = {};
   r.unlock_timestamp   = 0;
   r.released_timestamp = 0;
   r.slashed_timestamp  = 0;
   reqs.emplace(get_self(), pk, r);
}

} // namespace sysio
