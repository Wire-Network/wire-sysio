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
      enum class underwrite_fault_reason : uint8_t {
         SOURCE_DEPOSIT_MISSING  = 0, ///< the committed source deposit does not exist on the source chain
         SOURCE_DEPOSIT_MISMATCH = 1, ///< the deposit exists but its parameters do not match the commit
         NON_CANONICAL_ENVELOPE  = 2, ///< the commit rides an envelope later adjudicated non-canonical
      };

      /// A Tier-1 voter's ballot in an underwriter challenge. The two reject flavours let the
      /// council separate an honest mistake (bond refunded to the challenger) from a frivolous or
      /// malicious challenge (bond forfeited to the wrongly-challenged underwriter) — forfeiture
      /// only ever happens by explicit council judgment, never by default.
      enum class uwchal_ballot : uint8_t {
         UPHOLD         = 0, ///< fault proven — slash the underwriter; bond returns to the challenger
         REJECT_REFUND  = 1, ///< no fault; honest mistake — bond returns to the challenger
         REJECT_FORFEIT = 2, ///< no fault; frivolous or malicious — bond goes to the underwriter
      };

      /// Terminal verdict of a challenge (NONE while it is OPEN).
      enum class uwchal_verdict : uint8_t {
         NONE             = 0, ///< challenge still open
         UPHELD           = 1, ///< underwriter slashed; bond returned to the challenger
         REJECTED_REFUND  = 2, ///< council rejected — honest mistake; bond returned to the challenger
         REJECTED_FORFEIT = 3, ///< council rejected — frivolous; bond forfeited to the underwriter
         LAPSED           = 4, ///< lock window expired with no quorum — bond returned; nobody punished
      };

      /// Open a challenge against the WINNING underwriter of a CONFIRMED underwrite request,
      /// alleging its commit was faulty. Permissionless — anyone may file — but time-gated to the
      /// commitment's collateral-lock window and priced: the challenger escrows the WIRE value of
      /// the challenged locks as a bond (`uwchalbond` quotes it; both run one shared formula, so
      /// the quote can never drift from the charge — the `split_wire_fee` principle).
      ///
      /// Effects, in order: validates the commitment (uwreq CONFIRMED, `underwriter` is its
      /// winner, no prior challenge for this commitment — a verdict is final per commitment;
      /// other commitments by the same underwriter are independently challengeable), snapshots
      /// the Tier-1 electorate + quorum exactly as `opendispute` does, escrows the bond (inline
      /// `sysio.token::transfer` under the challenger's own authority), records the OPEN row, and
      /// marks the locks via `sysio.uwrit::holdlocks` — a held lock is NOT released at expiry
      /// while its challenge is open. uwrit re-validates lock liveness authoritatively, so a
      /// stale filing aborts whole, bond escrow included.
      ///
      /// `reason` is the numeric `underwrite_fault_reason` value (checked through
      /// `magic_enum::enum_cast` at this trust boundary); `detail` carries the challenger's
      /// free-text evidence context for the council (source tx ids, observed amounts, notes).
      [[sysio::action]]
      void openuwchal(name challenger,
                      uint64_t uwreq_id,
                      name underwriter,
                      uint8_t reason,
                      std::string detail);

      /// Cast a Tier-1 ballot in an open underwriter challenge. `ballot` is the numeric
      /// `uwchal_ballot` value. Same eligibility rules as `votedispute`: the voter must belong to
      /// the challenge's snapshotted electorate, one vote per owner. Record-only — the tally
      /// lives in `chkuwchal`, mirroring the votedispute/chkdispute split.
      [[sysio::action]]
      void voteuwchal(name owner, uint64_t chal_id, uint8_t ballot);

      /// Permissionless tally crank — `chkdispute`'s sibling. Nobody has to run it on a cadence:
      /// `sysio.uwrit::chklocks` (inlined from every `sysio.epoch::advance`) pokes it for any
      /// expired-but-challenged lock, which works precisely because the chain is NOT paused
      /// during an underwriter challenge. A manual call after the deciding vote resolves sooner.
      ///
      /// Resolution rules (N/Q = the snapshotted electorate size / quorum, fixed at open):
      ///   * UPHOLD ballots reach Q, any time — UPHELD: `slashop` the underwriter, then
      ///     `sysio.uwrit::sweeplocks` releases the now-deferred-slash locks (the slash lands
      ///     first — inline actions run depth-first in send order — so `opreg::releaselock`
      ///     takes its SLASHED branch, debiting the locked collateral and emitting the outbound
      ///     SLASH attestations); the bond returns to the challenger.
      ///   * REJECT ballots (both flavours combined) reach Q, any time — REJECTED: disposition by
      ///     majority among the rejectors, a tie favouring refund; `sysio.uwrit::freelocks`
      ///     clears the hold and the locks live out their natural window.
      ///   * `deadline_ms` (the locks' own expiry) passes with neither threshold met — LAPSED:
      ///     bond returned to the challenger, holds cleared, the locks release on the next
      ///     normal sweep. No verdict, nobody punished.
      [[sysio::action]]
      void chkuwchal(uint64_t chal_id);

      /// Read-only: the WIRE bond `openuwchal` would require RIGHT NOW to challenge this
      /// commitment — the winning underwriter's live lock amounts, each valued through its own
      /// reserve's current books (`opp::amm::token_to_wire`), summed. Returns 0 when the
      /// commitment is not currently challengeable (no CONFIRMED uwreq, `underwriter` is not its
      /// winner, a challenge already exists, or no live unheld locks) or when a leg cannot be
      /// priced. Shares its formula helper with `openuwchal`.
      [[sysio::action, sysio::read_only]]
      uint64_t uwchalbond(uint64_t uwreq_id, name underwriter);

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

      /// Challenge primary key (auto-incrementing id).
      struct uwchal_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(uwchal_key, (id))
      };

      /// An underwriter-fault challenge. Opened permissionlessly against one CONFIRMED
      /// commitment; resolved by a Tier-1 vote or lapsed at the lock window's end. The row is
      /// retained after resolution as the audit record AND as the guard that a commitment is
      /// challenged at most once, ever (mirrors one-dispute-per-(outpost, epoch)).
      struct [[sysio::table("uwchals")]] uwchal_entry {
         uint64_t                  id;
         uint64_t                  uwreq_id;         ///< the challenged commitment's uwreq
         name                      underwriter;      ///< the CONFIRMED winner under challenge
         name                      challenger;       ///< who filed and posted the bond
         underwrite_fault_reason   reason;           ///< alleged fault class
         std::string               detail;           ///< challenger's free-text evidence context
         opp::types::DisputeStatus status;           ///< OPEN / RESOLVED — same lifecycle enum as `disputes`
         uwchal_verdict            verdict;          ///< NONE while OPEN; the terminal outcome after
         uint64_t                  bond_amount = 0;  ///< WIRE the challenger escrowed (9-decimal units)
         /// The challenged locks' `expires_at_ms` — the vote deadline. Past it an unresolved
         /// challenge LAPSES (bond refunded); there is no after-deadline relaxed tally.
         uint64_t                  deadline_ms = 0;
         time_point                opened_at{};
         uint8_t                   network_gen = 0;  ///< roa network generation the electorate was drawn from
         /// The Tier-1 electorate + quorum, snapshotted at open — same discipline as
         /// `dispute_entry`: voter eligibility (`voteuwchal`) and the tally denominator
         /// (`chkuwchal`) come from one list frozen when the challenge opened, so registrations
         /// after open can neither join nor dilute it.
         std::vector<name>         electorate;
         uint32_t                  quorum = 0;

         /// Uniqueness: one challenge EVER per (uwreq, underwriter) commitment.
         uint128_t by_uwreq_underwriter() const {
            return (static_cast<uint128_t>(uwreq_id) << 64) | underwriter.value;
         }

         SYSLIB_SERIALIZE(uwchal_entry,
            (id)(uwreq_id)(underwriter)(challenger)(reason)(detail)(status)(verdict)
            (bond_amount)(deadline_ms)(opened_at)(network_gen)(electorate)(quorum))
      };

      using uwchals_t = sysio::kv::table<"uwchals"_n, uwchal_key, uwchal_entry,
         sysio::kv::index<"byuwrequw"_n,
            sysio::const_mem_fun<uwchal_entry, uint128_t, &uwchal_entry::by_uwreq_underwriter>>
      >;

      /// Challenge-vote primary key (the voting Tier-1 owner). The vote table is scoped by
      /// `chal_id`, so the owner alone is unique within a challenge.
      struct uwchal_vote_key {
         uint64_t owner;
         uint64_t primary_key() const { return owner; }
         SYSLIB_SERIALIZE(uwchal_vote_key, (owner))
      };

      /// One Tier-1 ballot in an underwriter challenge. Scoped by `chal_id`.
      struct [[sysio::table("uwchalvote")]] uwchal_vote {
         name          owner;
         uwchal_ballot ballot;
         time_point    voted_at{};

         SYSLIB_SERIALIZE(uwchal_vote, (owner)(ballot)(voted_at))
      };

      using uwchalvotes_t =
         sysio::kv::scoped_table<"uwchalvote"_n, uwchal_vote_key, uwchal_vote>;

   private:
      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name OPREG_ACCOUNT  = "sysio.opreg"_n;
      static constexpr name ROA_ACCOUNT    = "sysio.roa"_n;
      static constexpr name UWRIT_ACCOUNT  = "sysio.uwrit"_n;
      static constexpr name RESERV_ACCOUNT = "sysio.reserv"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;

      using DisputeStatus = opp::types::DisputeStatus;
      using NodeOwnerTier = opp::types::NodeOwnerTier;

      /// Dispute voting window: 24h. Before the deadline only the fast path (a checksum reaching a
      /// majority of the dispute's snapshotted electorate) can resolve; after it, the denominator
      /// relaxes to a quorum of cast votes plus a strict majority of cast votes.
      static constexpr uint32_t dispute_deadline_sec = 24 * 60 * 60;
   };

} // namespace sysio
