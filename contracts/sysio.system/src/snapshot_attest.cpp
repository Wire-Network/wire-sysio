#include <sysio.system/sysio.system.hpp>
#include <sysio.system/snapshot_attest.hpp>
#include <sysio.system/block_utils.hpp>

#include <sysio/sysio.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace sysiosystem {

namespace {

namespace snapshot_protocol = sysio::protocol::snapshot_attestation;

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
constexpr char unscheduled_block_id_error[] = "snapshot block is not a scheduled attestation height";
constexpr char historical_block_id_error[] = "snapshot block is older than latest attested snapshot height";
constexpr char future_block_id_error[] = "snapshot block cannot be in the future";
constexpr char threshold_pct_range_error[] = "threshold_pct must be between 1 and 100";
constexpr char min_providers_range_error[] = "min_providers must be at least 1";
constexpr char min_providers_capacity_error[] = "min_providers exceeds the maximum registrable providers";
constexpr char snapshot_record_not_found_error[] = "no attested snapshot record for this block number";
constexpr uint32_t quorum_percentage_denominator = 100;
constexpr uint32_t byzantine_fault_denominator = 3;

/// Classifies the producer-table state used as the snapshot-provider registration gate.
enum class snapshot_producer_eligibility {
   eligible,
   inactive,
   rank_exceeds_maximum,
};

/// Returns the producer-table eligibility used only when a provider mapping is registered.
snapshot_producer_eligibility get_snapshot_producer_eligibility(const producer_info& producer) {
   if (!producer.active()) {
      return snapshot_producer_eligibility::inactive;
   }
   if (producer.rank > max_snap_provider_rank) {
      return snapshot_producer_eligibility::rank_exceeds_maximum;
   }
   return snapshot_producer_eligibility::eligible;
}

/// Requires the producer's current table state to permit snapshot-provider registration.
void require_snapshot_producer_eligibility(const producers_table& producers, name producer) {
   const auto prod_itr = producers.require_find(producer_key_t{producer.value}, producer_not_registered_error);
   const auto eligibility = get_snapshot_producer_eligibility(*prod_itr);
   check(eligibility != snapshot_producer_eligibility::inactive, producer_not_active_error);
   check(eligibility != snapshot_producer_eligibility::rank_exceeds_maximum, producer_rank_too_high_error);
}

/// Counts the provider mappings used to establish a new height's frozen quorum.
uint32_t count_snapshot_providers(const snap_providers_table& providers) {
   uint32_t provider_count = 0;
   for (auto provider_itr = providers.begin(); provider_itr != providers.end(); ++provider_itr) {
      ++provider_count;
   }
   return provider_count;
}

/// Removes stale mappings only when capacity would otherwise reject a new registration.
void prune_stale_snapshot_providers_if_full(name self, snap_providers_table& providers) {
   if (count_snapshot_providers(providers) < max_snap_providers) {
      return;
   }

   producers_table producers(self);
   auto            provider_itr = providers.begin();
   while (provider_itr != providers.end()) {
      const auto producer_itr = producers.try_get(producer_key_t{provider_itr->producer.value});
      if (!producer_itr
          || get_snapshot_producer_eligibility(*producer_itr) != snapshot_producer_eligibility::eligible) {
         provider_itr = providers.erase(std::move(provider_itr));
      } else {
         ++provider_itr;
      }
   }
}

/// Calculates the quorum frozen when the first vote for a scheduled height arrives.
uint32_t calculate_snapshot_quorum(uint32_t provider_count, const snap_config& cfg) {
   const uint32_t bft_floor = provider_count / byzantine_fault_denominator + 1;
   return std::max(
      std::max(cfg.min_providers,
               (provider_count * cfg.threshold_pct + quorum_percentage_denominator - 1)
                  / quorum_percentage_denominator),
      bft_floor);
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

/// Writes one final attestation and removes every pending tuple through its height.
void finalize_snapshot_vote(name self, uint32_t block_num, const checksum256& block_id,
                            const checksum256& snapshot_hash) {
   snap_records_table records(self);
   const uint32_t     current_block = static_cast<uint32_t>(sysio::current_block_number());
   records.emplace(self, snap_record_key_t{static_cast<uint64_t>(block_num)}, [&](auto& row) {
      row.block_num         = block_num;
      row.block_id          = block_id;
      row.snapshot_hash     = snapshot_hash;
      row.attested_at_block = current_block;
   });

   snap_votes_table votes(self);
   auto             by_block_num = votes.get_index<snapshot_index::by_block_num>();
   auto             purge_itr = by_block_num.begin();
   while (purge_itr != by_block_num.end() && purge_itr->block_num <= block_num) {
      purge_itr = by_block_num.erase(std::move(purge_itr));
   }
}

} // namespace

// -------------------------------------------------------------------------------------------------
void snapshot_attest::regsnapprov(name producer, name snap_account) {
   require_auth(producer);

   producers_table producers(get_self());
   require_snapshot_producer_eligibility(producers, producer);

   snap_providers_table providers(get_self());
   prune_stale_snapshot_providers_if_full(get_self(), providers);

   check(!providers.contains(snap_provider_key_t{snap_account.value}), provider_already_registered_error);
   auto by_producer = providers.get_index<snapshot_index::by_producer>();
   check(by_producer.find(producer.value) == by_producer.end(), producer_already_registered_error);
   check(count_snapshot_providers(providers) < max_snap_providers, provider_capacity_error);

   providers.emplace(producer, snap_provider_key_t{snap_account.value}, [&](auto& row) {
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

   snap_providers_table providers(get_self());
   auto provider_itr = providers.find(snap_provider_key_t{account.value});
   if (provider_itr != providers.end()) {
      require_auth(provider_itr->snap_account);
      providers.erase(std::move(provider_itr));
      return;
   }

   auto by_producer = providers.get_index<snapshot_index::by_producer>();
   auto producer_itr = by_producer.find(account.value);
   check(producer_itr != by_producer.end(), provider_or_producer_not_registered_error);
   by_producer.erase(std::move(producer_itr));
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash) {
   require_auth(snap_account);

   const uint32_t block_num = block_info::block_height_from_id(block_id);
   check(block_num > 0, invalid_block_id_error);
   check(snapshot_protocol::is_scheduled_block(block_num), unscheduled_block_id_error);
   check(block_num <= sysio::current_block_number(), future_block_id_error);

   snap_records_table records(get_self());
   const snap_record_key_t record_key{static_cast<uint64_t>(block_num)};
   if (matches_final_snapshot_record(records, record_key, block_id, snapshot_hash)) {
      return;
   }
   if (records.cbegin() != records.cend()) {
      const auto latest_record = --records.cend();
      check(block_num > latest_record->block_num, historical_block_id_error);
   }

   snap_providers_table providers(get_self());
   const auto provider_itr = providers.find(snap_provider_key_t{snap_account.value});
   check(provider_itr != providers.end(), provider_not_registered_error);
   const name producer = provider_itr->producer;

   snap_config_singleton config_singleton(get_self());
   const snap_config config = config_singleton.get_or_default(snap_config{});
   check(config.min_providers > 0, snapshot_config_unset_error);

   snap_votes_table votes(get_self());
   auto             by_block_num = votes.get_index<snapshot_index::by_block_num>();
   std::optional<uint32_t> frozen_quorum;
   std::optional<uint64_t> matching_vote_id;
   for (auto vote_itr = by_block_num.lower_bound(static_cast<uint64_t>(block_num));
        vote_itr != by_block_num.end() && vote_itr->block_num == block_num; ++vote_itr) {
      if (!frozen_quorum) {
         frozen_quorum = vote_itr->quorum;
      }

      if (std::find(vote_itr->voters.begin(), vote_itr->voters.end(), producer) != vote_itr->voters.end()) {
         check(vote_itr->block_id == block_id && vote_itr->snapshot_hash == snapshot_hash,
               vote_equivocation_error);
         return;
      }
      if (vote_itr->block_id == block_id && vote_itr->snapshot_hash == snapshot_hash) {
         matching_vote_id = vote_itr->id;
      }
   }

   if (!frozen_quorum) {
      const uint32_t provider_count = count_snapshot_providers(providers);
      check(provider_count >= config.min_providers, insufficient_provider_count_error);
      frozen_quorum = calculate_snapshot_quorum(provider_count, config);
   }

   uint32_t voter_count = 1;
   if (matching_vote_id) {
      const auto matching_vote = votes.get(snap_vote_key_t{*matching_vote_id});
      voter_count = static_cast<uint32_t>(matching_vote.voters.size()) + 1;
      votes.modify(same_payer, snap_vote_key_t{*matching_vote_id}, [&](auto& row) {
         row.voters.push_back(producer);
      });
   } else {
      const uint64_t new_id = votes.available_primary_key();
      votes.emplace(snap_account, snap_vote_key_t{new_id}, [&](auto& row) {
         row.id            = new_id;
         row.block_num     = block_num;
         row.block_id      = block_id;
         row.snapshot_hash = snapshot_hash;
         row.quorum        = *frozen_quorum;
         row.voters        = {producer};
      });
   }

   if (voter_count >= *frozen_quorum) {
      finalize_snapshot_vote(get_self(), block_num, block_id, snapshot_hash);
   }
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::setsnpcfg(uint32_t min_providers, uint32_t threshold_pct) {
   require_auth(get_self());

   check(threshold_pct > 0 && threshold_pct <= quorum_percentage_denominator,
         threshold_pct_range_error);
   check(min_providers > 0, min_providers_range_error);
   check(min_providers <= max_snap_providers, min_providers_capacity_error);

   snap_config_singleton config_singleton(get_self());
   config_singleton.set(snap_config{min_providers, threshold_pct}, get_self());
}

// -------------------------------------------------------------------------------------------------
snap_record snapshot_attest::getsnaphash(uint32_t block_num) {
   snap_records_table records(get_self());
   auto record_itr = records.require_find(
      snap_record_key_t{static_cast<uint64_t>(block_num)}, snapshot_record_not_found_error);
   return *record_itr;
}

} // namespace sysiosystem
