
#include <sysio.system/sysio.system.hpp>
#include <sysio.system/snapshot_attest.hpp>
#include <sysio.system/block_utils.hpp>

#include <sysio/sysio.hpp>

#include <algorithm>
#include <utility>

namespace sysiosystem {

namespace {

constexpr char producer_not_registered_error[] = "producer is not registered";
constexpr char producer_not_active_error[] = "producer is not active";
constexpr char producer_rank_too_high_error[] = "producer rank exceeds maximum for snapshot providers";

/// Classifies the producer-table state that determines snapshot-provider eligibility.
enum class snapshot_producer_eligibility {
   eligible,
   inactive,
   rank_exceeds_maximum,
};

/// Returns the eligibility of a producer record for snapshot-provider registration and voting.
snapshot_producer_eligibility get_snapshot_producer_eligibility(const producer_info& producer) {
   if (!producer.active()) {
      return snapshot_producer_eligibility::inactive;
   }
   if (producer.rank > max_snap_provider_rank) {
      return snapshot_producer_eligibility::rank_exceeds_maximum;
   }
   return snapshot_producer_eligibility::eligible;
}

/// Requires the producer's current table state to permit snapshot-provider registration or voting.
void require_snapshot_producer_eligibility(const producers_table& producers, name producer) {
   const auto prod_itr = producers.require_find(producer_key_t{producer.value}, producer_not_registered_error);
   const auto eligibility = get_snapshot_producer_eligibility(*prod_itr);
   check(eligibility != snapshot_producer_eligibility::inactive, producer_not_active_error);
   check(eligibility != snapshot_producer_eligibility::rank_exceeds_maximum, producer_rank_too_high_error);
}

/// Counts the explicitly registered provider mappings that define the stable quorum membership.
uint32_t count_snapshot_providers(const snap_providers_table& providers) {
   uint32_t provider_count = 0;
   for (auto provider_itr = providers.begin(); provider_itr != providers.end(); ++provider_itr) {
      ++provider_count;
   }
   return provider_count;
}

/// Returns whether a pending vote remains backed by a currently eligible provider mapping.
bool is_currently_eligible_snapshot_voter(snap_providers_table& providers,
                                          const producers_table& producers,
                                          name producer) {
   const auto by_producer = providers.get_index<"byproducer"_n>();
   if (by_producer.find(producer.value) == by_producer.end()) {
      return false;
   }

   const auto producer_row = producers.try_get(producer_key_t{producer.value});
   return producer_row
          && get_snapshot_producer_eligibility(*producer_row) == snapshot_producer_eligibility::eligible;
}

} // namespace

// -------------------------------------------------------------------------------------------------
void snapshot_attest::regsnapprov(name producer, name snap_account) {
   require_auth(producer);

   // Validate the producer's current eligibility. A retained mapping must never give an inactive
   // or demoted producer a way to re-enter snapshot voting.
   producers_table producers(get_self());
   require_snapshot_producer_eligibility(producers, producer);

   // Ensure snap_account is not already registered
   snap_providers_table provs(get_self());
   check(!provs.contains(snap_provider_key_t{snap_account.value}),
         "snap_account is already registered as a provider");

   // Ensure this producer doesn't already have a provider registered
   auto by_prod = provs.get_index<"byproducer"_n>();
   check(by_prod.find(producer.value) == by_prod.end(),
         "producer already has a registered snapshot provider");
   check(count_snapshot_providers(provs) < max_snap_provider_rank,
         "maximum registered snapshot providers reached");

   provs.emplace(producer, snap_provider_key_t{snap_account.value}, [&](auto& row) {
      row.snap_account = snap_account;
      row.producer     = producer;
   });
}

// -------------------------------------------------------------------------------------------------
// The `account` parameter is overloaded: it can be either a snap_account (primary key lookup)
// or a producer (secondary index lookup). The primary key path takes precedence.
// This means a producer can unregister their own provider, and a snap_account can unregister itself.
// regsnapprov() enforces uniqueness of both snap_account and producer, so collisions cannot occur.
void snapshot_attest::delsnapprov(name account) {
   require_auth(account);

   snap_providers_table provs(get_self());

   // First try lookup as snap_account (primary key)
   auto prov_itr = provs.find(snap_provider_key_t{account.value});
   if (prov_itr != provs.end()) {
      require_auth(prov_itr->snap_account);
      provs.erase(std::move(prov_itr));
      return;
   }

   // Then try lookup as producer (secondary index)
   auto by_prod = provs.get_index<"byproducer"_n>();
   auto prod_itr = by_prod.find(account.value);
   check(prod_itr != by_prod.end(), "account is not registered as a snapshot provider or producer");
   by_prod.erase(std::move(prod_itr));
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash) {
   require_auth(snap_account);

   // Validate snap_account is a registered provider and resolve its delegating producer.
   // Votes are tracked by the stable PRODUCER identity, not the snap_account: a producer can
   // rotate its snap_account (delsnapprov + regsnapprov), so de-duplicating by snap_account
   // would let a single producer accumulate several distinct names in `voters` and clear the
   // Byzantine quorum floor on its own. The producer:provider mapping is 1:1 (enforced by
   // regsnapprov), so the producer is the correct identity to count.
   snap_providers_table provs(get_self());
   auto prov_itr = provs.find(snap_provider_key_t{snap_account.value});
   check(prov_itr != provs.end(), "snap_account is not a registered snapshot provider");
   const name producer = prov_itr->producer;

   uint32_t block_num = block_info::block_height_from_id(block_id);
   check(block_num > 0, "invalid block_id");

   // Check for disagreement against any already-attested record. Both the snapshot hash and the
   // block id must match: an equal hash under a different block id is still a different
   // attestation (the voter is on a different fork at this height).
   // Uses snap_hash_disagreement_error code so nodeop can detect this specific failure
   // without fragile string matching (see producer_plugin.cpp::submit_snapshot_vote).
   snap_records_table records(get_self());
   snap_record_key_t  rec_key{static_cast<uint64_t>(block_num)};
   auto rec_itr = records.find(rec_key);
   if (rec_itr != records.end()) {
      check(rec_itr->snapshot_hash == snapshot_hash && rec_itr->block_id == block_id,
            snap_hash_disagreement_error);
   }

   // Provider mappings are historical records. Revalidate the delegating producer at vote time so
   // unregistration or a rank change revokes voting rights without requiring a separate cleanup action.
   // The disagreement check intentionally precedes this authorization check so an ineligible node
   // still receives the fatal disagreement signal for a snapshot that conflicts with an attested record.
   producers_table producers(get_self());
   require_snapshot_producer_eligibility(producers, producer);

   // Find or create the vote entry for this block_num + block_id + snapshot_hash. Votes only
   // aggregate toward quorum when they agree on BOTH the block id and the snapshot hash;
   // otherwise votes for different forks at the same height could jointly attest a record whose
   // block_id is whichever vote arrived first.
   snap_votes_table votes(get_self());
   auto by_bn = votes.get_index<"byblocknum"_n>();

   uint64_t vote_id           = 0;
   bool     found             = false;
   bool     append_vote       = false;
   uint64_t matching_vote_id  = 0;
   bool     has_matching_vote = false;
   for (auto itr = by_bn.lower_bound(static_cast<uint64_t>(block_num));
        itr != by_bn.end() && itr->block_num == block_num; ++itr) {
      // A producer may cast one vote for a height, regardless of the block/hash tuple. Otherwise
      // a Byzantine producer can add its weight to each competing candidate at the same height.
      for (const auto& voter : itr->voters) {
         if (voter == producer) {
            check(itr->block_id == block_id && itr->snapshot_hash == snapshot_hash,
                  "producer has already voted for this snapshot");
            // An idempotent retry lets a producer re-evaluate a pending vote after it regains
            // eligibility, without adding weight or allowing an equivocation across tuples.
            vote_id = itr->id;
            found   = true;
            break;
         }
      }
      if (found) {
         break;
      }
      if (itr->block_id == block_id && itr->snapshot_hash == snapshot_hash) {
         matching_vote_id  = itr->id;
         has_matching_vote = true;
      }
   }

   if (!found && has_matching_vote) {
      vote_id     = matching_vote_id;
      found       = true;
      append_vote = true;
   }

   if (append_vote) {
      votes.modify(same_payer, snap_vote_key_t{vote_id}, [&](auto& row) {
         row.voters.push_back(producer);
      });
   } else if (!found) {
      uint64_t new_id = votes.available_primary_key();
      votes.emplace(snap_account, snap_vote_key_t{new_id}, [&](auto& row) {
         row.id            = new_id;
         row.block_num     = block_num;
         row.block_id      = block_id;
         row.snapshot_hash = snapshot_hash;
         row.voters        = {producer};
      });
      vote_id = new_id;
   }

   // Count only voters that remain currently eligible and mapped. Quorum membership stays on the
   // registered mapping set, so rank churn cannot lower the threshold for a permanent record.
   uint32_t voter_count = 0;
   const auto current_vote = votes.get(snap_vote_key_t{vote_id});
   for (const auto& voter : current_vote.voters) {
      if (is_currently_eligible_snapshot_voter(provs, producers, voter)) {
         ++voter_count;
      }
   }

   // Check quorum.
   snap_config_singleton cfg_singleton(get_self());
   snap_config cfg = cfg_singleton.get_or_default(snap_config{});

   const uint32_t provider_count = count_snapshot_providers(provs);

   // Byzantine-safe quorum floor: under the standard < N/3 fault assumption an attestation must
   // carry more than N/3 of registered providers, so a Byzantine minority cannot on its
   // own attest an arbitrary (block_id, snapshot_hash) — and, combined with the disagreement reject
   // above, cannot win the race to quorum. Enforced independently of the governance-set
   // min_providers / threshold_pct (which could be misconfigured as low as 1, allowing a
   // single-provider attest).
   const uint32_t bft_floor = provider_count / 3 + 1;
   uint32_t quorum = std::max(std::max(cfg.min_providers,
                                       (provider_count * cfg.threshold_pct + 99) / 100),
                              bft_floor);

   if (voter_count >= quorum) {
      // Attestation reached -- create the record if it does not already exist.
      // Re-check existence here (rather than reusing the iterator from above) so the
      // decision does not depend on iterator liveness across the intervening votes writes.
      if (!records.contains(rec_key)) {
         uint32_t current_block = static_cast<uint32_t>(sysio::current_block_number());
         records.emplace(get_self(), rec_key, [&](auto& row) {
            row.block_num         = block_num;
            row.block_id          = block_id;
            row.snapshot_hash     = snapshot_hash;
            row.attested_at_block = current_block;
         });
      }

      // Purge old votes with block_num <= attested block_num
      auto purge_itr = by_bn.begin();
      while (purge_itr != by_bn.end() && purge_itr->block_num <= block_num) {
         purge_itr = by_bn.erase(std::move(purge_itr));
      }
   }
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::setsnpcfg(uint32_t min_providers, uint32_t threshold_pct) {
   require_auth(get_self());

   check(threshold_pct > 0 && threshold_pct <= 100, "threshold_pct must be between 1 and 100");
   check(min_providers > 0, "min_providers must be at least 1");
   // regsnapprov limits the provider table to `max_snap_provider_rank` mappings, so a
   // `min_providers` above that ceiling can never be
   // met and would make every attestation unreachable regardless of how many
   // providers register. (This bounds the config; a min_providers set higher than
   // the count actually registered at attestation time remains an operational
   // concern the quorum math cannot resolve here.)
   check(min_providers <= max_snap_provider_rank,
         "min_providers exceeds the maximum registrable providers");

   snap_config_singleton cfg_singleton(get_self());
   cfg_singleton.set(snap_config{min_providers, threshold_pct}, get_self());
}

// -------------------------------------------------------------------------------------------------
snap_record snapshot_attest::getsnaphash(uint32_t block_num) {
   snap_records_table records(get_self());
   auto rec_itr = records.require_find(snap_record_key_t{static_cast<uint64_t>(block_num)},
                                       "no attested snapshot record for this block number");
   return *rec_itr;
}

} // namespace sysiosystem
