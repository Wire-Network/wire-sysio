#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>

namespace sysio {

   class [[sysio::contract("sysio.chalg")]] chalg : public contract {
   public:
      using contract::contract;

      /// One candidate envelope version in an OPP dispute: a distinct delivered checksum and the
      /// batch operators that delivered it. Used both as the `opendispute` action payload and as
      /// the per-row record on `dispute_entry::candidates`.
      struct dispute_candidate {
         checksum256       checksum;   ///< sha256 of the candidate envelope bytes
         std::vector<name> operators;  ///< batch operators that delivered this checksum

         SYSLIB_SERIALIZE(dispute_candidate, (checksum)(operators))
      };

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Initiate challenge (called by sysio.msgch on consensus failure).
      [[sysio::action]]
      void initchal(uint64_t chain_req_id);

      /// Process challenge response from outpost.
      [[sysio::action]]
      void submitresp(uint64_t challenge_id,
                      checksum256 response_hash,
                      std::vector<name> correct_ops,
                      std::vector<name> faulty_ops);

      /// Escalate to round 2 or manual resolution.
      [[sysio::action]]
      void escalate(uint64_t challenge_id);

      /// T1/T2 submits manual resolution referencing sysio.msig proposal.
      [[sysio::action]]
      void submitres(name submitter,
                     uint64_t challenge_id,
                     checksum256 orig_hash,
                     checksum256 r1_hash,
                     checksum256 r2_hash);

      /// Enforce resolution after sysio.msig approval (2/3 vote).
      [[sysio::action]]
      void enforce(uint64_t resolution_id);

      /// Execute slash on an operator.
      [[sysio::action]]
      void slashop(name operator_acct, std::string reason);

      // -----------------------------------------------------------------------
      //  OPP envelope dispute vote (Tier-1 node-owner resolution)
      // -----------------------------------------------------------------------

      /// Open an OPP envelope dispute. Called inline by `sysio.msgch::evalcons` when the active
      /// batch operators delivered 3+ distinct envelope versions for one (outpost, epoch) with no
      /// majority. Records the candidate checksums and pauses epoch advancement until a Tier-1
      /// node-owner vote resolves the canonical envelope.
      [[sysio::action]]
      void opendispute(uint64_t chain_code,
                       uint32_t epoch_index,
                       std::vector<dispute_candidate> candidates);

      /// Cast a Tier-1 node-owner vote for the canonical envelope checksum in an open dispute.
      /// `owner` must be a Tier-1 node owner in `sysio.roa::nodeowners`; `chosen_checksum` must be
      /// one of the dispute's candidate checksums. One vote per owner.
      [[sysio::action]]
      void votedispute(name owner, uint64_t dispute_id, checksum256 chosen_checksum);

      /// Permissionless crank: tally the votes for an open dispute. When the threshold is met
      /// (fast path: a checksum reaches `floor(N/2)+1` of the live Tier-1 count any time; after the
      /// 24h deadline: quorum of cast votes AND a strict majority of cast votes), record the winning
      /// checksum, dispatch it via `sysio.msgch::resolvedisp`, and unpause the epoch.
      [[sysio::action]]
      void chkdispute(uint64_t dispute_id);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Challenge primary key (auto-incrementing id).
      struct challenge_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(challenge_key, (id))
      };

      /// Challenge state table.
      struct [[sysio::table("challenges")]] challenge_entry {
         uint64_t    id;
         uint64_t    chain_request_id;
         uint32_t    epoch_index;
         uint8_t     round;            // 1 or 2
         opp::types::ChallengeStatus status;
         checksum256 challenge_hash;
         checksum256 response_hash;
         std::vector<name> correct_operators;
         std::vector<name> faulty_operators;
         time_point  challenged_at{};
         time_point  responded_at{};

         uint64_t by_request() const { return chain_request_id; }
         uint64_t by_epoch() const { return epoch_index; }

         SYSLIB_SERIALIZE(challenge_entry,
            (id)(chain_request_id)(epoch_index)(round)(status)(challenge_hash)(response_hash)
            (correct_operators)(faulty_operators)(challenged_at)(responded_at))
      };

      using challenges_t = sysio::kv::table<"challenges"_n, challenge_key, challenge_entry,
         sysio::kv::index<"byrequest"_n,
            sysio::const_mem_fun<challenge_entry, uint64_t, &challenge_entry::by_request>>,
         sysio::kv::index<"byepoch"_n,
            sysio::const_mem_fun<challenge_entry, uint64_t, &challenge_entry::by_epoch>>
      >;

