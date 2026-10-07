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

      /// Open an OPP envelope dispute. Called inline by `sysio.msgch::evalcons` for a terminal
      /// two-version tie or a post-boundary multi-version no-majority split. msgch owns the
      /// consensus boundary, terminality, strict-majority, and electorate-preflight checks because
      /// it alone has the live eligible group and delivery tally.
      /// Records the candidate checksums, snapshots the Tier-1 electorate (the Tier-1
      /// rows of `sysio.roa::nodeowners` for the current network generation) together with its
      /// quorum, and pauses epoch advancement until a Tier-1 node-owner vote resolves the
      /// canonical envelope. Defensively rejects direct calls when no Tier-1 node owner is
      /// registered: an empty-electorate dispute could never resolve and would hold the epoch
      /// paused forever.
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
      //  Underwriter-fault challenge (WIRE-297)
      // -----------------------------------------------------------------------
      //
      // The OPP envelope dispute's sibling. Same adjudication machinery — Tier-1 electorate
      // snapshotted at open, fixed quorum floor(N/2)+1, record-only votes, permissionless tally
      // crank, resolution through the `slashop` -> `opreg::slash` chokepoint — with the
      // differences the problem forces:
      //
      //   * A human FILES it (the depot can observe envelope divergence itself; a source-chain
      //     fault it cannot — someone must allege it), so filing is permissionless and priced
      //     with a challenger bond.
      //   * The ballot is a VERDICT (uphold / reject), not a choice among candidate versions.
      //   * It never pauses the epoch: an unresolved challenge is survivable — the challenged
      //     collateral lock simply lapses back to a normal release — so the chain keeps advancing.
      //   * The vote deadline is the commitment's own collateral-lock expiry, and there is NO
      //     after-deadline relaxed tally: a challenge "must be voted on before the window
      //     expires" (Jonathan, 2026-07-27); past it the challenge LAPSES with a full bond
      //     refund. Envelope disputes relax after their deadline only because a paused chain
      //     MUST eventually resolve.

      /// The fault a challenger alleges against the winning underwriter's commit. The council
      /// adjudicates the allegation against source-chain state (not visible to the depot) and
      /// votes; the enum classifies the case for the audit row and indexers.

      /// A Tier-1 voter's ballot in an underwriter challenge. The two reject flavours let the
      /// council separate an honest mistake (bond refunded to the challenger) from a frivolous or
      /// malicious challenge (bond forfeited to the wrongly-challenged underwriter) — forfeiture
      /// only ever happens by explicit council judgment, never by default.

      /// Terminal verdict of a challenge (NONE while it is OPEN).

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Dispute primary key (auto-incrementing id).
      struct dispute_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(dispute_key, (id))
      };

      /// OPP envelope dispute. Opened for an eligible post-boundary no-majority split with at least
      /// two versions for one (outpost, epoch): exactly two versions require every eligible operator
      /// to deliver, while a three-or-more-version split may open at the boundary. Resolved by a
      /// Tier-1 node-owner vote on the canonical checksum. The row is retained after resolution as
      /// the audit record (and as the guard that prevents re-opening the same (outpost, epoch)
      /// dispute).
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

      /// Challenge primary key (auto-incrementing id).

      /// An underwriter-fault challenge. Opened permissionlessly against one CONFIRMED
      /// commitment; resolved by a Tier-1 vote or lapsed at the lock window's end. The row
      /// survives resolution as the guard that a commitment is challenged at most once, ever
      /// (mirrors one-dispute-per-(outpost, epoch)) and as the verdict record.
      ///
      /// It does NOT survive at full size: `chkuwchal` compacts it to a fixed-width tombstone on
      /// resolution, clearing the two variable-length fields (`detail`, `electorate`) and erasing
      /// the challenge's ballot rows. Filing is permissionless and the bond comes back on every
      /// non-forfeit outcome, so recycled bond capital could otherwise pin unbounded
      /// caller-controlled bytes in RAM billed to `sysio`; compacting bounds the retained
      /// variable-length state by the number of CONCURRENTLY OPEN challenges — each backed by a
      /// live lock set and an escrowed bond — rather than by every challenge ever filed. The full
      /// filing and every ballot stay permanently readable in the action-trace history that
      /// indexers consume.

      /// Challenge-vote primary key (the voting Tier-1 owner). The vote table is scoped by
      /// `chal_id`, so the owner alone is unique within a challenge.

      /// One Tier-1 ballot in an underwriter challenge. Scoped by `chal_id`. Live only while the
      /// challenge is OPEN — it is the tally input and the one-vote-per-owner gate, and neither
      /// applies to a resolved challenge, so `chkuwchal` erases the scope on resolution.

      /// Claimable-bond primary key (the account the payout is owed to).

      /// WIRE this contract holds on an account's behalf out of a resolved challenge's escrow.
      /// `chkuwchal` credits it; `claimbond` pays it out and erases the row. Credits ACCUMULATE
      /// per account, so the table is bounded by the number of accounts with an unclaimed payout
      /// — not by the number of challenges ever resolved.

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
