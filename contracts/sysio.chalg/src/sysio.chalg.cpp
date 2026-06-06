#include <sysio.chalg/sysio.chalg.hpp>

#include <sysio.roa.hpp>                 // T1 voter eligibility: sysio.roa::nodeowners / roastate
#include <sysio.system/emissions.hpp>    // live Tier-1 count: sysio.system::nodecount
#include <magic_enum/magic_enum.hpp>

#include <algorithm>

namespace sysio {

using opp::types::ChallengeStatus;
using opp::types::DisputeStatus;
using opp::types::NodeOwnerTier;

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

// ---------------------------------------------------------------------------
//  initchal
// ---------------------------------------------------------------------------
void chalg::initchal(uint64_t chain_req_id) {
   require_auth(MSGCH_ACCOUNT);

   challenges_t challenges(get_self());

   // Check no existing challenge for this request
   auto req_idx = challenges.get_index<"byrequest"_n>();
   auto existing = req_idx.find(chain_req_id);
   check(existing == req_idx.end() || existing->status == ChallengeStatus::CHALLENGE_STATUS_RESOLVED,
         "active challenge already exists for this request");

   auto now = current_time_point();

   uint64_t next_id = std::max<uint64_t>(1, challenges.available_primary_key());

   challenge_entry c{};
   c.id               = next_id;
   c.chain_request_id = chain_req_id;
   c.epoch_index      = 0; // TODO: read from sysio.epoch
   c.round            = 1;
   c.status           = ChallengeStatus::CHALLENGE_STATUS_CHALLENGE_SENT;
   c.challenged_at    = now;
   challenges.emplace(ram_payer, challenge_key{next_id}, c);

   // TODO: Queue ATTESTATION_TYPE_CHALLENGE_REQUEST to source outpost
   //       via sysio.msgch::queueout inline action.

   // Partial pause: NORMAL messages suspended, CHALLENGE messages still process.
   // The epoch contract's is_paused flag controls normal processing;
   // challenge-type messages bypass the pause check in sysio.msgch.
}

// ---------------------------------------------------------------------------
//  submitresp
// ---------------------------------------------------------------------------
void chalg::submitresp(uint64_t challenge_id,
                       checksum256 response_hash,
                       std::vector<name> correct_ops,
                       std::vector<name> faulty_ops) {
   require_auth(get_self());

   challenges_t challenges(get_self());
   auto ch_pk = challenge_key{challenge_id};
   auto ch_row = challenges.get(ch_pk, "challenge not found");
   check(ch_row.status == ChallengeStatus::CHALLENGE_STATUS_CHALLENGE_SENT,
         "challenge not awaiting response");

   challenges.modify(same_payer, ch_pk, [&](auto& c) {
      c.response_hash     = response_hash;
      c.correct_operators = correct_ops;
      c.faulty_operators  = faulty_ops;
      c.responded_at      = current_time_point();
      c.status            = ChallengeStatus::CHALLENGE_STATUS_RESPONSE_RECEIVED;
   });

   // Evaluate: if faulty operators identified, slash them and resolve
   if (!faulty_ops.empty()) {
      for (const auto& faulty : faulty_ops) {
         // Inline action to sysio.opreg::slash — opreg is the canonical bond
         // ledger; it routes the slashed amount to the matching LP per
         // (chain, token_kind) and emits SLASH_OPERATOR attestations to the
         // outposts. uwrit's locks remain alive and are settled (deferred-
         // slash) by sysio.uwrit::release as each lock resolves.
         action(
            permission_level{get_self(), "active"_n},
            OPREG_ACCOUNT,
            "slash"_n,
            std::make_tuple(faulty, std::string("challenge round ") + std::to_string(ch_row.round))
         ).send();
      }

      challenges.modify(same_payer, ch_pk, [&](auto& c) {
         c.status = ChallengeStatus::CHALLENGE_STATUS_RESOLVED;
      });

      // Unpause epoch
      action(
         permission_level{get_self(), "active"_n},
         EPOCH_ACCOUNT,
         "unpause"_n,
         std::make_tuple()
      ).send();
   }
   // If no faulty operators identified, challenge remains in RESPONSE_RECEIVED
   // and can be escalated via escalate().
}

// ---------------------------------------------------------------------------
//  escalate
// ---------------------------------------------------------------------------
void chalg::escalate(uint64_t challenge_id) {
   require_auth(get_self());

   challenges_t challenges(get_self());
   auto ch_pk = challenge_key{challenge_id};
   auto ch_row = challenges.get(ch_pk, "challenge not found");
   check(ch_row.status == ChallengeStatus::CHALLENGE_STATUS_RESPONSE_RECEIVED,
         "challenge must be in RESPONSE_RECEIVED state to escalate");

   if (ch_row.round < MAX_AUTOMATIC_ROUNDS) {
      // Escalate to next automatic round
      auto now = current_time_point();

      uint64_t next_id = std::max<uint64_t>(1, challenges.available_primary_key());

      challenges.emplace(ram_payer, challenge_key{next_id}, challenge_entry{
         .id               = next_id,
         .chain_request_id = ch_row.chain_request_id,
         .epoch_index      = ch_row.epoch_index,
         .round            = static_cast<uint8_t>(ch_row.round + 1),
         .status           = ChallengeStatus::CHALLENGE_STATUS_CHALLENGE_SENT,
         .challenged_at    = now,
      });

      challenges.modify(same_payer, ch_pk, [&](auto& c) {
         c.status = ChallengeStatus::CHALLENGE_STATUS_ESCALATED;
      });

      // TODO: Queue new CHALLENGE_REQUEST to outpost
   } else {
      // Max automatic rounds exhausted — escalate to manual resolution
      challenges.modify(same_payer, ch_pk, [&](auto& c) {
         c.status = ChallengeStatus::CHALLENGE_STATUS_ESCALATED;
      });

      // GLOBAL PAUSE
      action(
         permission_level{get_self(), "active"_n},
         EPOCH_ACCOUNT,
         "pause"_n,
         std::make_tuple()
      ).send();
   }
}

// ---------------------------------------------------------------------------
//  submitres
// ---------------------------------------------------------------------------
void chalg::submitres(name submitter,
                      uint64_t challenge_id,
                      checksum256 orig_hash,
                      checksum256 r1_hash,
                      checksum256 r2_hash) {
   require_auth(submitter);

   challenges_t challenges(get_self());
   auto ch_pk = challenge_key{challenge_id};
   auto ch_row = challenges.get(ch_pk, "challenge not found");
   check(ch_row.status == ChallengeStatus::CHALLENGE_STATUS_ESCALATED,
         "challenge must be escalated for manual resolution");
   check(ch_row.round >= MAX_AUTOMATIC_ROUNDS,
         "only escalated challenges accept manual resolution");

   resolutions_t resolutions(get_self());
   uint64_t next_id = std::max<uint64_t>(1, resolutions.available_primary_key());

   resolutions.emplace(submitter, resolution_key{next_id}, manual_resolution{
      .id                  = next_id,
      .challenge_id        = challenge_id,
      .original_chain_hash = orig_hash,
      .round1_chain_hash   = r1_hash,
      .round2_chain_hash   = r2_hash,
      .msig_proposal       = name(0), // TODO: link to sysio.msig proposal name
      .is_resolved         = false,
   });

   // TODO: Create sysio.msig proposal for T1/T2/T3 vote (2/3 majority).
}

// ---------------------------------------------------------------------------
//  enforce
// ---------------------------------------------------------------------------
void chalg::enforce(uint64_t resolution_id) {
   require_auth(get_self());

   resolutions_t resolutions(get_self());
   auto res_pk = resolution_key{resolution_id};
   auto res_row = resolutions.get(res_pk, "resolution not found");
   check(!res_row.is_resolved, "resolution already enforced");

   // TODO: Verify sysio.msig proposal was approved with 2/3 majority.
   //       Identify faulty operators by comparing hashes.
   //       Mass slash all operators with non-matching hashes.

   resolutions.modify(same_payer, res_pk, [&](auto& r) {
      r.is_resolved = true;
   });

   // Resolve the parent challenge
   challenges_t challenges(get_self());
   auto ch_pk = challenge_key{res_row.challenge_id};
   if (challenges.contains(ch_pk)) {
      challenges.modify(same_payer, ch_pk, [&](auto& c) {
         c.status = ChallengeStatus::CHALLENGE_STATUS_RESOLVED;
      });
   }

   // Unpause epoch
   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      "unpause"_n,
      std::make_tuple()
   ).send();
}

