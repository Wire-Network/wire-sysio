#include <sysio.system/sysio.system.hpp>
#include <sysio.system/producer_score.hpp>
#include <sysio.system/snapshot_attest.hpp>
#include <sysio.system/block_utils.hpp>

#include <sysio/sysio.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace sysiosystem {

namespace {

constexpr char producer_rank_too_high_error[] = "producer rank exceeds maximum for snapshot providers";
constexpr char producer_no_live_standing_error[] = "producer does not meet the live PRODUCER operator requirements";
constexpr char vote_equivocation_error[] = "producer already voted a different snapshot tuple for this height";
constexpr char snapshot_config_unset_error[] = "snapshot attestation configuration has not been set";
constexpr char invalid_block_id_error[] = "invalid block_id";
constexpr char unscheduled_block_id_error[] = "snapshot block is not a scheduled attestation height";
constexpr char historical_block_id_error[] = "snapshot block is older than latest attested snapshot height";
constexpr char stale_block_id_error[] = "snapshot block is older than the pending vote window";
constexpr char future_block_id_error[] = "snapshot block cannot be in the future";
constexpr char min_providers_range_error[] = "min_providers must be at least 1";
constexpr char min_providers_capacity_error[] = "min_providers exceeds the number of producers that can vote";
constexpr char block_spacing_range_error[] = "block_spacing must be greater than 0";
constexpr char snapshot_record_not_found_error[] = "no attested snapshot record for this block number";

/// Stale vote rows one vote may erase: a full window, so a steady cadence clears its backlog in one call while
/// rows a cadence change strands drain over later votes.
constexpr uint32_t max_stale_vote_purge_rows = max_snap_providers * snapshot_attestation::max_pending_vote_heights;

/// Whether `producer` holds one of the first max_snap_provider_rank positions among schedulable producers.
///
/// Rank is POSITION in the "prodrank" index among schedulable producers, so this walks the index, counting only
/// schedulable rows: an active producer row, an ACTIVE PRODUCER operator and an active finalizer key. Counting
/// matches, rather than taking the first N entries, stops rows whose live standing lapsed from crowding out eligible
/// producers while their cached rank keys converge. Returns as soon as the producer is found.
bool is_ranked_snapshot_producer(name self, name producer) {
   producers_table  producers(self);
   finalizers_table finalizers(self);

   // Bounded on ROWS EXAMINED, not just on positions. `rank_score` is a CACHE while `is_schedulable` is evaluated
   // LIVE, so rows that no longer qualify are skipped and would otherwise cost an unbounded scan, at a cross-contract
   // sysio.opreg read plus a finalizer read apiece.
   uint32_t position = 0;
   uint32_t examined = 0;
   auto     idx      = producers.get_index<"prodrank"_n>();
   for (auto i = idx.cbegin(); i != idx.cend() && position < max_snap_provider_rank; ++i) {
      if (producer_rank::tier_of(i->rank_score) == producer_tier::demoted) break;
      if (++examined > max_rank_walk_rows) break;
      if (!producer_rank::is_schedulable(*i, finalizers)) continue;
      if (i->owner == producer) return true;
      ++position;
   }
   return false;
}

/// Credit every producer whose vote contributed to a quorum-reaching snapshot record.
///
/// The vote rows -- the only place a per-producer voter list exists -- are PURGED once the record is
/// finalized, so without this counter there is no attestation history to score. Reset on the same
/// `payepoch` cadence as the block counters, which supplies the trailing window.
void credit_snapshot_attestations(name self, const std::vector<name>& voters) {
   producers_table producers(self);
   const uint32_t  current_period = producer_rank::current_pay_period(self);
   for (const auto& voter : voters) {
      auto key = producer_key_t{voter.value};
      if (!producers.contains(key)) continue;
      // Stamped with its pay period; `compute` ignores a stale one. No exit has to consume it.
      producers.modify(same_payer, key, [&](auto& row) {
         if (row.snapshot_period != current_period) {
            row.snapshot_period       = current_period;
            row.snapshot_attestations = 0;
         }
         row.snapshot_attestations++;
      });
      // The credit moved the snapshot factor, so the stored sort key is stale until rescored.
      // Without this the factor would reach the index only on the next unrelated rescore.
      producer_rank::rescore(self, producers, voter);
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

/// Erases pending rows below the window, at most max_stale_vote_purge_rows per call.
void purge_stale_snapshot_votes(snap_votes_table& votes, uint64_t pending_floor) {
   auto     by_block_num = votes.get_index<snapshot_index::by_block_num>();
   auto     purge_itr    = by_block_num.begin();
   uint32_t purged_rows  = 0;
   while (purge_itr != by_block_num.end() && purge_itr->block_num < pending_floor
          && purged_rows < max_stale_vote_purge_rows) {
      purge_itr = by_block_num.erase(std::move(purge_itr));
      ++purged_rows;
   }
}

/// Writes one final attestation and removes the pending tuples from the window floor through its height.
///
/// Starts at the floor so the work stays bounded by the window; anything below it is the stale purge's.
void finalize_snapshot_vote(name self, uint32_t block_num, const checksum256& block_id,
                            const checksum256& snapshot_hash, uint64_t pending_floor) {
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
   auto             purge_itr = by_block_num.lower_bound(pending_floor);
   while (purge_itr != by_block_num.end() && purge_itr->block_num <= block_num) {
      purge_itr = by_block_num.erase(std::move(purge_itr));
   }
}

} // namespace

// -------------------------------------------------------------------------------------------------
void snapshot_attest::votesnaphash(name producer, checksum256 block_id, checksum256 snapshot_hash) {
   require_auth(producer);

   const uint32_t block_num = block_info::block_height_from_id(block_id);
   check(block_num > 0, invalid_block_id_error);

   snap_config_singleton config_singleton(get_self());
   const snap_config config = config_singleton.get_or_default(snap_config{});
   check(config.min_providers > 0, snapshot_config_unset_error);
   check(snapshot_attestation::is_scheduled_block(block_num, config.block_spacing),
         unscheduled_block_id_error);
   const uint32_t current_block = static_cast<uint32_t>(sysio::current_block_number());
   check(block_num <= current_block, future_block_id_error);

   snap_records_table records(get_self());
   const snap_record_key_t record_key{static_cast<uint64_t>(block_num)};
   // Matched before the rank check: a provider node shuts down on snap_hash_disagreement_error, so a
   // mismatch is reported whether or not the producer can still vote.
   const bool already_final = matches_final_snapshot_record(records, record_key, block_id, snapshot_hash);

   // Checked before the already-final return, so only a top-ranked producer's late vote succeeds.
   check(is_ranked_snapshot_producer(get_self(), producer), producer_rank_too_high_error);
   // A late vote cannot count toward a quorum, so it skips the standing check below.
   if (already_final) {
      return;
   }
   if (records.cbegin() != records.cend()) {
      const auto latest_record = --records.cend();
      check(block_num > latest_record->block_num, historical_block_id_error);
   }
   // sysio pays for vote rows, so heights older than the window are closed: that bounds what a withheld quorum leaves.
   const uint64_t pending_floor = snapshot_attestation::pending_vote_floor(current_block, config.block_spacing);
   check(block_num >= pending_floor, stale_block_id_error);

   // Rank does not see a raised collateral minimum, because sysio.opreg::setconfig re-evaluates no stored status, so
   // each counting vote also measures the bond against the live minimum, bootstrapped exempt.
   check(producer_rank::has_live_producer_standing(producer), producer_no_live_standing_error);

   snap_votes_table votes(get_self());
   purge_stale_snapshot_votes(votes, pending_floor);
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
         finalize_snapshot_vote(get_self(), block_num, block_id, snapshot_hash, pending_floor);
      }
      return;
   }

