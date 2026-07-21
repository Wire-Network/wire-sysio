#include <sysio.chalg/sysio.chalg.hpp>

#include <sysio.roa.hpp>   // T1 electorate snapshot at opendispute: sysio.roa::nodeowners / roastate
#include <magic_enum/magic_enum.hpp>

#include <algorithm>

namespace sysio {

using opp::types::DisputeStatus;
using opp::types::NodeOwnerTier;

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

// ---------------------------------------------------------------------------
//  slashop — execute a slash on an operator via sysio.opreg
// ---------------------------------------------------------------------------
void chalg::slashop(name operator_acct, std::string reason) {
   // Authorised callers: sysio.chalg itself (dispute resolution) and sysio.epoch (the single-path
   // slash of non-canonical OPP envelope deliverers at epoch close, per the dispute-vote design).
   // Both are trusted system contracts; chalg is the single slashing chokepoint that holds
   // opreg::slash authority.
   check(has_auth(get_self()) || has_auth(EPOCH_ACCOUNT),
         "slashop requires sysio.chalg or sysio.epoch authority");

   // Slash via sysio.opreg — the canonical bond ledger. opreg routes the slashable portion
   // (`balance - sum(active locks)`) to the matching LP on each (chain, token_kind) the operator has
   // bond on, marks the operator SLASHED, and lets sysio.uwrit::release deferred-slash the locked
   // portion as each underwriter lock resolves.
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
   check(candidates.size() >= 3,
         "a dispute requires at least 3 candidate envelope versions");

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
   roa::roastate_t roastate(ROA_ACCOUNT);
   check(roastate.exists(), "roa state not initialized");
   const uint8_t network_gen = roastate.get().network_gen;

   roa::nodeowners_t nodeowners(ROA_ACCOUNT, network_gen);
   auto by_tier = nodeowners.get_index<"bytier"_n>();
   const uint64_t t1_tier = magic_enum::enum_integer(NodeOwnerTier::NODE_OWNER_TIER_T1);
   std::vector<name> electorate;
   for (auto it = by_tier.lower_bound(t1_tier); it != by_tier.end() && it->by_tier() == t1_tier; ++it) {
      electorate.push_back(it->owner);
   }

   // An empty electorate could never vote, so the dispute could never resolve and the epoch pause
   // below would hold forever. Refuse to open instead -- the conflicting deliveries keep this
   // epoch from reaching consensus regardless, and the failure then names the actual problem.
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

   chalgstate_t chalgstate(get_self());
   auto st = chalgstate.get_or_default(chalg_state{});
   ++st.open_disputes;
   chalgstate.set(st, ram_payer);

   // Pause epoch advancement until every open dispute resolves. chkdispute releases the pause when
   // the last open dispute goes RESOLVED; re-sending pause for a second concurrent dispute is an
   // idempotent flag set.
   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      "pause"_n,
      std::make_tuple()
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
   // Permissionless crank — batch operators call this on their ~15s cadence.
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

   // Tally the cast votes by chosen checksum (parallel vectors; the set is <= 21).
   disputevotes_t votes(get_self(), dispute_id);
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

   // Resolve per the tally rule. Fast path (any time): a checksum reaches a majority of the
   // snapshotted electorate. After the deadline: a quorum of cast votes AND a strict majority of
   // cast votes. No plurality / tie-break — an unresolved tally just keeps waiting for more votes.
   const bool  past_deadline = current_time_point() >= d.deadline;
   bool        resolved      = false;
   checksum256 winner{};
   for (size_t i = 0; i < seen.size(); ++i) {
      if (counts[i] >= Q) { winner = seen[i]; resolved = true; break; }
      if (past_deadline && cast >= Q && 2 * counts[i] > cast) {
         winner = seen[i]; resolved = true; break;
      }
   }
   if (!resolved) return;

   disputes.modify(same_payer, d_pk, [&](auto& r) {
      r.status           = DisputeStatus::DISPUTE_STATUS_RESOLVED;
      r.winning_checksum = winner;
   });

   // This dispute leaves OPEN: drop the open-dispute count. The decrement is guarded so a row
   // written before the counter existed cannot underflow it.
   chalgstate_t chalgstate(get_self());
   auto st = chalgstate.get_or_default(chalg_state{});
   if (st.open_disputes > 0) --st.open_disputes;
   chalgstate.set(st, ram_payer);

   // Dispatch the winning envelope via sysio.msgch (it still holds the raw bytes). The next
   // chkcons advances the epoch, where the single-path slash of every non-canonical deliverer
   // runs in sysio.epoch::advance.
   action(
      permission_level{get_self(), "active"_n},
      MSGCH_ACCOUNT,
      "resolvedisp"_n,
      std::make_tuple(d.chain_code, d.epoch_index, winner)
   ).send();

   // Release the epoch pause only when no other dispute remains open -- sysio.epoch::is_paused is
   // a single flag shared by every dispute, so an earlier resolution must not resume advancement
   // while another (outpost, epoch) dispute is still voting.
   if (st.open_disputes == 0) {
      action(
         permission_level{get_self(), "active"_n},
         EPOCH_ACCOUNT,
         "unpause"_n,
         std::make_tuple()
      ).send();
   }
}

} // namespace sysio
