#include <sysio.chalg/sysio.chalg.hpp>

#include <algorithm>

namespace sysio {

using opp::types::ChallengeStatus;

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
   require_auth(get_self());

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

} // namespace sysio
