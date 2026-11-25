#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_keys.hpp>

namespace sysio {

   namespace chalg_limits {
      /// Minimum number of distinct envelope versions required to make a consensus split
      /// adjudicable by the Tier-1 dispute vote.
      inline constexpr uint32_t minimum_dispute_candidate_versions = 2;
   } // namespace chalg_limits

   class [[sysio::contract("sysio.chalg")]] chalg : public contract {
   public:
      using contract::contract;

      /// One candidate envelope version in an OPP dispute: a distinct delivered checksum and the
      /// batch operators that delivered it. Used both as the `opendispute` action payload and as
      /// the per-row record on `dispute_entry::candidates`.
      struct dispute_candidate {
         checksum256       checksum;   ///< sha256 of the candidate envelope bytes
         /// Batch operators that delivered this checksum when the dispute opened. Audit / off-chain
         /// only: the winner's acceptance in `sysio.msgch` slashes by comparing each recorded delivery's
         /// checksum with the winner, not by this list, which is never read back on chain.
         std::vector<name> operators;

         SYSLIB_SERIALIZE(dispute_candidate, (checksum)(operators))
      };

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Execute a slash on an operator. Auth: sysio.chalg or sysio.msgch, which slashes every
      /// non-canonical OPP envelope deliverer of an epoch when it accepts the epoch's winner, by
      /// consensus or by dispute resolution.
      [[sysio::action]]
      void slashop(name operator_acct, std::string reason);

      // -----------------------------------------------------------------------
      //  OPP envelope dispute vote (Tier-1 node-owner resolution)
      // -----------------------------------------------------------------------

      /// Open an OPP envelope dispute. Called inline by `sysio.msgch::evalcons` once no delivered
      /// version can still reach a strict majority of the eligible group, or for a split past the
      /// epoch's idle dispute fallback with fewer than a majority silent. msgch owns those checks and
      /// the electorate preflight because it alone has the live eligible group and delivery tally.
      /// Records the candidate checksums, snapshots the Tier-1 electorate (the Tier-1
      /// rows of `sysio.roa::nodeowners` for the current network generation) together with its
      /// quorum, and pauses the disputed chain's epoch advancement until a Tier-1 node-owner vote
      /// resolves the canonical envelope. Defensively rejects direct calls when no Tier-1 node owner
      /// is registered: an empty-electorate dispute could never resolve and would hold the chain's
      /// epoch paused forever.
      [[sysio::action]]
      void opendispute(uint64_t chain_code,
                       uint32_t epoch_index,
                       std::vector<dispute_candidate> candidates);

      /// Cast a Tier-1 node-owner vote for the canonical envelope checksum in an open dispute.
      /// `owner` must be a member of the dispute's snapshotted electorate (Tier-1 node owners at
      /// the time the dispute opened -- later registrations cannot join an in-flight dispute);
      /// `chosen_checksum` must be one of the dispute's candidate checksums. One vote per owner.
      /// Ballots are accepted, past the deadline too, only while the tally has no winner.
      [[sysio::action]]
      void votedispute(name owner, uint64_t dispute_id, checksum256 chosen_checksum);

      /// Permissionless crank: tally the votes for an open dispute against its snapshotted
      /// electorate of N owners and fixed quorum Q = floor(N/2)+1. When the threshold is met
      /// (fast path: a checksum reaches Q any time; after the 24h deadline: quorum of cast votes
      /// AND a strict majority of cast votes), record the winning checksum, dispatch it via
      /// `sysio.msgch::resolvedisp`, and unpause the disputed chain's epoch. The winner's acceptance
      /// in `resolvedisp` slashes the losers.
      [[sysio::action]]
      void chkdispute(uint64_t dispute_id);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Dispute primary key (auto-incrementing id).
      struct dispute_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(dispute_key, (id))
      };

      /// OPP envelope dispute. Opened for one (outpost, epoch) once no delivered version can still
      /// reach a strict majority of the eligible group with the eligible operators still silent (so
      /// at least two versions), or for a split of at least two versions, with fewer than a majority
      /// silent, past the epoch's idle dispute fallback. Resolved by a Tier-1 node-owner vote on the
      /// canonical checksum. The row is retained after resolution as the audit record (and as the
      /// guard that prevents re-opening the same (outpost, epoch) dispute). A chain holds at most one
      /// OPEN dispute: opening one pauses the chain's epoch, and deliveries bind to the chain's current
      /// epoch.
      struct [[sysio::table("disputes")]] dispute_entry {
         uint64_t                       id;
         uint64_t                       chain_code;        ///< outpost slug_name value
         uint32_t                       epoch_index;
         opp::types::DisputeStatus      status;
         checksum256                    winning_checksum;  ///< zero until RESOLVED
         time_point                     opened_at{};
         time_point                     deadline{};        ///< opened_at + dispute_deadline_sec
         std::vector<dispute_candidate> candidates;
         uint8_t                        network_gen = 0;   ///< roa network generation the electorate was drawn from
         /// The Tier-1 node owners eligible to vote, snapshotted from `sysio.roa::nodeowners`
         /// (scope `network_gen`) when the dispute opened. Voter eligibility (`votedispute`) and
         /// the tally denominator (`chkdispute`) both come from this one list, so eligibility and
         /// quorum can never derive from diverging registries, and registrations after open can
         /// neither join nor dilute an in-flight dispute.
         std::vector<name>              electorate;
         uint32_t                       quorum = 0;        ///< floor(electorate.size()/2)+1, fixed at open

         /// `byoutepoch` key: one dispute per (outpost, epoch).
         uint128_t by_outpost_epoch() const {
            return opp::outpost_epoch_key(chain_code, epoch_index);
         }

         SYSLIB_SERIALIZE(dispute_entry,
            (id)(chain_code)(epoch_index)(status)(winning_checksum)
            (opened_at)(deadline)(candidates)(network_gen)(electorate)(quorum))
      };

      using disputes_t = sysio::kv::table<"disputes"_n, dispute_key, dispute_entry,
         sysio::kv::index<"byoutepoch"_n,
            sysio::const_mem_fun<dispute_entry, uint128_t, &dispute_entry::by_outpost_epoch>>
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
      static constexpr name ROA_ACCOUNT    = "sysio.roa"_n;

      using DisputeStatus = opp::types::DisputeStatus;
      using NodeOwnerTier = opp::types::NodeOwnerTier;

      /// Dispute voting window: 24h. Before the deadline only the fast path (a checksum reaching a
      /// majority of the dispute's snapshotted electorate) can resolve; after it, the denominator
      /// relaxes to a quorum of cast votes plus a strict majority of cast votes.
      static constexpr uint32_t dispute_deadline_sec = 24 * 60 * 60;

   };

} // namespace sysio
