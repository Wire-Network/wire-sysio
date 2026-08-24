
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
constexpr char provider_not_registered_error[] = "snap_account is not a registered snapshot provider";
constexpr char provider_already_registered_error[] = "snap_account is already registered as a provider";
constexpr char producer_already_registered_error[] = "producer already has a registered snapshot provider";
constexpr char provider_capacity_error[] = "maximum registered snapshot providers reached";
constexpr char vote_equivocation_error[] = "producer already voted a different snapshot tuple for this height";
constexpr char snapshot_config_unset_error[] = "snapshot attestation configuration has not been set";
constexpr char insufficient_provider_count_error[] = "registered snapshot providers are below min_providers";
constexpr char provider_or_producer_not_registered_error[] =
   "account is not registered as a snapshot provider or producer";
constexpr char invalid_block_id_error[] = "invalid block_id";
constexpr char future_block_id_error[] = "snapshot block cannot be in the future";
constexpr char threshold_pct_range_error[] = "threshold_pct must be between 1 and 100";
constexpr char min_providers_range_error[] = "min_providers must be at least 1";
constexpr char min_providers_capacity_error[] = "min_providers exceeds the maximum registrable providers";
constexpr char snapshot_record_not_found_error[] = "no attested snapshot record for this block number";
constexpr uint32_t quorum_percentage_denominator = 100;
constexpr uint32_t byzantine_fault_denominator = 3;

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

/// Counts the current live provider mappings used by both sides of the quorum calculation.
uint32_t count_snapshot_providers(const snap_providers_table& providers) {
   uint32_t provider_count = 0;
   for (auto provider_itr = providers.begin(); provider_itr != providers.end(); ++provider_itr) {
      ++provider_count;
   }
   return provider_count;
}

/// Calculates the quorum required from the current live provider set.
uint32_t calculate_snapshot_quorum(uint32_t provider_count, const snap_config& cfg) {
   // The Byzantine floor remains independent of governance configuration so a low percentage
   // cannot allow a minority of the same live registration set to finalize a tuple.
   const uint32_t bft_floor = provider_count / byzantine_fault_denominator + 1;
   return std::max(
      std::max(cfg.min_providers,
               (provider_count * cfg.threshold_pct + quorum_percentage_denominator - 1)
                  / quorum_percentage_denominator),
      bft_floor);
}

/// Counts the voters in one pending tuple that remain in the current live provider set.
uint32_t count_current_snapshot_voters(snap_providers_table& providers, const std::vector<name>& voters) {
   const auto by_producer = providers.get_index<"byproducer"_n>();
   uint32_t voter_count = 0;
   for (const auto& voter : voters) {
      if (by_producer.find(voter.value) != by_producer.end()) {
         ++voter_count;
      }
   }
   return voter_count;
}

/// Finalizes every deterministically ordered pending tuple that meets the current live-set quorum.
void finalize_eligible_snapshot_votes(name self) {
   snap_config_singleton cfg_singleton(self);
   const snap_config cfg = cfg_singleton.get_or_default(snap_config{});
   if (cfg.min_providers == 0) {
      return;
   }

   snap_providers_table providers(self);
   const uint32_t       provider_count = count_snapshot_providers(providers);
   if (provider_count < cfg.min_providers) {
      return;
   }
   const uint32_t quorum = calculate_snapshot_quorum(provider_count, cfg);

   snap_records_table records(self);
   snap_votes_table   votes(self);
   auto               by_block_num = votes.get_index<"byblocknum"_n>();
   while (true) {
      bool        found_candidate = false;
      uint32_t    candidate_block_num = 0;
      checksum256 candidate_block_id;
      checksum256 candidate_snapshot_hash;

      for (auto vote_itr = by_block_num.begin(); vote_itr != by_block_num.end(); ++vote_itr) {
         if (count_current_snapshot_voters(providers, vote_itr->voters) < quorum) {
            continue;
         }
         found_candidate         = true;
         candidate_block_num     = vote_itr->block_num;
         candidate_block_id      = vote_itr->block_id;
         candidate_snapshot_hash = vote_itr->snapshot_hash;
         break;
      }
      if (!found_candidate) {
         return;
      }

      const snap_record_key_t record_key{static_cast<uint64_t>(candidate_block_num)};
      const auto              record_itr = records.find(record_key);
      if (record_itr == records.end()) {
         const uint32_t current_block = static_cast<uint32_t>(sysio::current_block_number());
         records.emplace(self, record_key, [&](auto& row) {
            row.block_num         = candidate_block_num;
            row.block_id          = candidate_block_id;
            row.snapshot_hash     = candidate_snapshot_hash;
            row.attested_at_block = current_block;
         });
      } else {
         check(record_itr->block_id == candidate_block_id
                  && record_itr->snapshot_hash == candidate_snapshot_hash,
               snap_hash_disagreement_error);
      }

      auto purge_itr = by_block_num.begin();
      while (purge_itr != by_block_num.end() && purge_itr->block_num <= candidate_block_num) {
         purge_itr = by_block_num.erase(std::move(purge_itr));
      }
   }
}

