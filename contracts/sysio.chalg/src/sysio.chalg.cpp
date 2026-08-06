#include <sysio.chalg/sysio.chalg.hpp>

#include <sysio.roa.hpp>                 // authoritative T1 electorate snapshot at open
#include <sysio.uwrit/sysio.uwrit.hpp>   // uwreq + lock reads for the underwriter challenge
#include <sysio.reserv/sysio.reserv.hpp> // reserve books that price the challenge bond
#include <sysio.opreg/sysio.opreg.hpp>   // operator status guard before slashing
#include <sysio.opp.common/amm_math.hpp> // token_to_wire — the bond's WIRE valuation
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <limits>

namespace sysio {

using opp::types::DisputeStatus;
using opp::types::NodeOwnerTier;
using opp::types::OperatorStatus;
using opp::types::UnderwriteRequestStatus;

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

namespace {

/// WIRE asset symbol for the challenge-bond escrow + payouts (9 decimals — mirrors
/// `sysio.reserv`'s WIRE_SYMBOL; deliberately NOT opreg's CORE_SYM).
constexpr sysio::symbol WIRE_SYMBOL{"WIRE", 9};

/// Wall-clock now in ms — the clock `sysio.uwrit`'s lock window runs on
/// (`lock_entry.expires_at_ms`), so challenge deadlines compare like-for-like.
uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

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

/// The bond quote for challenging one commitment — see `compute_uwchal_bond`.
struct uwchal_bond_quote {
   uint64_t bond        = 0; ///< Σ `token_to_wire(leg)` over the winner's live locks; 0 = unquotable
   uint64_t deadline_ms = 0; ///< the locks' shared `expires_at_ms` — becomes the vote deadline
   uint32_t live_locks  = 0; ///< locks counted into the bond
};

/// THE bond formula — one function behind both the read-only quote (`uwchalbond`) and the charge
/// (`openuwchal`), so the two can never drift (the `split_wire_fee` principle).
///
/// "The full collateral in question" (Jonathan, 2026-07-28) is the winning underwriter's lock
/// set for this uwreq. Locks are denominated per-leg in the leg's native token, so each is
/// valued in WIRE through its OWN reserve's live books — the same books the swap rode. The sum
/// accumulates in u128 (two stacked uint64 legs can carry — the `split_wire_fee` wrap lesson).
///
/// Returns a zeroed quote (unchallengeable / unquotable) when: the winner has no locks for the
/// uwreq, any lock is already held by another challenge or already expired (filing is time-gated
/// to the live window), a leg's reserve row is gone, a leg's books price it to zero, or the sum
/// does not fit uint64.
uwchal_bond_quote compute_uwchal_bond(name uwrit_account, name reserv_account,
                                      uint64_t uwreq_id, name underwriter) {
   uwchal_bond_quote quote;
   const uint64_t now_ms = current_time_ms();

   uwrit::locks_t locks(uwrit_account);
   reserve::reserves_t reserves(reserv_account);

   opp::amm::u128 total = 0;
   auto by_uwreq = locks.get_index<"byuwreq"_n>();
   for (auto it = by_uwreq.lower_bound(uwreq_id);
        it != by_uwreq.end() && it->uwreq_id == uwreq_id; ++it) {
      if (it->underwriter != underwriter) continue;
      if (it->challenge_id != 0 || now_ms >= it->expires_at_ms) return uwchal_bond_quote{};
      auto rit = reserves.find(reserve::reserve_key{it->chain_code, it->token_code, it->reserve_code});
      if (rit == reserves.end()) return uwchal_bond_quote{};
      const uint64_t leg_wire = opp::amm::token_to_wire(rit->reserve_chain_amount,
                                                        rit->reserve_wire_amount,
                                                        rit->connector_weight_bps,
                                                        it->amount);
      if (leg_wire == 0) return uwchal_bond_quote{};
      total += leg_wire;
      quote.deadline_ms = it->expires_at_ms;
      ++quote.live_locks;
   }
   if (quote.live_locks == 0) return uwchal_bond_quote{};
   if (total > std::numeric_limits<uint64_t>::max()) return uwchal_bond_quote{};
   quote.bond = static_cast<uint64_t>(total);
   return quote;
}

} // anonymous namespace

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
   const uint8_t network_gen = roa::current_network_gen(ROA_ACCOUNT);
   auto electorate = snapshot_t1_electorate(ROA_ACCOUNT, network_gen);

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

// ---------------------------------------------------------------------------
//  openuwchal — file an underwriter-fault challenge (WIRE-297)
// ---------------------------------------------------------------------------
void chalg::openuwchal(name challenger, uint64_t uwreq_id, name underwriter,
                       uint8_t reason, std::string detail) {
   require_auth(challenger);

   // Trust boundary: the ABI carries the numeric value; the checked cast is the validation
   // (never static_cast — out-of-range would be UB and hide bad input).
   const auto fault = magic_enum::enum_cast<underwrite_fault_reason>(reason);
   check(fault.has_value(), "openuwchal: unknown fault reason");

   // The challenged commitment: a CONFIRMED uwreq whose recorded winner is `underwriter`.
   uwrit::uwreqs_t reqs(UWRIT_ACCOUNT);
   auto rq = reqs.find(uwrit::id_key{uwreq_id});
   check(rq != reqs.end(), "openuwchal: underwrite request not found");
   check(rq->status == UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED,
         "openuwchal: underwrite request is not CONFIRMED");
   check(rq->winner == underwriter, "openuwchal: underwriter is not this request's winner");

   // A verdict is final per commitment: at most one challenge EVER per (uwreq, underwriter).
   // Other commitments — other swaps, any epoch — are independently challengeable.
   uwchals_t chals(get_self());
   auto uq_idx = chals.get_index<"byuwrequw"_n>();
   const uint128_t composite = (static_cast<uint128_t>(uwreq_id) << 64) | underwriter.value;
   check(uq_idx.find(composite) == uq_idx.end(),
         "openuwchal: this commitment has already been challenged");

   // Price the bond off the SAME formula `uwchalbond` quotes. Zero live locks means the window
   // has closed (or never opened); an unpriceable bond refuses the filing rather than guessing.
   const auto quote = compute_uwchal_bond(UWRIT_ACCOUNT, RESERV_ACCOUNT, uwreq_id, underwriter);
   check(quote.live_locks > 0,
         "openuwchal: no live collateral locks to challenge (the lock window has closed)");
   check(quote.bond > 0, "openuwchal: challenge bond cannot be priced");

   const uint8_t network_gen = roa::current_network_gen(ROA_ACCOUNT);
   auto electorate = snapshot_t1_electorate(ROA_ACCOUNT, network_gen);
   // Same refusal as opendispute: an electorate that cannot vote could never resolve the
   // challenge — it would only lapse at the window's end, having held the bond for nothing.
   check(!electorate.empty(), "cannot open a challenge with no registered tier-1 node owners");
   const uint32_t quorum = static_cast<uint32_t>(electorate.size()) / 2 + 1;

   // Escrow the bond under the challenger's OWN authority (the swapfromwire escrow pattern) —
   // this contract holds it until resolution routes it per the verdict.
   action(
      permission_level{challenger, "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(challenger, get_self(),
         asset(static_cast<int64_t>(quote.bond), WIRE_SYMBOL),
         std::string("sysio.chalg::openuwchal challenge bond"))
   ).send();

   const auto now     = current_time_point();
   uint64_t   next_id = std::max<uint64_t>(1, chals.available_primary_key());

   chals.emplace(ram_payer, uwchal_key{next_id}, uwchal_entry{
      .id          = next_id,
      .uwreq_id    = uwreq_id,
      .underwriter = underwriter,
      .challenger  = challenger,
      .reason      = *fault,
      .detail      = std::move(detail),
      .status      = DisputeStatus::DISPUTE_STATUS_OPEN,
      .verdict     = uwchal_verdict::NONE,
      .bond_amount = quote.bond,
      .deadline_ms = quote.deadline_ms,
      .opened_at   = now,
      .network_gen = network_gen,
      .electorate  = std::move(electorate),
      .quorum      = quorum,
   });

   // Mark the locks held. uwrit owns the lock data and re-validates liveness authoritatively —
   // a stale filing aborts the WHOLE open, bond escrow included. A held lock is skipped by
   // chklocks until the challenge resolves or lapses. NOTE: deliberately NO epoch pause — an
   // unresolved challenge is survivable, so the chain keeps advancing.
   action(
      permission_level{get_self(), "active"_n},
      UWRIT_ACCOUNT, "holdlocks"_n,
      std::make_tuple(uwreq_id, underwriter, next_id)
   ).send();
}

// ---------------------------------------------------------------------------
//  voteuwchal — Tier-1 ballot in an open challenge (record-only, like votedispute)
// ---------------------------------------------------------------------------
void chalg::voteuwchal(name owner, uint64_t chal_id, uint8_t ballot) {
   require_auth(owner);

   const auto cast_ballot = magic_enum::enum_cast<uwchal_ballot>(ballot);
   check(cast_ballot.has_value(), "voteuwchal: unknown ballot value");

   uwchals_t chals(get_self());
   auto c = chals.get(uwchal_key{chal_id}, "challenge not found");
   check(c.status == DisputeStatus::DISPUTE_STATUS_OPEN, "challenge is not open");

   // Voter eligibility: membership in the challenge's snapshotted Tier-1 electorate — the same
   // list chkuwchal's quorum is measured against, so a voter the tally would not count can never
   // cast a ballot.
   check(std::find(c.electorate.begin(), c.electorate.end(), owner) != c.electorate.end(),
         "voter is not in the challenge's tier-1 electorate");

   // One ballot per owner (the vote table is scoped by chal_id).
   uwchalvotes_t votes(get_self(), chal_id);
   auto v_pk = uwchal_vote_key{owner.value};
   check(!votes.contains(v_pk), "owner has already voted in this challenge");

   votes.emplace(ram_payer, v_pk, uwchal_vote{
      .owner    = owner,
      .ballot   = *cast_ballot,
      .voted_at = current_time_point(),
   });
}

// ---------------------------------------------------------------------------
//  chkuwchal — tally an open challenge; resolve or lapse (chkdispute's sibling)
// ---------------------------------------------------------------------------
void chalg::chkuwchal(uint64_t chal_id) {
   // Permissionless crank — poked inline by `sysio.uwrit::chklocks` for any expired-but-
   // challenged lock (the epoch tick IS the cadence; possible because the chain is not paused),
   // or called manually right after the deciding vote for a faster resolution.
   uwchals_t chals(get_self());
   auto c_pk = uwchal_key{chal_id};
   auto c = chals.get(c_pk, "challenge not found");
   check(c.status == DisputeStatus::DISPUTE_STATUS_OPEN, "challenge is not open");

   const uint32_t N = static_cast<uint32_t>(c.electorate.size());
   const uint32_t Q = c.quorum;
   check(Q > 0 && Q <= N, "challenge has no electorate snapshot");

   // Tally the cast ballots.
   uint32_t uphold = 0, reject_refund = 0, reject_forfeit = 0;
   uwchalvotes_t votes(get_self(), chal_id);
   for (auto it = votes.begin(); it != votes.end(); ++it) {
      switch (it->ballot) {
         case uwchal_ballot::UPHOLD:         ++uphold;         break;
         case uwchal_ballot::REJECT_REFUND:  ++reject_refund;  break;
         case uwchal_ballot::REJECT_FORFEIT: ++reject_forfeit; break;
      }
   }

   // Resolution rules — quorum thresholds any time; the deadline only LAPSES (there is
   // deliberately no after-deadline relaxed tally: a challenge must be voted before the lock
   // window expires; envelope disputes relax only because a paused chain MUST resolve).
   uwchal_verdict verdict = uwchal_verdict::NONE;
   if (uphold >= Q) {
      verdict = uwchal_verdict::UPHELD;
   } else if (reject_refund + reject_forfeit >= Q) {
      // Disposition by majority among the rejectors; a tie favours the refund — forfeiture
      // only ever happens by explicit council judgment.
      verdict = (reject_forfeit > reject_refund) ? uwchal_verdict::REJECTED_FORFEIT
                                                 : uwchal_verdict::REJECTED_REFUND;
   } else if (current_time_ms() >= c.deadline_ms) {
      verdict = uwchal_verdict::LAPSED;
   }
   if (verdict == uwchal_verdict::NONE) return; // keep waiting for ballots

   chals.modify(same_payer, c_pk, [&](auto& r) {
      r.status  = DisputeStatus::DISPUTE_STATUS_RESOLVED;
      r.verdict = verdict;
   });

   if (verdict == uwchal_verdict::UPHELD) {
      // Slash FIRST, then sweep: inline actions run depth-first in send order, so the SLASHED
      // status flip lands before sweeplocks' releaselock calls — which then take the
      // deferred-slash branch, debiting the locked collateral and emitting the outbound SLASH
      // attestations. Guard on live status: this can run inline from the epoch tick, where an
      // aborted slash (already SLASHED/TERMINATED out-of-band) must not stall advancement.
      opreg::operators_t ops(OPREG_ACCOUNT);
      auto op = ops.find(opreg::operator_key{c.underwriter.value});
      const bool slashable = op != ops.end() &&
                             op->status != OperatorStatus::OPERATOR_STATUS_SLASHED &&
                             op->status != OperatorStatus::OPERATOR_STATUS_TERMINATED;
      if (slashable) {
         action(
            permission_level{get_self(), "active"_n},
            get_self(), "slashop"_n,
            std::make_tuple(c.underwriter,
               std::string("underwriter fault upheld (")
                  + std::string(magic_enum::enum_name(c.reason))
                  + "): challenge " + std::to_string(chal_id))
         ).send();
      }
      action(
         permission_level{get_self(), "active"_n},
         UWRIT_ACCOUNT, "sweeplocks"_n,
         std::make_tuple(c.uwreq_id, c.underwriter)
      ).send();
   } else {
      // REJECTED or LAPSED: clear the hold; the locks live out their natural window and release
      // on the next normal sweep (a healthy release — no collateral moves).
      action(
         permission_level{get_self(), "active"_n},
         UWRIT_ACCOUNT, "freelocks"_n,
         std::make_tuple(c.uwreq_id, c.underwriter)
      ).send();
   }

   // Route the bond per the verdict: forfeiture to the wrongly-challenged underwriter ONLY on an
   // explicit REJECT_FORFEIT majority; every other outcome returns it to the challenger.
   if (c.bond_amount > 0) {
      const bool forfeited = (verdict == uwchal_verdict::REJECTED_FORFEIT);
      const name bond_recipient = forfeited ? c.underwriter : c.challenger;
      action(
         permission_level{get_self(), "active"_n},
         TOKEN_ACCOUNT, "transfer"_n,
         std::make_tuple(get_self(), bond_recipient,
            asset(static_cast<int64_t>(c.bond_amount), WIRE_SYMBOL),
            forfeited
               ? std::string("sysio.chalg: challenge rejected — bond forfeited to the underwriter")
               : std::string("sysio.chalg: challenge bond returned"))
      ).send();
   }
}

// ---------------------------------------------------------------------------
//  uwchalbond — read-only quote of the bond openuwchal would charge right now
// ---------------------------------------------------------------------------
uint64_t chalg::uwchalbond(uint64_t uwreq_id, name underwriter) {
   // Soft gating — a quote, not a filing: every not-currently-challengeable state answers 0
   // (callers treat 0 as "no quote"), mirroring `sysio.reserv::swapquote`'s contract.
   uwrit::uwreqs_t reqs(UWRIT_ACCOUNT);
   auto rq = reqs.find(uwrit::id_key{uwreq_id});
   if (rq == reqs.end()) return 0;
   if (rq->status != UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED) return 0;
   if (rq->winner != underwriter) return 0;

   uwchals_t chals(get_self());
   auto uq_idx = chals.get_index<"byuwrequw"_n>();
   const uint128_t composite = (static_cast<uint128_t>(uwreq_id) << 64) | underwriter.value;
   if (uq_idx.find(composite) != uq_idx.end()) return 0; // already challenged — a verdict is final

   return compute_uwchal_bond(UWRIT_ACCOUNT, RESERV_ACCOUNT, uwreq_id, underwriter).bond;
}

} // namespace sysio