   voter_count = 1;
   if (matching_vote_id) {
      const auto matching_vote = votes.get(snap_vote_key_t{*matching_vote_id});
      voter_count = static_cast<uint32_t>(matching_vote.voters.size()) + 1;
      // Billed to sysio, not to whoever opened the row.
      votes.modify(get_self(), snap_vote_key_t{*matching_vote_id}, [&](auto& row) {
         row.voters.push_back(producer);
         quorum_voters = row.voters;
      });
   } else {
      const uint64_t new_id = votes.available_primary_key();
      votes.emplace(get_self(), snap_vote_key_t{new_id}, [&](auto& row) {
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
      finalize_snapshot_vote(get_self(), block_num, block_id, snapshot_hash, pending_floor);
   }
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::setsnpcfg(uint32_t min_providers, uint32_t block_spacing) {
   require_auth(get_self());

   check(min_providers > 0, min_providers_range_error);
   check(min_providers <= max_snap_providers, min_providers_capacity_error);
   check(snapshot_attestation::is_valid_block_spacing(block_spacing), block_spacing_range_error);

   snap_config_singleton config_singleton(get_self());
   config_singleton.set(snap_config{min_providers, block_spacing}, get_self());
}

// -------------------------------------------------------------------------------------------------
snap_record snapshot_attest::getsnaphash(uint32_t block_num) {
   snap_records_table records(get_self());
   auto record_itr = records.require_find(
      snap_record_key_t{static_cast<uint64_t>(block_num)}, snapshot_record_not_found_error);
   return *record_itr;
}

} // namespace sysiosystem