// ---------------------------------------------------------------------------
//  slashop
// ---------------------------------------------------------------------------
void chalg::slashop(name operator_acct, std::string reason) {
   // Authorised callers: sysio.chalg itself (challenge / dispute resolution) and sysio.epoch
   // (the single-path slash of non-canonical OPP envelope deliverers at epoch close, per the
   // dispute-vote design). Both are trusted system contracts; chalg is the single slashing
   // chokepoint that holds opreg::slash authority.
   check(has_auth(get_self()) || has_auth(EPOCH_ACCOUNT),
         "slashop requires sysio.chalg or sysio.epoch authority");

   // Slash via sysio.opreg — the canonical bond ledger. opreg routes the
   // slashable portion (`balance - sum(active locks)`) to the matching LP
   // on each (chain, token_kind) the operator has bond on, marks the
   // operator SLASHED, and lets sysio.uwrit::release deferred-slash the
   // locked portion as each underwriter lock resolves. Pause-on-slashing
   // and outpost roster sync (per-task §7 / §8) are handled by Tasks 6-9.
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
   uint64_t composite = (static_cast<uint64_t>(chain_code) << 32) | epoch_index;
   check(oe_idx.find(composite) == oe_idx.end(),
         "a dispute already exists for this outpost+epoch");

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
   });

   // Pause epoch advancement until the dispute resolves.
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

   // Voter eligibility: a Tier-1 node owner in sysio.roa::nodeowners, scoped by the active
   // network generation. Point lookup — no enumeration of the owner set.
   roa::roastate_t roastate(ROA_ACCOUNT);
   check(roastate.exists(), "roa state not initialized");
   const uint8_t network_gen = roastate.get().network_gen;

   roa::nodeowners_t nodeowners(ROA_ACCOUNT, network_gen);
   auto no = nodeowners.get(roa::nodeowner_key{owner.value},
                            "voter is not a registered node owner");
   check(no.tier == magic_enum::enum_integer(NodeOwnerTier::NODE_OWNER_TIER_T1),
         "only tier-1 node owners may vote on a dispute");

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

   // N = live Tier-1 node-owner count; Q = floor(N/2)+1.
   sysiosystem::emissions::nodecountstate_t nodecount(SYSTEM_ACCOUNT);
   const uint32_t N = nodecount.exists() ? nodecount.get().t1_count : 0;
   check(N > 0, "no tier-1 node owners are registered");
   const uint32_t Q = N / 2 + 1;

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

   // Resolve per the tally rule. Fast path (any time): a checksum reaches a majority of ALL live
   // Tier-1 owners. After the deadline: a quorum of cast votes AND a strict majority of cast votes.
   // No plurality / tie-break — an unresolved tally just keeps waiting for more votes.
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

   // Dispatch the winning envelope via sysio.msgch (it still holds the raw bytes), then release the
   // paused epoch. The next chkcons advances the epoch, where the single-path slash of every
   // non-canonical deliverer runs in sysio.epoch::advance.
   action(
      permission_level{get_self(), "active"_n},
      MSGCH_ACCOUNT,
      "resolvedisp"_n,
      std::make_tuple(d.chain_code, d.epoch_index, winner)
   ).send();
   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      "unpause"_n,
      std::make_tuple()
   ).send();
}

} // namespace sysio
