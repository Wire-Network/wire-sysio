#include <sysio.chalg/sysio.chalg.hpp>

#include <sysio.roa.hpp>                 // authoritative T1 electorate snapshot at open
#include <sysio.opreg/sysio.opreg.hpp>   // operator status guard before slashing
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <limits>
#include <optional>

namespace sysio {

using opp::types::DisputeStatus;
using opp::types::NodeOwnerTier;
using opp::types::OperatorStatus;

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

namespace {

/// Action identifiers owned by sysio.epoch and sent by dispute open and resolution.
namespace epoch_actions {
constexpr name PAUSE   = "pause"_n;
constexpr name UNPAUSE = "unpause"_n;
} // namespace epoch_actions

/// Rejection text for a dispute without enough competing envelope versions to adjudicate.
constexpr const char* DISPUTE_REQUIRES_TWO_CANDIDATES =
   "a dispute requires at least two candidate envelope versions";

/// Rejection text for a ballot on a dispute whose tally already has a winner.
constexpr const char* DISPUTE_ALREADY_HAS_WINNER = "dispute already has a winner";

/// Rejection text for a `slashop` authorized by neither sysio.chalg nor sysio.msgch.
constexpr const char* SLASHOP_REQUIRES_CHALG_OR_MSGCH = "slashop requires sysio.chalg or sysio.msgch authority";

/// Snapshot the Tier-1 electorate: the Tier-1 rows of `sysio.roa::nodeowners` for `network_gen`,
/// walked via the `bytier` index (bounded by the Tier-1 registration cap). Shared by
/// `opendispute` and `openuwchal` so both case types freeze eligibility and quorum from the SAME
/// list at open — later registrations or a generation rotation can never change an in-flight
/// case's electorate.
std::vector<name> snapshot_t1_electorate(name roa_account, uint8_t network_gen) {
   roa::nodeowners_t nodeowners(roa_account, network_gen);
   auto by_tier = nodeowners.get_index<"bytier"_n>();
   const uint64_t t1_tier = magic_enum::enum_integer(NodeOwnerTier::NODE_OWNER_TIER_T1);
   std::vector<name> electorate;
   for (auto it = by_tier.lower_bound(t1_tier); it != by_tier.end() && it->by_tier() == t1_tier; ++it) {
      electorate.push_back(it->owner);
   }
   return electorate;
}

/// The winning checksum of dispute `d`'s tally `votes`, if it has one. Fast path (any time): a
/// checksum reaches the dispute's quorum Q. After the deadline: a quorum of cast votes AND a strict
/// majority of cast votes. No plurality / tie-break -- an undecided tally waits for more votes.
std::optional<checksum256> tally_winner(const chalg::dispute_entry& d, const chalg::disputevotes_t& votes) {
   // Tally the cast votes by chosen checksum (parallel vectors; the set is <= 21).
   std::vector<checksum256> seen;
   std::vector<uint32_t>    counts;
   uint32_t cast = 0;
   for (auto it = votes.begin(); it != votes.end(); ++it) {
      bool found = false;
      for (size_t i = 0; i < seen.size(); ++i) {
         if (seen[i] == it->chosen_checksum) { counts[i]++; found = true; break; }
      }
      if (!found) { seen.push_back(it->chosen_checksum); counts.push_back(1); }
      ++cast;
   }

   const uint32_t Q             = d.quorum;
   const bool     past_deadline = current_time_point() >= d.deadline;
   for (size_t i = 0; i < seen.size(); ++i) {
      if (counts[i] >= Q) return seen[i];
      if (past_deadline && cast >= Q && 2 * counts[i] > cast) return seen[i];
   }
   return std::nullopt;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  slashop — execute a slash on an operator via sysio.opreg
// ---------------------------------------------------------------------------
void chalg::slashop(name operator_acct, std::string reason) {
   // Authorised callers: sysio.chalg itself and sysio.msgch, which slashes each non-canonical OPP envelope
   // deliverer when it accepts the epoch's winner. chalg is the single slashing chokepoint that holds
   // opreg::slash authority.
   check(has_auth(get_self()) || has_auth(MSGCH_ACCOUNT), SLASHOP_REQUIRES_CHALG_OR_MSGCH);

   // Slash via sysio.opreg — the canonical bond ledger. opreg removes every collateral balance the
   // operator holds, keeps the custody with the registry, marks the operator SLASHED, and records
   // one OPERATOR_ACTION(SLASH) audit entry per (chain, token) balance.
   action(
      permission_level{get_self(), "active"_n},
      OPREG_ACCOUNT,
      "slash"_n,
      std::make_tuple(operator_acct, reason)
   ).send();
}

// ---------------------------------------------------------------------------
//  opendispute — open an OPP envelope dispute (called inline by sysio.msgch)
// ---------------------------------------------------------------------------
void chalg::opendispute(uint64_t chain_code,
                        uint32_t epoch_index,
                        std::vector<dispute_candidate> candidates) {
   require_auth(MSGCH_ACCOUNT);
   check(candidates.size() >= chalg_limits::minimum_dispute_candidate_versions,
         DISPUTE_REQUIRES_TWO_CANDIDATES);

   disputes_t disputes(get_self());

   // At most one dispute per (outpost, epoch). evalcons gates on this too, but enforce it here so
   // the inline call is idempotent under re-fired deliveries.
   auto oe_idx = disputes.get_index<"byoutepoch"_n>();
   uint128_t composite = opp::outpost_epoch_key(chain_code, epoch_index);
   check(oe_idx.find(composite) == oe_idx.end(),
         "a dispute already exists for this outpost+epoch");

   // Snapshot the electorate: the Tier-1 rows of sysio.roa::nodeowners for the active network
   // generation, walked via the `bytier` index (bounded by the Tier-1 registration cap). Voter
   // eligibility (votedispute) and the tally quorum (chkdispute) are both served from this
   // snapshot for the dispute's whole life, so they cannot diverge from each other, and later
   // registrations or a generation rotation cannot change an in-flight dispute's electorate or
   // quorum.
   const uint8_t network_gen = roa::current_network_gen(ROA_ACCOUNT);
   auto electorate = snapshot_t1_electorate(ROA_ACCOUNT, network_gen);

   // Defense in depth for direct calls: msgch preflights this invariant and soft-returns so a
   // terminal delivery remains retryable, while this assertion keeps every other caller from
   // opening an unresolvable, permanently-pausing dispute.
   check(!electorate.empty(), "cannot open a dispute with no registered tier-1 node owners");
   const uint32_t quorum = static_cast<uint32_t>(electorate.size()) / 2 + 1;

   auto     now     = current_time_point();
   uint64_t next_id = std::max<uint64_t>(1, disputes.available_primary_key());

   disputes.emplace(ram_payer, dispute_key{next_id}, dispute_entry{
      .id               = next_id,
      .chain_code       = chain_code,
      .epoch_index      = epoch_index,
      .status           = DisputeStatus::DISPUTE_STATUS_OPEN,
      .winning_checksum = checksum256{},
      .opened_at        = now,
      .deadline         = now + sysio::seconds(dispute_deadline_sec),
      .candidates       = std::move(candidates),
      .network_gen      = network_gen,
      .electorate       = std::move(electorate),
      .quorum           = quorum,
   });

   // Pause the disputed chain's epoch advancement until the dispute resolves; other chains keep
   // advancing. chkdispute releases the pause.
   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      epoch_actions::PAUSE,
      std::make_tuple(sysio::slug_name{chain_code})
   ).send();
}

// ---------------------------------------------------------------------------
//  votedispute — Tier-1 node owner votes for the canonical envelope checksum
// ---------------------------------------------------------------------------
void chalg::votedispute(name owner, uint64_t dispute_id, checksum256 chosen_checksum) {
   require_auth(owner);

   disputes_t disputes(get_self());
   auto d = disputes.get(dispute_key{dispute_id}, "dispute not found");
   check(d.status == DisputeStatus::DISPUTE_STATUS_OPEN, "dispute is not open");

   // `chosen_checksum` must be one of the dispute's candidate versions.
   bool is_candidate = false;
   for (const auto& c : d.candidates) {
      if (c.checksum == chosen_checksum) { is_candidate = true; break; }
   }
   check(is_candidate, "chosen checksum is not a candidate in this dispute");

   // Voter eligibility: membership in the dispute's snapshotted Tier-1 electorate. The snapshot is
   // the same list chkdispute's quorum is measured against, so a voter the tally would not count
   // can never cast a vote, and an owner registered after the dispute opened cannot join it.
   check(std::find(d.electorate.begin(), d.electorate.end(), owner) != d.electorate.end(),
         "voter is not in the dispute's tier-1 electorate");

   // One vote per owner (the vote table is scoped by dispute_id).
   disputevotes_t votes(get_self(), dispute_id);
   auto v_pk = dispute_vote_key{owner.value};
   check(!votes.contains(v_pk), "owner has already voted in this dispute");

   // A tally with a winner takes no more ballots: past the deadline one more ballot for another
   // checksum could break the winner's strict majority of cast votes before chkdispute records it.
   check(!tally_winner(d, votes), DISPUTE_ALREADY_HAS_WINNER);

   votes.emplace(ram_payer, v_pk, dispute_vote{
      .owner           = owner,
      .chosen_checksum = chosen_checksum,
      .voted_at        = current_time_point(),
   });
}

// ---------------------------------------------------------------------------
//  chkdispute — tally votes; on resolution dispatch the winner and unpause
// ---------------------------------------------------------------------------
void chalg::chkdispute(uint64_t dispute_id) {
   // Permissionless crank, driven by `batch_operator_plugin`'s epoch tick
   // (`--batch-epoch-poll-ms`, 15s default) from every ACTIVE batch operator.
   // That cadence is this action's ONLY driver: a dispute pauses
   // `sysio.epoch::advance` itself, so no inline poke from the epoch cycle can reach here.
   disputes_t disputes(get_self());
   auto d_pk = dispute_key{dispute_id};
   auto d = disputes.get(d_pk, "dispute not found");
   check(d.status == DisputeStatus::DISPUTE_STATUS_OPEN, "dispute is not open");

   // N = the dispute's snapshotted Tier-1 electorate size; Q = its quorum, both fixed when the
   // dispute opened, so registry changes while it is open can never raise, lower, or zero the
   // threshold. opendispute guarantees a non-empty snapshot; the invariant check guards rows
   // written before electorate snapshots existed from silently resolving with Q == 0.
   const uint32_t N = static_cast<uint32_t>(d.electorate.size());
   const uint32_t Q = d.quorum;
   check(Q > 0 && Q <= N, "dispute has no electorate snapshot");

   disputevotes_t votes(get_self(), dispute_id);
   const auto winner = tally_winner(d, votes);
   if (!winner) return;

   disputes.modify(same_payer, d_pk, [&](auto& r) {
      r.status           = DisputeStatus::DISPUTE_STATUS_RESOLVED;
      r.winning_checksum = *winner;
   });

   // Dispatch the winning envelope via sysio.msgch (it still holds the raw bytes). The winner's
   // acceptance in `resolvedisp` slashes the losers: every operator whose delivery for the epoch differs
   // from it, one delivered after the dispute opened included.
   action(
      permission_level{get_self(), "active"_n},
      MSGCH_ACCOUNT,
      "resolvedisp"_n,
      std::make_tuple(d.chain_code, d.epoch_index, *winner)
   ).send();

   // Release the chain's pause. A paused chain cannot open another dispute: its epoch cannot move,
   // and an (outpost, epoch) holds at most one dispute, so this was its only open one.
   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      epoch_actions::UNPAUSE,
      std::make_tuple(sysio::slug_name{d.chain_code})
   ).send();
}

} // namespace sysio
