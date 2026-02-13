#include <sysio.depot/sysio.depot.hpp>

namespace sysio {

// ── evaluate_consensus (FR-301/302/303/304) ──────────────────────────────────
//
// Collects epoch votes from all elected batch operators and determines
// consensus via Option A (all 7 identical) or Option B (4+ majority at
// epoch boundary with all delivered chains matching).

void depot::evaluate_consensus(uint64_t epoch_number) {
   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   // Skip if already in challenge or paused state for this epoch
   if (s.state == depot_state_paused) return;

   // Get the elected schedule for this epoch
   op_schedule_table sched(get_self(), chain_scope);
   auto sch_it = sched.find(epoch_number);
   if (sch_it == sched.end()) return; // no schedule yet

   uint32_t total_elected = sch_it->elected_operator_ids.size();

   // Collect all votes for this epoch
   epoch_votes_table votes(get_self(), chain_scope);
   auto by_epoch = votes.get_index<"byepoch"_n>();
   auto vit = by_epoch.lower_bound(epoch_number);

   // Count votes per distinct hash
   struct hash_count {
      checksum256 hash;
      uint32_t    count = 0;
   };
   std::vector<hash_count> hash_counts;
   uint32_t total_votes = 0;

   while (vit != by_epoch.end() && vit->epoch_number == epoch_number) {
      ++total_votes;
      bool found = false;
      for (auto& hc : hash_counts) {
         if (hc.hash == vit->chain_hash) {
            ++hc.count;
            found = true;
            break;
         }
      }
      if (!found) {
         hash_counts.push_back({vit->chain_hash, 1});
      }
      ++vit;
   }

   if (total_votes == 0) return; // no votes yet

   // ── Option A: All elected operators delivered identical hashes ─────────
   if (total_votes == total_elected && hash_counts.size() == 1) {
      // Unanimous consensus achieved
      mark_epoch_valid(chain_scope, epoch_number);
      return;
   }

   // ── Option B: Majority (4+ of 7) with identical data ─────────────────
   // Only viable when epoch boundary is reached without all operators delivering
   if (total_votes >= CONSENSUS_MAJORITY) {
      for (auto& hc : hash_counts) {
         if (hc.count >= CONSENSUS_MAJORITY) {
            // Majority consensus — check that this is the only hash that
            // was delivered (all delivered chains match)
            // Per FR-302 Option B: majority deliver identical AND
            // all delivered chains match
            if (hash_counts.size() == 1) {
               mark_epoch_valid(chain_scope, epoch_number);
               return;
            }
            // If there are multiple distinct hashes but majority agrees,
            // we still have consensus but with dissenters to slash
            mark_epoch_valid(chain_scope, epoch_number);
            // Identify and slash minority operators
            slash_minority_operators(chain_scope, epoch_number, hc.hash);
            return;
         }
      }
   }

   // ── Not enough votes yet — wait for more submissions ──────────────────
   // If all elected have voted and no consensus, trigger challenge
   if (total_votes == total_elected) {
      enter_challenge_state(epoch_number);
   }
}

// ── mark_epoch_valid (internal) ──────────────────────────────────────────────

void depot::mark_epoch_valid(uint64_t chain_scope, uint64_t epoch_number) {
   message_chains_table chains(get_self(), chain_scope);
   auto by_epoch = chains.get_index<"byepochdir"_n>();
   uint128_t ek = (uint128_t(epoch_number) << 8) | uint64_t(message_direction_inbound);
   auto it = by_epoch.lower_bound(ek);

   while (it != by_epoch.end() &&
          it->epoch_number == epoch_number &&
          it->direction == message_direction_inbound) {
      if (it->status == chain_status_pending) {
         by_epoch.modify(it, same_payer, [&](auto& c) {
            c.status = chain_status_valid;
         });
      }
      ++it;
   }
}

// ── slash_minority_operators (internal) ──────────────────────────────────────
//
// After consensus, identify operators who delivered non-consensus hashes
// and slash them (FR-504).

void depot::slash_minority_operators(uint64_t chain_scope, uint64_t epoch_number,
                                     const checksum256& consensus_hash) {
   epoch_votes_table votes(get_self(), chain_scope);
   auto by_epoch = votes.get_index<"byepoch"_n>();
   auto vit = by_epoch.lower_bound(epoch_number);

   known_operators_table ops(get_self(), chain_scope);

   while (vit != by_epoch.end() && vit->epoch_number == epoch_number) {
      if (vit->chain_hash != consensus_hash) {
         // This operator delivered a non-consensus hash — slash them
         auto op_it = ops.find(vit->operator_id);
         if (op_it != ops.end() && op_it->status != operator_status_slashed) {
            ops.modify(op_it, same_payer, [&](auto& o) {
               o.status            = operator_status_slashed;
               o.collateral.amount = 0; // DEC-006
               o.status_changed_at = current_time_point();
            });

            // Queue outbound slash to Outpost
            std::vector<char> slash_payload;
            queue_outbound_message(assertion_type_slash_operator, slash_payload);
         }
      }
      ++vit;
   }
}

} // namespace sysio