/// Returns true for an existing matching final record and rejects a conflicting final tuple.
bool matches_final_snapshot_record(snap_records_table& records, snap_record_key_t record_key,
                                   const checksum256& block_id, const checksum256& snapshot_hash) {
   const auto record_itr = records.find(record_key);
   if (record_itr == records.end()) {
      return false;
   }
   check(record_itr->snapshot_hash == snapshot_hash && record_itr->block_id == block_id,
         snap_hash_disagreement_error);
   return true;
}

/// Remove selected producers from every pending tuple, erasing tuples that become empty.
void remove_snapshot_producer_votes(name self, const std::vector<name>& removed_producers) {
   snap_votes_table votes(self);
   auto             vote_itr = votes.begin();
   while (vote_itr != votes.end()) {
      const auto vote_id = vote_itr->id;
      const bool has_removed_voter = std::any_of(vote_itr->voters.begin(), vote_itr->voters.end(), [&](name voter) {
         return std::find(removed_producers.begin(), removed_producers.end(), voter) != removed_producers.end();
      });
      ++vote_itr;
      if (!has_removed_voter) {
         continue;
      }

      const auto current_vote = votes.get(snap_vote_key_t{vote_id});
      const auto retained_voter_count = static_cast<uint32_t>(std::count_if(
         current_vote.voters.begin(), current_vote.voters.end(), [&](name voter) {
            return std::find(removed_producers.begin(), removed_producers.end(), voter) == removed_producers.end();
         }));
      if (retained_voter_count == 0) {
         auto erase_itr = votes.find(snap_vote_key_t{vote_id});
         votes.erase(std::move(erase_itr));
      } else {
         votes.modify(same_payer, snap_vote_key_t{vote_id}, [&](auto& row) {
            row.voters.erase(std::remove_if(row.voters.begin(), row.voters.end(), [&](name voter) {
               return std::find(removed_producers.begin(), removed_producers.end(), voter)
                      != removed_producers.end();
            }), row.voters.end());
         });
      }
   }
   finalize_eligible_snapshot_votes(self);
}

} // namespace

