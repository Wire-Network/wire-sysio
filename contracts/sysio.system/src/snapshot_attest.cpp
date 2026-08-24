#include <sysio.system/sysio.system.hpp>
#include <sysio.system/snapshot_attest.hpp>
#include <sysio.system/block_utils.hpp>

#include <sysio/sysio.hpp>

#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

namespace sysiosystem {

namespace {

constexpr char producer_not_registered_error[] = "producer is not registered";
constexpr char producer_not_active_error[] = "producer is not active";
constexpr char producer_rank_too_high_error[] = "producer rank exceeds maximum for snapshot providers";
constexpr char provider_not_registered_error[] = "snap_account is not a registered snapshot provider candidate";
constexpr char roster_membership_error[] = "snap_account is not a member of the active snapshot roster";
constexpr char roster_version_error[] = "snapshot roster version mismatch";
constexpr char vote_tuple_error[] = "pending snapshot vote does not match the expected tuple";
constexpr char equivocation_error[] =
   "producer already voted a different snapshot tuple for this height and roster version";
constexpr char invalid_block_id_error[] = "invalid block_id";
constexpr char future_block_id_error[] = "snapshot block cannot be in the future";
constexpr char pending_vote_not_found_error[] = "pending snapshot vote not found";
constexpr char roster_count_mismatch_error[] = "snapshot roster state count mismatch";
constexpr char roster_digest_mismatch_error[] = "snapshot roster state digest mismatch";
constexpr char governance_excluded_voter_error[] =
   "snapshot provider is excluded from the pending governance roster";
constexpr char duplicate_provider_error[] = "snap_account is already registered as a provider candidate";
constexpr char duplicate_producer_error[] = "producer already has a registered snapshot provider candidate";
constexpr char candidate_rank_error[] =
   "snapshot provider candidate does not rank within the maximum roster size";
constexpr char missing_registration_error[] =
   "account is not registered as a snapshot provider candidate or producer";
constexpr char roster_not_ready_error[] = "snapshot roster is not ready for activation";
constexpr char roster_not_bootstrapped_error[] = "cannot propose a snapshot roster before bootstrap activation";
constexpr char proposal_below_minimum_error[] = "proposed snapshot roster is below min_providers";
constexpr char proposal_not_shrinking_error[] = "governance roster proposal must shrink the active roster";
constexpr char invalid_proposal_members_error[] =
   "proposed snapshot roster contains an invalid, duplicate, or ineligible member";
constexpr char missing_proposal_error[] = "no snapshot roster proposal exists";
constexpr char threshold_range_error[] = "threshold_pct must be between 1 and 100";
constexpr char minimum_provider_error[] = "min_providers must be at least 1";
constexpr char minimum_provider_max_error[] = "min_providers exceeds the maximum snapshot roster size";
constexpr char missing_record_error[] = "no attested snapshot record for this block number";

/// Divisor used by the active-roster Byzantine quorum floor.
constexpr uint32_t byzantine_quorum_divisor = 3;

/// Increment that makes the Byzantine quorum floor strictly greater than one third.
constexpr uint32_t byzantine_quorum_increment = 1;

/// Whole-percentage scale used by threshold configuration and quorum rounding.
constexpr uint32_t percentage_scale = 100;

/// Classifies the producer-table state that determines snapshot-provider eligibility.
enum class snapshot_producer_eligibility {
   eligible,
   inactive,
   rank_exceeds_maximum,
};

/// Couples a registration with the current producer rank used for candidate selection.
struct ranked_registration {
   snap_roster_member member;
   uint32_t           rank;
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
producer_info require_snapshot_producer_eligibility(const producers_table& producers, name producer) {
   const auto prod_itr = producers.require_find(producer_key_t{producer.value}, producer_not_registered_error);
   const auto eligibility = get_snapshot_producer_eligibility(*prod_itr);
   check(eligibility != snapshot_producer_eligibility::inactive, producer_not_active_error);
   check(eligibility != snapshot_producer_eligibility::rank_exceeds_maximum, producer_rank_too_high_error);
   return *prod_itr;
}

/// Returns whether a producer exists and is currently eligible for snapshot membership.
bool is_currently_eligible_snapshot_producer(const producers_table& producers, name producer) {
   const auto producer_row = producers.try_get(producer_key_t{producer.value});
   return producer_row
          && get_snapshot_producer_eligibility(*producer_row) == snapshot_producer_eligibility::eligible;
}

/// Orders registration candidates by producer rank and then producer account.
bool candidate_precedes(const ranked_registration& lhs, const ranked_registration& rhs) {
   return std::tie(lhs.rank, lhs.member.producer.value) < std::tie(rhs.rank, rhs.member.producer.value);
}

/// Returns members in the canonical producer/account order used for roster digests.
std::vector<snap_roster_member> canonicalize_members(std::vector<snap_roster_member> members) {
   std::sort(members.begin(), members.end(), [](const auto& lhs, const auto& rhs) {
      return std::tie(lhs.producer.value, lhs.snap_account.value)
             < std::tie(rhs.producer.value, rhs.snap_account.value);
   });
   return members;
}

/// Hashes the canonical packed roster representation.
checksum256 digest_members(const std::vector<snap_roster_member>& members) {
   const auto canonical_members = canonicalize_members(members);
   const auto packed = sysio::pack(canonical_members);
   return sysio::sha256(packed.data(), packed.size());
}

/// Reads all currently retained registration candidates in deterministic selection order.
std::vector<ranked_registration> collect_ranked_registrations(name self) {
   snap_registrations_table registrations(self);
   producers_table          producers(self);
   std::vector<ranked_registration> result;
   result.reserve(max_snap_roster_size);

   for (auto itr = registrations.begin(); itr != registrations.end(); ++itr) {
      const auto producer = producers.try_get(producer_key_t{itr->producer.value});
      if (producer
          && get_snapshot_producer_eligibility(*producer) == snapshot_producer_eligibility::eligible) {
         result.push_back({snap_roster_member{itr->snap_account, itr->producer}, producer->rank});
      }
   }
   std::sort(result.begin(), result.end(), candidate_precedes);
   return result;
}

/// Projects ranked registration candidates to the active-roster row type.
std::vector<snap_roster_member> collect_candidate_members(name self) {
   const auto ranked = collect_ranked_registrations(self);
   std::vector<snap_roster_member> members;
   members.reserve(ranked.size());
   for (const auto& registration : ranked) {
      members.push_back(registration.member);
   }
   return members;
}

/// Returns all active roster rows in canonical digest order.
std::vector<snap_roster_member> collect_active_members(name self) {
   snap_roster_table roster(self);
   std::vector<snap_roster_member> members;
   members.reserve(max_snap_roster_size);
   for (auto itr = roster.begin(); itr != roster.end(); ++itr) {
      members.push_back(*itr);
   }
   return canonicalize_members(std::move(members));
}

/// Returns whether an exact producer/provider pair belongs to the supplied roster.
bool contains_member(const std::vector<snap_roster_member>& members, name producer, name snap_account) {
   return std::find_if(members.begin(), members.end(), [&](const auto& member) {
      return member.producer == producer && member.snap_account == snap_account;
   }) != members.end();
}

/// Validates uniqueness, current registration, and eligibility for every proposed roster member.
bool validate_roster_members(name self, const std::vector<snap_roster_member>& members) {
   if (members.empty() || members.size() > max_snap_roster_size) {
      return false;
   }

   snap_registrations_table registrations(self);
   producers_table producers(self);
   std::vector<name> seen_producers;
   std::vector<name> seen_accounts;
   seen_producers.reserve(members.size());
   seen_accounts.reserve(members.size());

   for (const auto& member : members) {
      if (std::find(seen_producers.begin(), seen_producers.end(), member.producer) != seen_producers.end()
          || std::find(seen_accounts.begin(), seen_accounts.end(), member.snap_account) != seen_accounts.end()) {
         return false;
      }
      const auto registration = registrations.try_get(snap_account_key_t{member.snap_account.value});
      if (!registration || registration->producer != member.producer
          || !is_currently_eligible_snapshot_producer(producers, member.producer)) {
         return false;
      }
      seen_producers.push_back(member.producer);
      seen_accounts.push_back(member.snap_account);
   }
   return true;
}

/// Returns whether the height already has any pending vote, which closes its activation boundary.
bool has_pending_vote_for_height(snap_votes_table& votes, uint32_t block_num) {
   auto by_block = votes.get_index<"byblocknum"_n>();
   const auto itr = by_block.lower_bound(static_cast<uint64_t>(block_num));
   return itr != by_block.end() && itr->block_num == block_num;
}

/// Atomically replaces the complete active roster and advances its version.
snap_roster_state activate_roster(name self,
                                  const std::vector<snap_roster_member>& members,
                                  snap_roster_state state) {
   snap_roster_table roster(self);
   auto itr = roster.begin();
   while (itr != roster.end()) {
      itr = roster.erase(std::move(itr));
   }
   for (const auto& member : members) {
      roster.emplace(self, snap_account_key_t{member.snap_account.value}, [&](auto& row) {
         row = member;
      });
   }

   state.active_version += 1;
   state.activated_at_block = static_cast<uint32_t>(sysio::current_block_number());
   state.active_count = static_cast<uint32_t>(members.size());
   state.active_digest = digest_members(members);
   snap_roster_state_singleton(self).set(state, self);
   return state;
}

/// Activates a valid governance proposal or complete non-shrinking candidate at a new-height boundary.
snap_roster_state maybe_activate_roster(name self,
                                       uint32_t block_num,
                                       name producer,
                                       name snap_account,
                                       snap_votes_table& votes) {
   snap_roster_state_singleton state_singleton(self);
   auto state = state_singleton.get_or_default(snap_roster_state{});
   if (has_pending_vote_for_height(votes, block_num)) {
      return state;
   }

   snap_config_singleton cfg_singleton(self);
   const auto cfg = cfg_singleton.get_or_default(snap_config{});
   snap_roster_proposal_singleton proposal_singleton(self);
   if (proposal_singleton.exists()) {
      const auto proposal = proposal_singleton.get();
      if (proposal.expected_active_version != state.active_version) {
         proposal_singleton.remove();
      } else if (proposal.members.size() >= cfg.min_providers
                 && validate_roster_members(self, proposal.members)) {
         // A valid governance shrink owns every open boundary. Excluded members must fail before
         // creating an old-version row, otherwise one member can consume every boundary and veto
         // the approved shrink indefinitely.
         check(contains_member(proposal.members, producer, snap_account), governance_excluded_voter_error);
         state = activate_roster(self, proposal.members, state);
         proposal_singleton.remove();
         return state;
      } else {
         proposal_singleton.remove();
      }
   }

   const auto candidates = collect_candidate_members(self);
   const auto candidate_digest = digest_members(candidates);
   const bool enough_members = state.active_version == 0
                               ? candidates.size() >= cfg.min_providers
                               : candidates.size() >= state.active_count;
   if (enough_members && candidate_digest != state.active_digest
       && contains_member(candidates, producer, snap_account)) {
      state = activate_roster(self, candidates, state);
   }
   return state;
}

/// Returns whether a pending voter still carries weight in the exact active roster.
bool is_currently_weighted_voter(name self, name producer) {
   snap_roster_table roster(self);
   auto roster_by_producer = roster.get_index<"byproducer"_n>();
   const auto roster_itr = roster_by_producer.find(producer.value);
   if (roster_itr == roster_by_producer.end()) {
      return false;
   }

   snap_registrations_table registrations(self);
   const auto registration = registrations.try_get(snap_account_key_t{roster_itr->snap_account.value});
   if (!registration || registration->producer != producer) {
      return false;
   }

   return is_currently_eligible_snapshot_producer(producers_table(self), producer);
}

/// Scans the next bounded vote-table page and removes obsolete or finalized rows.
void cleanup_pending_votes(name self,
                           snap_votes_table& votes,
                           snap_roster_state& state,
                           std::optional<uint32_t> finalized_height = std::nullopt) {
   if (finalized_height) {
      state.cleanup_finalized_height =
         std::max(state.cleanup_finalized_height, *finalized_height);
   }
   uint32_t inspected = 0;
   auto itr = state.cleanup_cursor == 0
                 ? votes.begin()
                 : votes.lower_bound(snap_vote_key_t{state.cleanup_cursor});
   while (itr != votes.end() && inspected < max_snap_vote_cleanup_rows) {
      ++inspected;
      if (itr->roster_version < state.active_version
          || (state.cleanup_finalized_height > 0
              && itr->block_num <= state.cleanup_finalized_height)) {
         itr = votes.erase(std::move(itr));
      } else {
         ++itr;
      }
   }
   state.cleanup_cursor = itr == votes.end() ? 0 : itr->id;
   snap_roster_state_singleton(self).set(state, self);
}

/// Validates the final-record state for a tuple and returns true for an identical existing record.
bool validate_final_record(name self,
                           uint32_t block_num,
                           const checksum256& block_id,
                           const checksum256& snapshot_hash) {
   snap_records_table records(self);
   const auto record = records.try_get(snap_record_key_t{static_cast<uint64_t>(block_num)});
   if (!record) {
      return false;
   }
   check(record->snapshot_hash == snapshot_hash && record->block_id == block_id,
         snap_hash_disagreement_error);
   return true;
}

/// Evaluates one exact pending tuple and creates its final record when quorum is reached.
void evaluate_pending_vote(name self,
                           uint64_t vote_id,
                           const checksum256& expected_block_id,
                           const checksum256& expected_snapshot_hash,
                           uint64_t expected_roster_version) {
   const uint32_t block_num = block_info::block_height_from_id(expected_block_id);
   check(block_num > 0, invalid_block_id_error);

   snap_records_table records(self);
   const auto final_record = records.try_get(snap_record_key_t{static_cast<uint64_t>(block_num)});
   const bool matches_final_record = final_record
                                     && final_record->snapshot_hash == expected_snapshot_hash
                                     && final_record->block_id == expected_block_id;

   snap_votes_table votes(self);
   const auto vote = votes.try_get(snap_vote_key_t{vote_id});
   if (!vote) {
      check(matches_final_record, pending_vote_not_found_error);
      return;
   }
   check(vote->roster_version == expected_roster_version
         && vote->block_num == block_num
         && vote->block_id == expected_block_id
         && vote->snapshot_hash == expected_snapshot_hash,
         vote_tuple_error);

   snap_roster_state_singleton state_singleton(self);
   auto state = state_singleton.get_or_default(snap_roster_state{});
   if (final_record || expected_roster_version < state.active_version) {
      const auto stale_vote = votes.find(snap_vote_key_t{vote_id});
      if (stale_vote != votes.end()) {
         votes.erase(std::move(stale_vote));
      }
      const std::optional<uint32_t> finalized_height = final_record
                                                          ? std::optional<uint32_t>{block_num}
                                                          : std::optional<uint32_t>{};
      cleanup_pending_votes(self, votes, state, finalized_height);
      return;
   }
   check(state.active_version == expected_roster_version, roster_version_error);

   const auto active_members = collect_active_members(self);
   check(active_members.size() == state.active_count, roster_count_mismatch_error);
   check(digest_members(active_members) == state.active_digest, roster_digest_mismatch_error);

   snap_config_singleton cfg_singleton(self);
   const auto cfg = cfg_singleton.get_or_default(snap_config{});
   if (state.active_count < cfg.min_providers) {
      cleanup_pending_votes(self, votes, state);
      return;
   }

   uint32_t voter_count = 0;
   for (const auto& voter : vote->voters) {
      if (is_currently_weighted_voter(self, voter)) {
         ++voter_count;
      }
   }

   const uint32_t bft_floor =
      state.active_count / byzantine_quorum_divisor + byzantine_quorum_increment;
   const uint32_t quorum = std::max(
      std::max(cfg.min_providers,
               (state.active_count * cfg.threshold_pct + percentage_scale - 1) / percentage_scale),
      bft_floor);
   if (voter_count < quorum) {
      cleanup_pending_votes(self, votes, state);
      return;
   }

   const snap_record_key_t record_key{static_cast<uint64_t>(block_num)};
   if (!records.contains(record_key)) {
      records.emplace(self, record_key, [&](auto& row) {
         row.block_num         = block_num;
         row.block_id          = expected_block_id;
         row.snapshot_hash     = expected_snapshot_hash;
         row.attested_at_block = static_cast<uint32_t>(sysio::current_block_number());
         row.roster_version    = state.active_version;
         row.roster_digest     = state.active_digest;
      });
   }

   const auto current_vote = votes.find(snap_vote_key_t{vote_id});
   if (current_vote != votes.end()) {
      votes.erase(std::move(current_vote));
   }
   cleanup_pending_votes(self, votes, state, block_num);
}

} // namespace

// -------------------------------------------------------------------------------------------------
void reconcile_snapshot_registrations(name self) {
   snap_registrations_table registrations(self);
   producers_table producers(self);

   auto itr = registrations.begin();
   while (itr != registrations.end()) {
      if (!is_currently_eligible_snapshot_producer(producers, itr->producer)) {
         itr = registrations.erase(std::move(itr));
      } else {
         ++itr;
      }
   }

   const auto candidates = collect_candidate_members(self);
   snap_roster_state_singleton state_singleton(self);
   if (candidates.empty() && !state_singleton.exists()) {
      return;
   }
   auto state = state_singleton.get_or_default(snap_roster_state{});
   state.candidate_count = static_cast<uint32_t>(candidates.size());
   state.candidate_digest = digest_members(candidates);
   state_singleton.set(state, self);
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::regsnapprov(name producer, name snap_account) {
   require_auth(producer);
   reconcile_snapshot_registrations(get_self());

   producers_table producers(get_self());
   const auto producer_row = require_snapshot_producer_eligibility(producers, producer);

   snap_registrations_table registrations(get_self());
   check(!registrations.contains(snap_account_key_t{snap_account.value}),
         duplicate_provider_error);
   auto by_producer = registrations.get_index<"byproducer"_n>();
   check(by_producer.find(producer.value) == by_producer.end(),
         duplicate_producer_error);

   const auto ranked = collect_ranked_registrations(get_self());
   if (ranked.size() >= max_snap_roster_size) {
      const ranked_registration candidate{snap_roster_member{snap_account, producer}, producer_row.rank};
      const auto& worst = ranked.back();
      check(candidate_precedes(candidate, worst),
            candidate_rank_error);
      registrations.erase(snap_account_key_t{worst.member.snap_account.value});
   }

   registrations.emplace(producer, snap_account_key_t{snap_account.value}, [&](auto& row) {
      row.snap_account = snap_account;
      row.producer = producer;
   });
   reconcile_snapshot_registrations(get_self());
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::delsnapprov(name account) {
   require_auth(account);
   snap_registrations_table registrations(get_self());

   auto registration = registrations.find(snap_account_key_t{account.value});
   if (registration != registrations.end()) {
      require_auth(registration->snap_account);
      registrations.erase(std::move(registration));
      reconcile_snapshot_registrations(get_self());
      return;
   }

   auto by_producer = registrations.get_index<"byproducer"_n>();
   auto producer_registration = by_producer.find(account.value);
   check(producer_registration != by_producer.end(),
         missing_registration_error);
   by_producer.erase(std::move(producer_registration));
   reconcile_snapshot_registrations(get_self());
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash) {
   require_auth(snap_account);
   const uint32_t block_num = block_info::block_height_from_id(block_id);
   check(block_num > 0, invalid_block_id_error);
   check(block_num <= sysio::current_block_number(), future_block_id_error);
   if (validate_final_record(get_self(), block_num, block_id, snapshot_hash)) {
      return;
   }

   reconcile_snapshot_registrations(get_self());
   snap_registrations_table registrations(get_self());
   const auto registration = registrations.try_get(snap_account_key_t{snap_account.value});
   check(registration.has_value(), provider_not_registered_error);
   const name producer = registration->producer;
   require_snapshot_producer_eligibility(producers_table(get_self()), producer);

   snap_votes_table votes(get_self());
   const auto state = maybe_activate_roster(get_self(), block_num, producer, snap_account, votes);
   check(state.active_version > 0, roster_not_ready_error);

   snap_roster_table roster(get_self());
   const auto active_member = roster.try_get(snap_account_key_t{snap_account.value});
   check(active_member && active_member->producer == producer, roster_membership_error);

   auto by_block_roster = votes.get_index<"byblkroster"_n>();
   const auto block_roster_key = make_snap_vote_block_roster_key(block_num, state.active_version);
   uint64_t vote_id = 0;
   bool found_producer_vote = false;
   std::optional<uint64_t> matching_vote_id;
   for (auto itr = by_block_roster.lower_bound(block_roster_key);
        itr != by_block_roster.end() && itr->by_block_roster() == block_roster_key; ++itr) {
      if (std::find(itr->voters.begin(), itr->voters.end(), producer) != itr->voters.end()) {
         check(itr->block_id == block_id && itr->snapshot_hash == snapshot_hash, equivocation_error);
         vote_id = itr->id;
         found_producer_vote = true;
         break;
      }
      if (itr->block_id == block_id && itr->snapshot_hash == snapshot_hash) {
         matching_vote_id = itr->id;
      }
   }

   if (!found_producer_vote && matching_vote_id) {
      vote_id = *matching_vote_id;
      votes.modify(same_payer, snap_vote_key_t{vote_id}, [&](auto& row) {
         row.voters.push_back(producer);
      });
   } else if (!found_producer_vote) {
      vote_id = votes.available_primary_key();
      votes.emplace(snap_account, snap_vote_key_t{vote_id}, [&](auto& row) {
         row.id = vote_id;
         row.roster_version = state.active_version;
         row.block_num = block_num;
         row.block_id = block_id;
         row.snapshot_hash = snapshot_hash;
         row.voters = {producer};
      });
   }

   evaluate_pending_vote(get_self(), vote_id, block_id, snapshot_hash, state.active_version);
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::evalsnapvote(uint64_t vote_id,
                                   checksum256 expected_block_id,
                                   checksum256 expected_snapshot_hash,
                                   uint64_t expected_roster_version) {
   evaluate_pending_vote(get_self(), vote_id, expected_block_id, expected_snapshot_hash,
                         expected_roster_version);
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::propsnaprost(std::vector<snap_roster_member> members,
                                   uint64_t expected_active_version) {
   require_auth(get_self());
   reconcile_snapshot_registrations(get_self());

   snap_roster_state_singleton state_singleton(get_self());
   const auto state = state_singleton.get_or_default(snap_roster_state{});
   check(state.active_version > 0, roster_not_bootstrapped_error);
   check(state.active_version == expected_active_version, roster_version_error);

   snap_config_singleton cfg_singleton(get_self());
   const auto cfg = cfg_singleton.get_or_default(snap_config{});
   check(members.size() >= cfg.min_providers, proposal_below_minimum_error);
   check(members.size() < state.active_count, proposal_not_shrinking_error);

   members = canonicalize_members(std::move(members));
   check(validate_roster_members(get_self(), members), invalid_proposal_members_error);
   snap_roster_proposal_singleton(get_self()).set(
      snap_roster_proposal{expected_active_version, members, digest_members(members)}, get_self());
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::cancsnaprost() {
   require_auth(get_self());
   snap_roster_proposal_singleton proposal(get_self());
   check(proposal.exists(), missing_proposal_error);
   proposal.remove();
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::setsnpcfg(uint32_t min_providers, uint32_t threshold_pct) {
   require_auth(get_self());
   check(threshold_pct > 0 && threshold_pct <= percentage_scale, threshold_range_error);
   check(min_providers > 0, minimum_provider_error);
   check(min_providers <= max_snap_roster_size,
         minimum_provider_max_error);
   snap_config_singleton(get_self()).set(snap_config{min_providers, threshold_pct}, get_self());
}

// -------------------------------------------------------------------------------------------------
snap_record snapshot_attest::getsnaphash(uint32_t block_num) {
   snap_records_table records(get_self());
   const auto record = records.require_find(snap_record_key_t{static_cast<uint64_t>(block_num)},
                                            missing_record_error);
   return *record;
}

} // namespace sysiosystem
