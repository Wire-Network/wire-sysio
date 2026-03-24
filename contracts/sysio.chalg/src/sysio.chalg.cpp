#include <sysio.chalg/sysio.chalg.hpp>

namespace sysio {

// ---------------------------------------------------------------------------
//  initchal
// ---------------------------------------------------------------------------
void chalg::initchal(uint64_t chain_req_id) {
   require_auth(MSGCH_ACCOUNT);

   challenges_t challenges(get_self(), get_self().value);

   // Check no existing challenge for this request
   auto req_idx = challenges.get_index<"byrequest"_n>();
   auto existing = req_idx.find(chain_req_id);
   check(existing == req_idx.end() || existing->status == CHAL_RESOLVED,
         "active challenge already exists for this request");

   auto now = current_time_point();

   challenges.emplace(get_self(), [&](auto& c) {
      c.id = challenges.available_primary_key();
      c.chain_request_id = chain_req_id;
      c.epoch_index = 0; // TODO: read from sysio.epoch
      c.round = 1;
      c.status = CHAL_CHALLENGE_SENT;
      c.challenged_at = now;
   });

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

   challenges_t challenges(get_self(), get_self().value);
   auto it = challenges.find(challenge_id);
   check(it != challenges.end(), "challenge not found");
   check(it->status == CHAL_CHALLENGE_SENT, "challenge not awaiting response");

   challenges.modify(it, same_payer, [&](auto& c) {
      c.response_hash = response_hash;
      c.correct_operators = correct_ops;
      c.faulty_operators = faulty_ops;
      c.responded_at = current_time_point();
      c.status = CHAL_RESPONSE_RECEIVED;
   });

   // Evaluate: if faulty operators identified, slash them and resolve
   if (!faulty_ops.empty()) {
      for (const auto& faulty : faulty_ops) {
         // Inline action to sysio.uwrit::slash
         action(
            permission_level{get_self(), "active"_n},
            UWRIT_ACCOUNT,
            "slash"_n,
            std::make_tuple(faulty, std::string("challenge round ") + std::to_string(it->round))
         ).send();
      }

      challenges.modify(it, same_payer, [&](auto& c) {
         c.status = CHAL_RESOLVED;
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

   challenges_t challenges(get_self(), get_self().value);
   auto it = challenges.find(challenge_id);
   check(it != challenges.end(), "challenge not found");
   check(it->status == CHAL_RESPONSE_RECEIVED,
         "challenge must be in RESPONSE_RECEIVED state to escalate");

   if (it->round < MAX_AUTOMATIC_ROUNDS) {
      // Escalate to next automatic round
      auto now = current_time_point();
      challenges.emplace(get_self(), [&](auto& c) {
         c.id = challenges.available_primary_key();
         c.chain_request_id = it->chain_request_id;
         c.epoch_index = it->epoch_index;
         c.round = it->round + 1;
         c.status = CHAL_CHALLENGE_SENT;
         c.challenged_at = now;
      });

      challenges.modify(it, same_payer, [&](auto& c) {
         c.status = CHAL_ESCALATED;
      });

      // TODO: Queue new CHALLENGE_REQUEST to outpost
   } else {
      // Max automatic rounds exhausted — escalate to manual resolution
      challenges.modify(it, same_payer, [&](auto& c) {
         c.status = CHAL_ESCALATED;
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

   challenges_t challenges(get_self(), get_self().value);
   auto it = challenges.find(challenge_id);
   check(it != challenges.end(), "challenge not found");
   check(it->status == CHAL_ESCALATED, "challenge must be escalated for manual resolution");
   check(it->round >= MAX_AUTOMATIC_ROUNDS, "only escalated challenges accept manual resolution");

   resolutions_t resolutions(get_self(), get_self().value);
   resolutions.emplace(submitter, [&](auto& r) {
      r.id = resolutions.available_primary_key();
      r.challenge_id = challenge_id;
      r.original_chain_hash = orig_hash;
      r.round1_chain_hash = r1_hash;
      r.round2_chain_hash = r2_hash;
      r.msig_proposal = name(0); // TODO: link to sysio.msig proposal name
      r.is_resolved = false;
   });

   // TODO: Create sysio.msig proposal for T1/T2/T3 vote (2/3 majority).
}

// ---------------------------------------------------------------------------
//  enforce
// ---------------------------------------------------------------------------
void chalg::enforce(uint64_t resolution_id) {
   require_auth(get_self());

   resolutions_t resolutions(get_self(), get_self().value);
   auto it = resolutions.find(resolution_id);
   check(it != resolutions.end(), "resolution not found");
   check(!it->is_resolved, "resolution already enforced");

   // TODO: Verify sysio.msig proposal was approved with 2/3 majority.
   //       Identify faulty operators by comparing hashes.
   //       Mass slash all operators with non-matching hashes.

   resolutions.modify(it, same_payer, [&](auto& r) {
      r.is_resolved = true;
   });

   // Resolve the parent challenge
   challenges_t challenges(get_self(), get_self().value);
   auto chal_it = challenges.find(it->challenge_id);
   if (chal_it != challenges.end()) {
      challenges.modify(chal_it, same_payer, [&](auto& c) {
         c.status = CHAL_RESOLVED;
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

   // Slash via sysio.uwrit
   action(
      permission_level{get_self(), "active"_n},
      UWRIT_ACCOUNT,
      "slash"_n,
      std::make_tuple(operator_acct, reason)
   ).send();

   // TODO: Blacklist operator via sysio.epoch.
   //       Queue ATTESTATION_TYPE_SLASH_OPERATOR to all outposts.
}

} // namespace sysio