      /// Manual resolution primary key (auto-incrementing id).
      struct resolution_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(resolution_key, (id))
      };

      /// Manual resolution table.
      struct [[sysio::table("resolutions")]] manual_resolution {
         uint64_t    id;
         uint64_t    challenge_id;
         checksum256 original_chain_hash;
         checksum256 round1_chain_hash;
         checksum256 round2_chain_hash;
         name        msig_proposal;
         bool        is_resolved = false;

         SYSLIB_SERIALIZE(manual_resolution,
            (id)(challenge_id)(original_chain_hash)(round1_chain_hash)
            (round2_chain_hash)(msig_proposal)(is_resolved))
      };

      using resolutions_t = sysio::kv::table<"resolutions"_n, resolution_key, manual_resolution>;

      /// Dispute primary key (auto-incrementing id).
      struct dispute_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(dispute_key, (id))
      };

      /// OPP envelope dispute. Opened on a 3+-way no-majority split for one (outpost, epoch);
      /// resolved by a Tier-1 node-owner vote on the canonical checksum. The row is retained after
      /// resolution as the audit record (and as the guard that prevents re-opening the same
      /// (outpost, epoch) dispute).
      struct [[sysio::table("disputes")]] dispute_entry {
         uint64_t                       id;
         uint64_t                       chain_code;        ///< outpost slug_name value
         uint32_t                       epoch_index;
         opp::types::DisputeStatus      status;
         checksum256                    winning_checksum;  ///< zero until RESOLVED
         time_point                     opened_at{};
         time_point                     deadline{};        ///< opened_at + dispute_deadline_sec
         std::vector<dispute_candidate> candidates;

         uint64_t by_epoch() const { return epoch_index; }
         uint64_t by_outpost_epoch() const {
            return (static_cast<uint64_t>(chain_code) << 32) | epoch_index;
         }

         SYSLIB_SERIALIZE(dispute_entry,
            (id)(chain_code)(epoch_index)(status)(winning_checksum)
            (opened_at)(deadline)(candidates))
      };

      using disputes_t = sysio::kv::table<"disputes"_n, dispute_key, dispute_entry,
         sysio::kv::index<"byepoch"_n,
            sysio::const_mem_fun<dispute_entry, uint64_t, &dispute_entry::by_epoch>>,
         sysio::kv::index<"byoutepoch"_n,
            sysio::const_mem_fun<dispute_entry, uint64_t, &dispute_entry::by_outpost_epoch>>
      >;

      /// Dispute-vote primary key (the voting Tier-1 owner). The vote table is scoped by
      /// `dispute_id`, so the owner alone is unique within a dispute.
      struct dispute_vote_key {
         uint64_t owner;
         uint64_t primary_key() const { return owner; }
         SYSLIB_SERIALIZE(dispute_vote_key, (owner))
      };

      /// One Tier-1 node-owner vote in a dispute. Scoped by `dispute_id`.
      struct [[sysio::table("disputevote")]] dispute_vote {
         name        owner;
         checksum256 chosen_checksum;
         time_point  voted_at{};

         SYSLIB_SERIALIZE(dispute_vote, (owner)(chosen_checksum)(voted_at))
      };

      using disputevotes_t =
         sysio::kv::scoped_table<"disputevote"_n, dispute_vote_key, dispute_vote>;

   private:
      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name OPREG_ACCOUNT  = "sysio.opreg"_n;
      static constexpr name UWRIT_ACCOUNT  = "sysio.uwrit"_n;
      static constexpr name MSIG_ACCOUNT   = "sysio.msig"_n;
      static constexpr name ROA_ACCOUNT    = "sysio.roa"_n;
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;

      // ChallengeStatus constants (match protobuf values)
      using ChallengeStatus = opp::types::ChallengeStatus;
      using DisputeStatus   = opp::types::DisputeStatus;
      using NodeOwnerTier   = opp::types::NodeOwnerTier;

      static constexpr uint8_t MAX_AUTOMATIC_ROUNDS = 2;

      /// Dispute voting window: 24h. Before the deadline only the fast path (a checksum reaching a
      /// majority of ALL live Tier-1 owners) can resolve; after it, the denominator relaxes to a
      /// quorum of cast votes plus a strict majority of cast votes.
      static constexpr uint32_t dispute_deadline_sec = 24 * 60 * 60;
   };

} // namespace sysio