// -------------------------------------------------------------------------------------------------
void reconcile_snapshot_registrations(name self) {
   snap_providers_table providers(self);
   producers_table      producers(self);
   std::vector<name>    removed_producers;

   auto provider_itr = providers.begin();
   while (provider_itr != providers.end()) {
      const auto producer_itr = producers.try_get(producer_key_t{provider_itr->producer.value});
      if (!producer_itr
          || get_snapshot_producer_eligibility(*producer_itr) != snapshot_producer_eligibility::eligible) {
         removed_producers.push_back(provider_itr->producer);
         provider_itr = providers.erase(std::move(provider_itr));
      } else {
         ++provider_itr;
      }
   }

   if (!removed_producers.empty()) {
      remove_snapshot_producer_votes(self, removed_producers);
   }
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::regsnapprov(name producer, name snap_account) {
   require_auth(producer);

   reconcile_snapshot_registrations(get_self());

   producers_table producers(get_self());
   require_snapshot_producer_eligibility(producers, producer);

   snap_providers_table provs(get_self());
   check(!provs.contains(snap_provider_key_t{snap_account.value}), provider_already_registered_error);

   auto by_prod = provs.get_index<"byproducer"_n>();
   check(by_prod.find(producer.value) == by_prod.end(), producer_already_registered_error);
   check(count_snapshot_providers(provs) < max_snap_provider_rank, provider_capacity_error);

   provs.emplace(producer, snap_provider_key_t{snap_account.value}, [&](auto& row) {
      row.snap_account = snap_account;
      row.producer     = producer;
   });
}

// -------------------------------------------------------------------------------------------------
// The `account` parameter is overloaded: it can be either a snap_account (primary key lookup)
// or a producer (secondary index lookup). The primary key path takes precedence.
// This means a producer can unregister their own provider, and a snap_account can unregister itself.
// If one account occupies both roles in different mappings, the first call removes its primary-key
// snap_account mapping and a repeated call removes its producer-keyed delegation.
void snapshot_attest::delsnapprov(name account) {
   require_auth(account);

   snap_providers_table provs(get_self());

   // First try lookup as snap_account (primary key)
   auto prov_itr = provs.find(snap_provider_key_t{account.value});
   if (prov_itr != provs.end()) {
      require_auth(prov_itr->snap_account);
      const name producer = prov_itr->producer;
      provs.erase(std::move(prov_itr));
      remove_snapshot_producer_votes(get_self(), {producer});
      return;
   }

   // Then try lookup as producer (secondary index)
   auto by_prod = provs.get_index<"byproducer"_n>();
   auto prod_itr = by_prod.find(account.value);
   check(prod_itr != by_prod.end(), provider_or_producer_not_registered_error);
   const name producer = prod_itr->producer;
   by_prod.erase(std::move(prod_itr));
   remove_snapshot_producer_votes(get_self(), {producer});
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash) {
   require_auth(snap_account);

   const uint32_t block_num = block_info::block_height_from_id(block_id);
   check(block_num > 0, invalid_block_id_error);
   check(block_num <= sysio::current_block_number(), future_block_id_error);

   snap_records_table records(get_self());
   snap_record_key_t  rec_key{static_cast<uint64_t>(block_num)};
   if (matches_final_snapshot_record(records, rec_key, block_id, snapshot_hash)) {
      return;
   }

   // The final-record disagreement check intentionally precedes authorization so a node with a stale
   // registration still receives the fatal disagreement signal for a conflicting local snapshot.
   reconcile_snapshot_registrations(get_self());
   if (matches_final_snapshot_record(records, rec_key, block_id, snapshot_hash)) {
      return;
   }

   snap_providers_table provs(get_self());
   const auto prov_itr = provs.find(snap_provider_key_t{snap_account.value});
   check(prov_itr != provs.end(), provider_not_registered_error);
   const name producer = prov_itr->producer;

   producers_table producers(get_self());
   require_snapshot_producer_eligibility(producers, producer);

   snap_votes_table votes(get_self());
   auto by_bn = votes.get_index<"byblocknum"_n>();

   snap_config_singleton cfg_singleton(get_self());
   const snap_config cfg = cfg_singleton.get_or_default(snap_config{});
   check(cfg.min_providers > 0, snapshot_config_unset_error);

   const uint32_t provider_count = count_snapshot_providers(provs);
   check(provider_count >= cfg.min_providers, insufficient_provider_count_error);

   uint64_t vote_id           = 0;
   bool     found             = false;
   bool     append_vote       = false;
   uint64_t matching_vote_id  = 0;
   bool     has_matching_vote = false;
   std::vector<uint64_t> superseded_vote_ids;
   for (auto itr = votes.begin(); itr != votes.end(); ++itr) {
      if (itr->block_num == block_num) {
         continue;
      }
      if (std::find(itr->voters.begin(), itr->voters.end(), producer) != itr->voters.end()) {
         superseded_vote_ids.push_back(itr->id);
      }
   }
   for (auto itr = by_bn.lower_bound(static_cast<uint64_t>(block_num));
        itr != by_bn.end() && itr->block_num == block_num; ++itr) {
      for (const auto& voter : itr->voters) {
         if (voter == producer) {
            check(itr->block_id == block_id && itr->snapshot_hash == snapshot_hash,
                  vote_equivocation_error);
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

   // A producer carries at most one pending vote across all heights. Moving to another height
   // removes only that producer's old weight, so one Byzantine provider cannot globally advance
   // the pending set or erase honest votes. With at most 30 producers, both rows and voters remain
   // bounded by the live registration cap.
   for (const auto superseded_vote_id : superseded_vote_ids) {
      const auto superseded_vote = votes.get(snap_vote_key_t{superseded_vote_id});
      if (superseded_vote.voters.size() == 1) {
         auto superseded_itr = votes.find(snap_vote_key_t{superseded_vote_id});
         votes.erase(std::move(superseded_itr));
      } else {
         votes.modify(same_payer, snap_vote_key_t{superseded_vote_id}, [&](auto& row) {
            row.voters.erase(std::remove(row.voters.begin(), row.voters.end(), producer), row.voters.end());
         });
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

   finalize_eligible_snapshot_votes(get_self());
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::setsnpcfg(uint32_t min_providers, uint32_t threshold_pct) {
   require_auth(get_self());

   check(threshold_pct > 0 && threshold_pct <= quorum_percentage_denominator,
         threshold_pct_range_error);
   check(min_providers > 0, min_providers_range_error);
   // regsnapprov limits the provider table to `max_snap_provider_rank` mappings, so a
   // `min_providers` above that ceiling can never be met. votesnaphash separately rejects
   // attestation while the current live set is smaller than the configured floor.
   check(min_providers <= max_snap_provider_rank, min_providers_capacity_error);

   snap_config_singleton cfg_singleton(get_self());
   cfg_singleton.set(snap_config{min_providers, threshold_pct}, get_self());
   finalize_eligible_snapshot_votes(get_self());
}

// -------------------------------------------------------------------------------------------------
snap_record snapshot_attest::getsnaphash(uint32_t block_num) {
   snap_records_table records(get_self());
   auto rec_itr = records.require_find(
      snap_record_key_t{static_cast<uint64_t>(block_num)}, snapshot_record_not_found_error);
   return *rec_itr;
}

} // namespace sysiosystem
