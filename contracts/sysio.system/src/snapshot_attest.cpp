#include <sysio.system/sysio.system.hpp>
#include <sysio.system/producer_score.hpp>
#include <sysio.system/snapshot_attest.hpp>
#include <sysio.system/block_utils.hpp>

#include <sysio/print.hpp>
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
constexpr char provider_capacity_error[] = "maximum registered snapshot providers reached";
constexpr char vote_equivocation_error[] = "producer already voted a different snapshot tuple for this height";
constexpr char snapshot_config_unset_error[] = "snapshot attestation configuration has not been set";
constexpr char invalid_block_id_error[] = "invalid block_id";
constexpr char unscheduled_block_id_error[] = "snapshot block is not a scheduled attestation height";
constexpr char historical_block_id_error[] = "snapshot block is older than latest attested snapshot height";
constexpr char future_block_id_error[] = "snapshot block cannot be in the future";
constexpr char min_providers_range_error[] = "min_providers must be at least 1";
constexpr char min_providers_capacity_error[] = "min_providers exceeds the maximum registrable providers";
constexpr char snapshot_record_not_found_error[] = "no attested snapshot record for this block number";
constexpr char stale_provider_prune_log_prefix[] = "regsnapprov: pruned stale mapping for producer ";
constexpr char stale_provider_prune_log_infix[] = " and snapshot provider ";
constexpr char log_line_ending[] = "\n";

/// Classifies the producer-table state used as the snapshot-provider registration gate.
enum class snapshot_producer_eligibility {
   eligible,
   inactive,
   rank_exceeds_maximum,
};

/// Returns the producer-table eligibility used only when a provider mapping is registered.
///
/// `rank` is no longer a stored field -- it is POSITION in the "prodrank" index among schedulable
/// producers. So the rank gate is a bounded walk of at most `max_snap_provider_rank` schedulable
/// entries, testing membership, rather than a point read. Counting matches (rather than taking the
/// first N index entries) is what stops unbonded registrants -- which occupy index slots but can
/// never be scheduled -- from crowding real producers out of snapshot-provider eligibility.
/// The producers holding rank positions 1..max_snap_provider_rank, in rank order.
///
/// Computed ONCE per caller and then tested for membership, rather than re-walked per producer:
/// the prune path checks up to max_snap_providers entries, and a per-entry walk would make that
/// quadratic.
std::vector<name> snapshot_ranked_producers(name self) {
   producers_table  producers(self);
   finalizers_table finalizers(self);

   std::vector<name> ranked;
   ranked.reserve(max_snap_provider_rank);

   auto idx = producers.get_index<"prodrank"_n>();
   for (auto i = idx.cbegin(); i != idx.cend() && ranked.size() < max_snap_provider_rank; ++i) {
      if (producer_rank::tier_of(i->rank_score) == producer_tier::demoted) break;
      if (!producer_rank::is_schedulable(*i, finalizers)) continue;
      ranked.push_back(i->owner);
   }
   return ranked;
}

/// Returns the producer-table eligibility used only when a provider mapping is registered.
snapshot_producer_eligibility get_snapshot_producer_eligibility(const producer_info& producer,
                                                               const std::vector<name>& ranked) {
   if (!producer.active()) {
      return snapshot_producer_eligibility::inactive;
   }
   if (std::find(ranked.begin(), ranked.end(), producer.owner) == ranked.end()) {
      return snapshot_producer_eligibility::rank_exceeds_maximum;
   }
   return snapshot_producer_eligibility::eligible;
}

/// Requires the producer's current table state to permit snapshot-provider registration.
void require_snapshot_producer_eligibility(name self, name producer) {
   producers_table producers(self);
   const auto prod_itr = producers.require_find(producer_key_t{producer.value}, producer_not_registered_error);
   const auto eligibility = get_snapshot_producer_eligibility(*prod_itr, snapshot_ranked_producers(self));
   check(eligibility != snapshot_producer_eligibility::inactive, producer_not_active_error);
   check(eligibility != snapshot_producer_eligibility::rank_exceeds_maximum, producer_rank_too_high_error);
}

/// Credit every producer whose vote contributed to a quorum-reaching snapshot record.
///
/// The vote rows -- the only place a per-producer voter list exists -- are PURGED once the record is
/// finalized, so without this counter there is no attestation history to score. Reset on the same
/// `payepoch` cadence as the block counters, which supplies the trailing window.
void credit_snapshot_attestations(name self, const std::vector<name>& voters) {
   producers_table producers(self);
   for (const auto& voter : voters) {
      auto key = producer_key_t{voter.value};
      if (!producers.contains(key)) continue;
      producers.modify(same_payer, key, [](auto& row) { row.snapshot_attestations++; });
      // The credit moved the snapshot factor, so the stored sort key is stale until rescored.
      // Without this the factor would reach the index only on the next unrelated rescore.
      producer_rank::rescore(self, producers, voter);
   }
}

