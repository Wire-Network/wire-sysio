#include <sysio.depot/sysio.depot.hpp>

namespace sysio {

// ── challenge (FR-501/502) ──────────────────────────────────────────────────
//
// A challenger submits a challenge against an epoch.
// Suspends normal message processing, enters CHALLENGE state,
// queues a CHALLENGE_REQUEST outbound message for the Outpost.

void depot::challenge(name challenger, uint64_t epoch_number, std::vector<char> evidence) {
   require_auth(challenger);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   check(s.state == depot_state_active || s.state == depot_state_challenge,
         "depot: system is paused, cannot accept challenges");

   // Verify the challenger is a registered challenger operator
   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto op_it = by_account.find(challenger.value);
   check(op_it != by_account.end(), "depot: challenger not registered");
   check(op_it->op_type == operator_type_challenger, "depot: account is not a challenger");
   check(op_it->status == operator_status_active, "depot: challenger is not active");

   // Verify epoch exists and is in a challengeable state
   opp_epoch_in_table epochs(get_self(), chain_scope);
   auto ep_it = epochs.find(epoch_number);
   check(ep_it != epochs.end(), "depot: epoch not found");
   check(!ep_it->challenge_flag, "depot: epoch already under challenge");

   // Check no active challenge exists for this epoch
   challenges_table chals(get_self(), chain_scope);
   auto by_epoch = chals.get_index<"byepoch"_n>();
   auto chal_it = by_epoch.find(epoch_number);
   if (chal_it != by_epoch.end()) {
      check(chal_it->status == challenge_status_resolved,
            "depot: epoch already has an active challenge");
   }

   // Create challenge record
   chals.emplace(get_self(), [&](auto& c) {
      c.id             = chals.available_primary_key();
      c.epoch_number   = epoch_number;
      c.status         = challenge_status_round1_pending;
      c.round          = 1;
      c.challenge_data = evidence;
   });

   // Mark epoch as challenged
   epochs.modify(ep_it, same_payer, [&](auto& e) {
      e.challenge_flag = true;
   });

   // Enter challenge state
   enter_challenge_state(epoch_number);

   // FR-502: Queue outbound CHALLENGE_REQUEST to Outpost
   queue_outbound_message(assertion_type_challenge_response, evidence);
}

// ── chalresp (FR-503/504) ───────────────────────────────────────────────────
//
// An elected batch operator responds to a challenge.
// If this is round 1, compare with original. If mismatch on round 2, PAUSE.

void depot::chalresp(name operator_account, uint64_t challenge_id,
                     std::vector<char> response_data) {
   require_auth(operator_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   check(s.state == depot_state_challenge, "depot: system is not in challenge state");

   // Verify operator is elected
   verify_elected(operator_account, s.current_epoch);

   // Find the challenge
   challenges_table chals(get_self(), chain_scope);
   auto chal_it = chals.find(challenge_id);
   check(chal_it != chals.end(), "depot: challenge not found");
   check(chal_it->status == challenge_status_round1_pending ||
         chal_it->status == challenge_status_round2_pending,
         "depot: challenge is not awaiting a response");

   uint8_t current_round = chal_it->round;

   // Compute hash of the response data
   checksum256 response_hash = sha256(response_data.data(), response_data.size());

   // Compare with the original epoch data
   opp_epoch_in_table epochs(get_self(), chain_scope);
   auto ep_it = epochs.find(chal_it->epoch_number);
   check(ep_it != epochs.end(), "depot: challenged epoch not found");

   bool hashes_match = (response_hash == ep_it->epoch_merkle);

   if (current_round == 1) {
      if (hashes_match) {
         // FR-503: Round 1 — response matches original, challenge fails
         chals.modify(chal_it, same_payer, [&](auto& c) {
            c.status = challenge_status_round1_complete;
         });

         // Resume normal operations
         resume_from_challenge();
      } else {
         // FR-504: Round 1 — mismatch, escalate to round 2
         chals.modify(chal_it, same_payer, [&](auto& c) {
            c.status = challenge_status_round2_pending;
            c.round  = 2;
         });

         // Queue second challenge request
         queue_outbound_message(assertion_type_challenge_response, response_data);
      }
   } else if (current_round == 2) {
      if (hashes_match) {
         // FR-505: Round 2 — response matches, original was wrong
         chals.modify(chal_it, same_payer, [&](auto& c) {
            c.status = challenge_status_round2_complete;
         });

         // Slash original operators who delivered wrong data
         epoch_votes_table votes(get_self(), chain_scope);
         auto by_epoch = votes.get_index<"byepoch"_n>();
         auto vit = by_epoch.lower_bound(chal_it->epoch_number);
         while (vit != by_epoch.end() && vit->epoch_number == chal_it->epoch_number) {
            if (vit->chain_hash != response_hash) {
               // Slash this operator
               known_operators_table ops2(get_self(), chain_scope);
               auto op2 = ops2.find(vit->operator_id);
               if (op2 != ops2.end() && op2->status != operator_status_slashed) {
                  ops2.modify(op2, same_payer, [&](auto& o) {
                     o.status            = operator_status_slashed;
                     o.collateral.amount = 0;
                     o.status_changed_at = current_time_point();
                  });
               }
            }
            ++vit;
         }

         // Resume with corrected data
         resume_from_challenge();
      } else {
         // FR-506: Round 2 — second mismatch, enter PAUSE state
         chals.modify(chal_it, same_payer, [&](auto& c) {
            c.status = challenge_status_paused;
         });

         // Enter global pause — manual resolution required
         auto state = get_state();
         state.state = depot_state_paused;
         set_state(state);
      }
   }
}

// ── chalresolve (FR-507) ────────────────────────────────────────────────────
//
// Manual resolution: A T1/T2 assembler proposes a resolution with
// 3 hashes (original, round1, round2). Requires 2/3 supermajority vote
// from active node operators to accept.

void depot::chalresolve(name        proposer,
                        uint64_t    challenge_id,
                        checksum256 original_hash,
                        checksum256 round1_hash,
                        checksum256 round2_hash) {
   require_auth(proposer);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   check(s.state == depot_state_paused, "depot: system must be paused for manual resolution");

   challenges_table chals(get_self(), chain_scope);
   auto chal_it = chals.find(challenge_id);
   check(chal_it != chals.end(), "depot: challenge not found");
   check(chal_it->status == challenge_status_paused,
         "depot: challenge must be in paused state for resolution");

   // Create a fork proposal for voting
   opp_forks_table forks(get_self(), chain_scope);
   uint64_t fork_id = forks.available_primary_key();

   // The merkle root here represents the "correct" chain hash as determined
   // by the resolver's analysis of original, round1, and round2 hashes
   forks.emplace(get_self(), [&](auto& f) {
      f.fork_id        = fork_id;
      f.epoch_number   = chal_it->epoch_number;
      f.end_message_id = 0; // will be determined after vote passes
      f.merkle_root    = round2_hash; // the latest re-derivation is proposed as correct
   });
}

// ── chalvote (FR-507) ───────────────────────────────────────────────────────
//
// Active node operators vote on a manual resolution proposal.
// If 2/3 supermajority approves, the resolution is applied:
// slash losers, resume operations.

void depot::chalvote(name voter, uint64_t challenge_id, bool approve) {
   require_auth(voter);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   check(s.state == depot_state_paused, "depot: system must be paused for voting");

   // Verify voter is an active node operator
   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto op_it = by_account.find(voter.value);
   check(op_it != by_account.end(), "depot: voter not registered as operator");
   check(op_it->op_type == operator_type_node, "depot: only node operators can vote on resolutions");
   check(op_it->status == operator_status_active, "depot: voter must be active");

   // Find the challenge and associated fork
   challenges_table chals(get_self(), chain_scope);
   auto chal_it = chals.find(challenge_id);
   check(chal_it != chals.end(), "depot: challenge not found");
   check(chal_it->status == challenge_status_paused, "depot: challenge not in voting state");

   // Find the fork proposal for this epoch
   opp_forks_table forks(get_self(), chain_scope);
   auto by_epoch = forks.get_index<"byepoch"_n>();
   auto fork_it = by_epoch.find(chal_it->epoch_number);
   check(fork_it != by_epoch.end(), "depot: no resolution proposal found for this epoch");

   // Check voter hasn't already voted on this fork
   opp_fork_votes_table votes(get_self(), chain_scope);
   auto by_fu = votes.get_index<"byforkuser"_n>();
   uint128_t fu_key = (uint128_t(fork_it->fork_id) << 64) | voter.value;
   check(by_fu.find(fu_key) == by_fu.end(), "depot: already voted on this resolution");

   // Record vote
   votes.emplace(get_self(), [&](auto& v) {
      v.id         = votes.available_primary_key();
      v.fork_id    = fork_it->fork_id;
      v.voter      = voter;
      v.vote_state = approve ? 1 : 2;
   });

   // Count votes for this fork
   auto vit = by_fu.lower_bound(uint128_t(fork_it->fork_id) << 64);
   uint32_t accept_count = 0;
   uint32_t total_count  = 0;
   while (vit != by_fu.end()) {
      // Extract fork_id from composite key
      uint64_t vid_fork = uint64_t(vit->by_fork_user() >> 64);
      if (vid_fork != fork_it->fork_id) break;

      ++total_count;
      if (vit->vote_state == 1) ++accept_count;
      ++vit;
   }

   // Count total active node operators for threshold calculation
   auto by_ts = ops.get_index<"bytypestatus"_n>();
   uint128_t node_key = (uint128_t(operator_type_node) << 64) | uint64_t(operator_status_active);
   auto node_it = by_ts.lower_bound(node_key);
   uint32_t total_nodes = 0;
   while (node_it != by_ts.end() &&
          node_it->op_type == operator_type_node &&
          node_it->status == operator_status_active) {
      ++total_nodes;
      ++node_it;
   }

   // FR-507: Check if 2/3 supermajority is reached
   if (total_nodes > 0 && accept_count * 10000 >= total_nodes * RESOLUTION_VOTE_THRESHOLD_BPS) {
      // Resolution accepted — apply it
      chals.modify(chal_it, same_payer, [&](auto& c) {
         c.status = challenge_status_resolved;
      });

      // Slash operators who delivered non-consensus data
      slash_minority_operators(chain_scope, chal_it->epoch_number, fork_it->merkle_root);

      // Resume operations
      resume_from_challenge();
   }
}

// ── enter_challenge_state (internal) ────────────────────────────────────────

void depot::enter_challenge_state(uint64_t epoch_number) {
   auto s = get_state();
   if (s.state != depot_state_challenge) {
      s.state = depot_state_challenge;
      set_state(s);
   }
}

// ── resume_from_challenge (internal) ────────────────────────────────────────

void depot::resume_from_challenge() {
   auto s = get_state();
   s.state = depot_state_active;
   set_state(s);
}

// ── distribute_slash (internal, FR-504/DEC-005) ─────────────────────────────
//
// Distributes slashed collateral:
//   50% to the challenger
//   25% to underwriters
//   25% to batch operators

void depot::distribute_slash(uint64_t operator_id, name challenger) {
   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   known_operators_table ops(get_self(), chain_scope);
   auto op_it = ops.find(operator_id);
   if (op_it == ops.end()) return;

   int64_t total_collateral = op_it->collateral.amount;
   if (total_collateral <= 0) return;

   symbol collateral_sym = op_it->collateral.symbol;

   // Calculate shares
   int64_t challenger_share = (total_collateral * SLASH_CHALLENGER_BPS) / 10000;
   int64_t underwriter_share = (total_collateral * SLASH_UNDERWRITERS_BPS) / 10000;
   int64_t batch_share = total_collateral - challenger_share - underwriter_share;

   // Zero the operator's collateral
   ops.modify(op_it, same_payer, [&](auto& o) {
      o.collateral.amount = 0;
      o.status            = operator_status_slashed;
      o.status_changed_at = current_time_point();
   });

   // TODO: Transfer shares via inline actions to sysio.token
   // action(permission_level{get_self(), "active"_n},
   //        s.token_contract, "transfer"_n,
   //        std::make_tuple(get_self(), challenger,
   //                        asset(challenger_share, collateral_sym),
   //                        std::string("slash distribution - challenger")))
   //    .send();
   //
   // Underwriter and batch operator shares would need to be distributed
   // proportionally among all active operators of those types.
}

} // namespace sysio
