#include <sysio.depot/sysio.depot.hpp>
#include <sysio/transaction.hpp>

namespace sysio {

// ── regoperator (FR-602) ─────────────────────────────────────────────────────

void depot::regoperator(name              wire_account,
                        operator_type_t   op_type,
                        std::vector<char> secp256k1_pubkey,
                        std::vector<char> ed25519_pubkey,
                        asset             collateral) {
   require_auth(wire_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   // Validate key sizes (FR-606)
   check(secp256k1_pubkey.size() == 33, "depot: secp256k1 pubkey must be 33 bytes compressed");
   check(ed25519_pubkey.size() == 32, "depot: ed25519 pubkey must be 32 bytes");
   check(collateral.amount > 0, "depot: collateral must be positive");
   check(op_type == operator_type_node || op_type == operator_type_batch ||
         op_type == operator_type_underwriter || op_type == operator_type_challenger,
         "depot: invalid operator type");

   known_operators_table ops(get_self(), chain_scope);

   // Check no duplicate registration for this account
   auto by_account = ops.get_index<"byaccount"_n>();
   auto acct_it = by_account.find(wire_account.value);
   check(acct_it == by_account.end(), "depot: operator already registered for this chain");

   // Check no duplicate secp256k1 pubkey
   auto by_secp = ops.get_index<"bysecppub"_n>();
   auto secp_hash = sha256(secp256k1_pubkey.data(), secp256k1_pubkey.size());
   check(by_secp.find(secp_hash) == by_secp.end(),
         "depot: secp256k1 pubkey already registered");

   ops.emplace(get_self(), [&](auto& o) {
      o.id                = ops.available_primary_key();
      o.op_type           = op_type;
      o.status            = operator_status_warmup;
      o.wire_account      = wire_account;
      o.secp256k1_pubkey  = secp256k1_pubkey;
      o.ed25519_pubkey    = ed25519_pubkey;
      o.collateral        = collateral;
      o.registered_at     = current_time_point();
      o.status_changed_at = current_time_point();
   });
}

// ── unregop ──────────────────────────────────────────────────────────────────

void depot::unregop(name wire_account) {
   require_auth(wire_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto it = by_account.find(wire_account.value);
   check(it != by_account.end(), "depot: operator not found");
   check(it->status != operator_status_slashed, "depot: operator already slashed");
   check(it->status != operator_status_exited, "depot: operator already exited");
   check(it->status != operator_status_cooldown, "depot: operator already in cooldown");
   check(it->status == operator_status_active || it->status == operator_status_warmup,
         "depot: operator must be active or in warmup to unregister");

   by_account.modify(it, same_payer, [&](auto& o) {
      o.status            = operator_status_cooldown;
      o.status_changed_at = current_time_point();
   });
}

// ── exitop (cooldown -> exited, graceful exit) ───────────────────────────────

void depot::exitop(name wire_account) {
   require_auth(wire_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto it = by_account.find(wire_account.value);
   check(it != by_account.end(), "depot: operator not found");
   check(it->status == operator_status_cooldown, "depot: operator must be in cooldown to exit");

   // TODO: Verify cooldown duration has elapsed (OQ-010: durations not yet specified)

   by_account.modify(it, same_payer, [&](auto& o) {
      o.status            = operator_status_exited;
      o.status_changed_at = current_time_point();
   });

   // TODO: Return collateral to operator's wire_account
}

// ── activateop ───────────────────────────────────────────────────────────────

void depot::activateop(name wire_account) {
   require_auth(get_self()); // governance action

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto it = by_account.find(wire_account.value);
   check(it != by_account.end(), "depot: operator not found");
   check(it->status == operator_status_warmup, "depot: operator must be in warmup state");

   by_account.modify(it, same_payer, [&](auto& o) {
      o.status            = operator_status_active;
      o.status_changed_at = current_time_point();
   });
}

// ── slashop (FR-605 / FR-504) ────────────────────────────────────────────────

void depot::slashop(name wire_account, std::string reason) {
   require_auth(get_self()); // only callable internally or by governance

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto it = by_account.find(wire_account.value);
   check(it != by_account.end(), "depot: operator not found");
   check(it->status != operator_status_slashed, "depot: operator already slashed");

   by_account.modify(it, same_payer, [&](auto& o) {
      o.status            = operator_status_slashed;
      o.collateral.amount = 0; // DEC-006: collapse ALL collateral
      o.status_changed_at = current_time_point();
   });

   // Queue outbound slash notification to Outpost (FR-504 step 6)
   // Roster update with reason BAD_BEHAVIOUR
   std::vector<char> slash_payload;
   // Serialize: operator_id + reason
   // The exact serialization format will be defined by OPP spec
   queue_outbound_message(assertion_type_slash_operator, slash_payload);
}

// ── elect_operators_for_epoch (FR-604) ───────────────────────────────────────

void depot::elect_operators_for_epoch(uint64_t next_epoch) {
   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   // Check if schedule already exists for this epoch
   op_schedule_table sched(get_self(), chain_scope);
   auto existing = sched.find(next_epoch);
   if (existing != sched.end()) return; // already elected

   // Gather all active batch operators
   known_operators_table ops(get_self(), chain_scope);
   auto by_ts = ops.get_index<"bytypestatus"_n>();
   uint128_t key = (uint128_t(operator_type_batch) << 64) | uint64_t(operator_status_active);
   auto it = by_ts.lower_bound(key);

   std::vector<uint64_t> active_ids;
   while (it != by_ts.end() && it->op_type == operator_type_batch && it->status == operator_status_active) {
      active_ids.push_back(it->id);
      ++it;
   }

   check(active_ids.size() >= MAX_BATCH_OPERATORS_PER_EPOCH,
         "depot: insufficient active batch operators for election");

   // Get previous epoch schedule to enforce no-consecutive rule
   std::vector<uint64_t> prev_elected;
   if (next_epoch > 1) {
      auto prev = sched.find(next_epoch - 1);
      if (prev != sched.end()) {
         prev_elected = prev->elected_operator_ids;
      }
   }

   // Random round-robin selection (FR-604)
   // Use block hash as randomness source for deterministic on-chain election
   // tapos_block_prefix gives us block-specific entropy
   uint32_t seed = sysio::tapos_block_prefix();
   std::vector<uint64_t> eligible;
   for (auto id : active_ids) {
      // No consecutive participation rule
      bool was_previous = false;
      for (auto prev_id : prev_elected) {
         if (prev_id == id) { was_previous = true; break; }
      }
      if (!was_previous) {
         eligible.push_back(id);
      }
   }

   // If not enough eligible due to no-consecutive rule, relax it
   if (eligible.size() < MAX_BATCH_OPERATORS_PER_EPOCH) {
      eligible = active_ids;
   }

   // Fisher-Yates shuffle using deterministic seed
   for (uint32_t i = eligible.size() - 1; i > 0; --i) {
      seed = seed * 1103515245 + 12345; // LCG
      uint32_t j = seed % (i + 1);
      auto tmp = eligible[i];
      eligible[i] = eligible[j];
      eligible[j] = tmp;
   }

   // Take first MAX_BATCH_OPERATORS_PER_EPOCH
   std::vector<uint64_t> elected(eligible.begin(),
      eligible.begin() + std::min((uint32_t)eligible.size(), MAX_BATCH_OPERATORS_PER_EPOCH));

   sched.emplace(get_self(), [&](auto& r) {
      r.epoch_number        = next_epoch;
      r.elected_operator_ids = elected;
      r.created_at          = current_time_point();
   });
}

// ── verify_elected (internal) ────────────────────────────────────────────────

void depot::verify_elected(name operator_account, uint64_t epoch_number) {
   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   // Find operator id
   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto op_it = by_account.find(operator_account.value);
   check(op_it != by_account.end(), "depot: operator not registered");

   // Find schedule for this epoch
   op_schedule_table sched(get_self(), chain_scope);
   auto sch_it = sched.find(epoch_number);
   check(sch_it != sched.end(), "depot: no schedule for epoch");

   bool found = false;
   for (auto id : sch_it->elected_operator_ids) {
      if (id == op_it->id) { found = true; break; }
   }
   check(found, "depot: operator not elected for this epoch (FR-605: non-elected delivery => slash)");
}

} // namespace sysio