/// Counts provider mappings for the bounded registration-capacity check.
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

   producers_table   producers(self);
   const auto        ranked = snapshot_ranked_producers(self);
   auto              provider_itr = providers.begin();
   while (provider_itr != providers.end()) {
      const auto producer_itr = producers.try_get(producer_key_t{provider_itr->producer.value});
      if (!producer_itr
          || get_snapshot_producer_eligibility(*producer_itr, ranked) != snapshot_producer_eligibility::eligible) {
         const name stale_producer = provider_itr->producer;
         const name stale_snap_account = provider_itr->snap_account;
         provider_itr = providers.erase(std::move(provider_itr));
         sysio::print(stale_provider_prune_log_prefix, stale_producer,
                      stale_provider_prune_log_infix, stale_snap_account, log_line_ending);
      } else {
         ++provider_itr;
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
   require_snapshot_producer_eligibility(get_self(), producer);

   snap_providers_table providers(get_self());
   const auto provider_itr = providers.find(snap_provider_key_t{snap_account.value});
   if (provider_itr != providers.end()) {
      check(provider_itr->producer == producer, provider_already_registered_error);
      return;
   }

   auto by_producer = providers.get_index<snapshot_index::by_producer>();
   auto producer_itr = by_producer.find(producer.value);
   if (producer_itr != by_producer.end()) {
      by_producer.erase(std::move(producer_itr));
   } else {
      prune_stale_snapshot_providers_if_full(get_self(), providers);
   }
   check(count_snapshot_providers(providers) < max_snap_providers, provider_capacity_error);

   providers.emplace(producer, snap_provider_key_t{snap_account.value}, [&](auto& row) {
      row.snap_account = snap_account;
      row.producer     = producer;
   });
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
   std::optional<uint64_t> matching_vote_id;
   uint32_t                voter_count = 0;
   bool                    exact_retry = false;
   std::vector<name>       quorum_voters;
   for (auto vote_itr = by_block_num.lower_bound(static_cast<uint64_t>(block_num));
        vote_itr != by_block_num.end() && vote_itr->block_num == block_num; ++vote_itr) {
      if (std::find(vote_itr->voters.begin(), vote_itr->voters.end(), producer) != vote_itr->voters.end()) {
         check(vote_itr->block_id == block_id && vote_itr->snapshot_hash == snapshot_hash,
               vote_equivocation_error);
         voter_count   = static_cast<uint32_t>(vote_itr->voters.size());
         quorum_voters = vote_itr->voters;
         exact_retry   = true;
         break;
      }
      if (vote_itr->block_id == block_id && vote_itr->snapshot_hash == snapshot_hash) {
         matching_vote_id = vote_itr->id;
      }
   }

   if (exact_retry) {
      if (voter_count >= config.min_providers) {
         credit_snapshot_attestations(get_self(), quorum_voters);
         finalize_snapshot_vote(get_self(), block_num, block_id, snapshot_hash);
      }
      return;
   }

   voter_count = 1;
   if (matching_vote_id) {
      const auto matching_vote = votes.get(snap_vote_key_t{*matching_vote_id});
      voter_count = static_cast<uint32_t>(matching_vote.voters.size()) + 1;
      votes.modify(same_payer, snap_vote_key_t{*matching_vote_id}, [&](auto& row) {
         row.voters.push_back(producer);
         quorum_voters = row.voters;
      });
   } else {
      const uint64_t new_id = votes.available_primary_key();
      votes.emplace(snap_account, snap_vote_key_t{new_id}, [&](auto& row) {
         row.id            = new_id;
         row.block_num     = block_num;
         row.block_id      = block_id;
         row.snapshot_hash = snapshot_hash;
         row.voters        = {producer};
         quorum_voters     = row.voters;
      });
   }

   if (voter_count >= config.min_providers) {
      credit_snapshot_attestations(get_self(), quorum_voters);
      finalize_snapshot_vote(get_self(), block_num, block_id, snapshot_hash);
   }
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::setsnpcfg(uint32_t min_providers) {
   require_auth(get_self());

   check(min_providers > 0, min_providers_range_error);
   check(min_providers <= max_snap_providers, min_providers_capacity_error);

   snap_config_singleton config_singleton(get_self());
   config_singleton.set(snap_config{min_providers}, get_self());
}

// -------------------------------------------------------------------------------------------------
snap_record snapshot_attest::getsnaphash(uint32_t block_num) {
   snap_records_table records(get_self());
   auto record_itr = records.require_find(
      snap_record_key_t{static_cast<uint64_t>(block_num)}, snapshot_record_not_found_error);
   return *record_itr;
}

} // namespace sysiosystem
