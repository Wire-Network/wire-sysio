#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_keys.hpp>

namespace sysio {

   class [[sysio::contract("sysio.chalg")]] chalg : public contract {
   public:
      using contract::contract;

      /// One candidate envelope version in an OPP dispute: a distinct delivered checksum and the
      /// batch operators that delivered it. Used both as the `opendispute` action payload and as
      /// the per-row record on `dispute_entry::candidates`.
      struct dispute_candidate {
         checksum256       checksum;   ///< sha256 of the candidate envelope bytes
         /// Batch operators that delivered this checksum. Audit / off-chain only — the on-chain
         /// slash path classifies via `outpcons.winning_checksum` vs each delivered checksum in
         /// `sysio.epoch::advance`, not this list; it is never read back on chain.
         std::vector<name> operators;

         SYSLIB_SERIALIZE(dispute_candidate, (checksum)(operators))
      };

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Execute a slash on an operator. Auth: sysio.chalg (dispute resolution) or sysio.epoch
      /// (the single-path slash of non-canonical OPP envelope deliverers at epoch close).
      [[sysio::action]]
      void slashop(name operator_acct, std::string reason);

      // -----------------------------------------------------------------------
      //  OPP envelope dispute vote (Tier-1 node-owner resolution)
      // -----------------------------------------------------------------------

      /// Open an OPP envelope dispute. Called inline by `sysio.msgch::evalcons` when the active
      /// batch operators delivered 3+ distinct envelope versions for one (outpost, epoch) with no
      /// majority. Records the candidate checksums, snapshots the Tier-1 electorate (the Tier-1
      /// rows of `sysio.roa::nodeowners` for the current network generation) together with its
      /// quorum, and pauses epoch advancement until a Tier-1 node-owner vote resolves the
      /// canonical envelope. Rejects opening when no Tier-1 node owner is registered: an
      /// empty-electorate dispute could never resolve and would hold the epoch paused forever.
      [[sysio::action]]
      void opendispute(uint64_t chain_code,
                       uint32_t epoch_index,
                       std::vector<dispute_candidate> candidates);

      /// Cast a Tier-1 node-owner vote for the canonical envelope checksum in an open dispute.
      /// `owner` must be a member of the dispute's snapshotted electorate (Tier-1 node owners at
      /// the time the dispute opened -- later registrations cannot join an in-flight dispute);
      /// `chosen_checksum` must be one of the dispute's candidate checksums. One vote per owner.
      [[sysio::action]]
      void votedispute(name owner, uint64_t dispute_id, checksum256 chosen_checksum);

      /// Permissionless crank: tally the votes for an open dispute against its snapshotted
      /// electorate of N owners and fixed quorum Q = floor(N/2)+1. When the threshold is met
      /// (fast path: a checksum reaches Q any time; after the 24h deadline: quorum of cast votes
      /// AND a strict majority of cast votes), record the winning checksum, dispatch it via
      /// `sysio.msgch::resolvedisp`, and -- when this was the last open dispute -- unpause the
      /// epoch.
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
         uint8_t                        network_gen = 0;   ///< roa network generation the electorate was drawn from
         /// The Tier-1 node owners eligible to vote, snapshotted from `sysio.roa::nodeowners`
         /// (scope `network_gen`) when the dispute opened. Voter eligibility (`votedispute`) and
         /// the tally denominator (`chkdispute`) both come from this one list, so eligibility and
         /// quorum can never derive from diverging registries, and registrations after open can
         /// neither join nor dilute an in-flight dispute.
         std::vector<name>              electorate;
         uint32_t                       quorum = 0;        ///< floor(electorate.size()/2)+1, fixed at open

         uint64_t by_epoch() const { return epoch_index; }
         uint128_t by_outpost_epoch() const {
            return opp::outpost_epoch_key(chain_code, epoch_index);
         }

         SYSLIB_SERIALIZE(dispute_entry,
            (id)(chain_code)(epoch_index)(status)(winning_checksum)
            (opened_at)(deadline)(candidates)(network_gen)(electorate)(quorum))
      };

      using disputes_t = sysio::kv::table<"disputes"_n, dispute_key, dispute_entry,
         sysio::kv::index<"byepoch"_n,
            sysio::const_mem_fun<dispute_entry, uint64_t, &dispute_entry::by_epoch>>,
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

      /// Contract-global dispute bookkeeping. `open_disputes` counts the disputes currently in
      /// `DISPUTE_STATUS_OPEN` across all (outpost, epoch) keys: `opendispute` increments it and
      /// `chkdispute` decrements it on resolution, sending `sysio.epoch::unpause` only when it
      /// reaches zero. `sysio.epoch::is_paused` is a single flag, so resolving one of several
      /// concurrently open disputes must not resume epoch advancement while another is still open.
      struct [[sysio::table("chalgstate")]] chalg_state {
         uint32_t open_disputes = 0;

         SYSLIB_SERIALIZE(chalg_state, (open_disputes))
      };

      using chalgstate_t = sysio::kv::global<"chalgstate"_n, chalg_state>;

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
