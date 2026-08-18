#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>
// For the OPP envelope budget — the roster ceiling below derives from it rather than
// restating a consensus-visible cap in a second place. Deliberately the shared
// opp.common header and NOT `sysio.msgch.hpp`: several contracts include this header
// without msgch on their include path.
#include <sysio.opp.common/opp_envelope_budget.hpp>

namespace sysio {

   class [[sysio::contract("sysio.epoch")]] epoch : public contract {
   public:
      using contract::contract;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set epoch configuration (privileged).
      [[sysio::action]]
      void setconfig(uint32_t epoch_duration_sec,
                     uint32_t operators_per_epoch,
                     uint32_t batch_operator_minimum_active,
                     uint32_t batch_op_groups,
                     uint32_t epoch_retention_envelope_log_count);

      /// Advance epoch if duration elapsed (permissionless crank).
      [[sysio::action]]
      void advance();

      /// Group assignment — reads AVAILABLE batch ops from sysio.opreg.
      [[sysio::action]]
      void schbatchgps();

      /// Set global pause (only callable by sysio.chalg).
      [[sysio::action]]
      void pause();

      /// Clear global pause (only callable by sysio.chalg).
      [[sysio::action]]
      void unpause();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Global epoch configuration singleton.
      struct [[sysio::table("epochcfg")]] epoch_config {
         uint32_t    epoch_duration_sec = 360;   // 6 minutes
         uint32_t    operators_per_epoch = 7;
         uint32_t    batch_operator_minimum_active = 21;
         uint32_t    batch_op_groups = 3;          // rotation groups (21 / 7)

         /// Cap multiplier for the metadata-only `envelope_log` table on
         /// `sysio.msgch`. Effective row cap is
         /// `active_outposts * 2 * epoch_retention_envelope_log_count`
         /// (one inbound + one outbound record per active outpost per
         /// epoch). Default 200 — matches the SOL/ETH per-direction
         /// metadata-log cap. Each `evalcons` consensus-reach + `buildenv`
         /// emit reads this directly; runtime changes via `setconfig`
         /// take effect on the next write.
         uint32_t    epoch_retention_envelope_log_count = 200;

         SYSLIB_SERIALIZE(epoch_config,
            (epoch_duration_sec)(operators_per_epoch)
            (batch_operator_minimum_active)(batch_op_groups)
            (epoch_retention_envelope_log_count))
      };

      using epochcfg_t = sysio::kv::global<"epochcfg"_n, epoch_config>;

      /// Current epoch state singleton.
      struct [[sysio::table("epochstate")]] epoch_state {
         uint32_t                          current_epoch_index = 0;
         time_point                        current_epoch_start{};
         time_point                        next_epoch_start{};
         uint8_t                           current_batch_op_group = 0; // 0, 1, or 2
         std::vector<std::vector<name>>    batch_op_groups;           // 3 groups of 7
         checksum256                       last_consensus_hash;
         bool                              is_paused = false;

         SYSLIB_SERIALIZE(epoch_state,
            (current_epoch_index)(current_epoch_start)(next_epoch_start)
            (current_batch_op_group)(batch_op_groups)(last_consensus_hash)(is_paused))
      };

      using epochstate_t = sysio::kv::global<"epochstate"_n, epoch_state>;

      /// Emissions readiness gate block log. One row per epoch_index that
      /// the gate has blocked from advancing. Inserted on the first gate
      /// failure for a given epoch; same-reason retries update last_retry_at
      /// and retry_count without re-broadcast. Pruned when the gate
      /// eventually passes for that epoch (advance proceeds normally).
      struct blocklog_key {
         uint64_t epoch_index;
         uint64_t primary_key() const { return epoch_index; }
         SYSLIB_SERIALIZE(blocklog_key, (epoch_index))
      };

      struct [[sysio::table("blocklog")]] blocklog_entry {
         uint32_t                              epoch_index        = 0;
         sysio::opp::types::EmissionsBlockReason reason           =
            sysio::opp::types::EMISSIONS_BLOCK_REASON_UNSPECIFIED;
         int64_t                               attempted_emission = 0;
         int64_t                               treasury_remaining = 0;
         int64_t                               sysio_balance      = 0;
         uint32_t                              first_blocked_at   = 0; // unix seconds
         uint32_t                              last_retry_at      = 0; // unix seconds
         uint32_t                              retry_count        = 0;

         SYSLIB_SERIALIZE(blocklog_entry,
            (epoch_index)(reason)(attempted_emission)(treasury_remaining)
            (sysio_balance)(first_blocked_at)(last_retry_at)(retry_count))
      };

      using blocklog_t = sysio::kv::table<"blocklog"_n, blocklog_key, blocklog_entry>;

      /// Digest of the last OPERATORS attestation queued for one outpost.
      ///
      /// `advance` re-derives the roster every epoch but queues it only when this
      /// digest changes, so a static roster costs zero envelope bytes. The roster
      /// changes on registration / activation / slash — rarely — while `advance`
      /// runs every epoch, and re-sending it bought nothing: an outpost cannot miss
      /// an envelope and continue (both outposts enforce strict epoch sequencing, so
      /// a missed envelope stalls rather than diverges), and an outpost that DROPS a
      /// roster does so on conditions that are pure functions of the payload, which a
      /// byte-identical re-send reproduces exactly.
      ///
      /// Per-outpost rather than global because a newly-activated outpost has never
      /// received the roster: the ABSENCE of a row is what makes its first `advance`
      /// send unconditionally. Rows are reused, never erased, mirroring
      /// `msgch::outpost_consensus_entry`.
      ///
      /// There is deliberately no periodic re-send. Every case in which an outpost
      /// could want a roster it was already sent is an explicit operational act that
      /// needs depot-side coordination regardless: an outpost re-init resets its epoch
      /// cursor alongside its registry, so it rejects every envelope until it is
      /// re-bootstrapped, and raising an outpost's own roster ceiling is a program
      /// upgrade with a runbook. A timer would serve those late while costing a send
      /// forever.
      struct roster_digest_key {
         uint64_t chain_code;
         uint64_t primary_key() const { return chain_code; }
         SYSLIB_SERIALIZE(roster_digest_key, (chain_code))
      };

      struct [[sysio::table("rosterdig")]] roster_digest_entry {
         uint64_t    chain_code    = 0;
         checksum256 digest        = {};
         uint32_t    sent_at_epoch = 0;

         SYSLIB_SERIALIZE(roster_digest_entry, (chain_code)(digest)(sent_at_epoch))
      };

      using rosterdig_t = sysio::kv::table<"rosterdig"_n, roster_digest_key, roster_digest_entry>;

      // Well-known accounts
      static constexpr name CHALG_ACCOUNT  = "sysio.chalg"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name OPREG_ACCOUNT  = "sysio.opreg"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;
      static constexpr name UWRIT_ACCOUNT  = "sysio.uwrit"_n;
      static constexpr name RESERV_ACCOUNT = "sysio.reserv"_n;

      /// Bounds on `epoch_duration_sec`. Floor is a typo-guard: well below this
      /// value, `expected_rounds` in sysio.system::payepoch falls back to 1
      /// for any non-trivial epoch, masking misconfig. Ceiling bounds the
      /// `(epoch_duration_sec * 2) / TOTAL_BLOCKS_PER_ROUND` arithmetic and
      /// prevents governance typo from setting a multi-year epoch.
      static constexpr uint32_t MIN_EPOCH_DURATION_SEC = 60;
      static constexpr uint32_t MAX_EPOCH_DURATION_SEC = 30u * 24u * 60u * 60u;

      /// Upper bound on the number of batch-operator groups. The active group is
      /// carried on-chain and in the batch_operator_plugin as a `uint8_t`, which
      /// reserves 255 as its "not in any group" sentinel. Groups are indexed
      /// 0..batch_op_groups-1, so capping at 255 keeps the largest index (254)
      /// below the sentinel and inside the byte, preventing index truncation or a
      /// collision that would miselect batch operators.
      static constexpr uint32_t MAX_BATCH_OP_GROUPS = 255;

      /// Upper bound on `operators_per_epoch` (the size of one schedule group).
      /// Every member of the expiring group costs two inline actions per active
      /// outpost in `advance` (opreg::recorddel + opreg::termcheck) plus an inline
      /// transfer in sysio.system::payepoch, so group size directly scales the
      /// epoch-boundary transaction; an absurd value (which the product equality
      /// alone cannot reject -- UINT32_MAX groups of one is internally consistent)
      /// would abort `advance` on its vector reserves and halt epoch advancement
      /// chain-wide. 100 mirrors sysio.system's producer-pay safety cap
      /// (MAX_STANDBY_END_RANK) and is an order of magnitude above any practical
      /// group size.
      static constexpr uint32_t MAX_OPERATORS_PER_EPOCH = 100;

      /// Upper bound on `batch_operator_minimum_active`, which the product
      /// equality pins to the total resident schedule window
      /// (operators_per_epoch * batch_op_groups). The full window is held in the
      /// `epochstate` singleton row, flattened into a resident vector on every
      /// `advance`, and serialized into the per-outpost BatchOperatorGroups
      /// attestation (~20 bytes per member against the 32 KiB OPP envelope cap).
      /// The ceiling is unchanged by the platform cap reduction 65 536 -> 32 768,
      /// but the headroom it leaves is materially tighter: at the ceiling the
      /// roster attestation alone is ~20 KB, about 62 % of a 32 KiB envelope
      /// (it was ~31 % of 64 KiB). 1000 members still keeps all three bounded --
      /// the resident window vector, the epoch-boundary transaction, and the
      /// attestation -- while far exceeding any practical roster; a roster that
      /// large would leave little room for value-bearing attestations in the same
      /// envelope, and `buildenv` would carry the remainder to the next epoch.
      static constexpr uint32_t MAX_SCHEDULED_BATCH_OPERATORS = 1000;

      /// Upper bound on the authex-linked chain addresses carried for ONE operator
      /// in the OPERATORS attestation.
      ///
      /// The per-operator address walk is otherwise unbounded — `authex::links` has
      /// no cap on how many keys one account may link — so a single account could
      /// inflate one roster entry without limit and push the attestation past the
      /// envelope on its own. An operator legitimately needs one address per
      /// registered outpost chain (two today: EVM + SVM), so 8 is generous headroom
      /// while still bounding the entry. It also matches what the Solana outpost
      /// already assumes when sizing its own pre-decode payload gate.
      static constexpr uint32_t MAX_OPERATOR_CHAIN_ADDRESSES = 8;

      // -----------------------------------------------------------------------
      //  OPERATORS roster ceiling
      // -----------------------------------------------------------------------
      //
      // `MAX_SCHEDULED_BATCH_OPERATORS` above does this arithmetic for the sibling
      // BATCH_OPERATOR_GROUPS attestation; it was never done for OPERATORS, which is
      // ~5x fatter per member and drawn from an UNBOUNDED registry rather than a
      // bounded schedule window. `sysio.opreg::setconfig` validates its summed
      // `max_available_*` ceilings against the result, so a governance change cannot
      // raise the registry past what an envelope can carry.

      /// Encoded bytes of one `ChainAddress`: 1 B tag + 1 B length + 2 B `kind` varint
      /// + 2 B inner tag/length + a 33-byte compressed secp256k1 key (Ed25519 is 32,
      /// so 33 is the worst case).
      static constexpr uint32_t ROSTER_BYTES_PER_ADDRESS = 39;

      /// Encoded bytes of one `OperatorEntry` carrying `addresses` chain addresses:
      /// a maximum-length WIRE account name (4 B framing + 13 B) + its addresses
      /// + 2 B `type` + 3 B `status` (SLASHED is 241, a two-byte varint) + 2 B for the
      /// repeated-field framing in `Operators`.
      ///
      /// Parameterised on the address count rather than pinned to today's outpost set:
      /// an operator carries one address per registered outpost, so registering a third
      /// outpost makes every entry ~39 B fatter. A fixed constant would silently
      /// overstate capacity the moment that happens.
      static constexpr uint32_t roster_bytes_per_operator(uint32_t addresses) {
         return 17 + addresses * ROSTER_BYTES_PER_ADDRESS + 2 + 3 + 2;
      }

      /// The share of one envelope the roster may claim.
      ///
      /// Sizing the ceiling against the WHOLE budget would let a legal configuration
      /// produce a roster that fills an envelope by itself — the exact hazard
      /// `MAX_SCHEDULED_BATCH_OPERATORS` names ("little room for value-bearing
      /// attestations in the same envelope"). Half leaves the remainder for
      /// BATCH_OPERATOR_GROUPS and the SWAP_REMIT / WITHDRAW_REMIT / RESERVE_READY
      /// traffic that settles user value, which must not be deferred behind the roster.
      static constexpr uint32_t ROSTER_ENVELOPE_SHARE_DIVISOR = 2;

      /// Strictest OUTPOST-side ceiling on roster ENTRIES.
      ///
      /// The depot's envelope arithmetic is necessary but not sufficient: an outpost
      /// that cannot seat the roster it receives skips the WHOLE attestation and keeps
      /// its previous one, so the roster silently stops tracking the depot. The Solana
      /// outpost's `OperatorRegistry` is a fixed-size PDA — 32 entries today, raised to
      /// 128 by wire-solana#442 (SOL-385), which explicitly defers the depot-side number
      /// to WIRE-342. Ethereum applies no count cap, so Solana is the binding one.
      ///
      /// Keep in lock-step with `liqsol-core`'s `MAX_OPERATORS`; this bound is only
      /// meaningful while it is the smaller of the two.
      static constexpr uint32_t MAX_OUTPOST_ROSTER_ENTRIES = 128;

      /// Roster ceiling for a network carrying `outpost_count` active outposts: the
      /// smaller of what half an envelope can carry and what the strictest outpost can
      /// seat. Both bounds are real and neither implies the other — bytes gate what the
      /// depot can SEND, entries gate what an outpost can STORE.
      static constexpr uint32_t max_roster_operators(uint32_t outpost_count) {
         const uint32_t addresses =
            outpost_count == 0 ? 1
                               : (outpost_count > MAX_OPERATOR_CHAIN_ADDRESSES
                                     ? MAX_OPERATOR_CHAIN_ADDRESSES : outpost_count);
         const uint32_t by_bytes =
            static_cast<uint32_t>(sysio::opp::SINGLE_ATTESTATION_BUDGET_BYTES
                                  / ROSTER_ENVELOPE_SHARE_DIVISOR
                                  / roster_bytes_per_operator(addresses));
         return by_bytes < MAX_OUTPOST_ROSTER_ENTRIES ? by_bytes : MAX_OUTPOST_ROSTER_ENTRIES;
      }

   private:

      // Namespace alias for OPP protobuf enum types
      using OperatorType   = sysio::opp::types::OperatorType;
      using OperatorStatus = sysio::opp::types::OperatorStatus;
   };

   /// Asserts the caller signed the transaction and is a member of the current
   /// resident batch-operator group. This is a group-membership check only: the
   /// group is a snapshot from the last schedule, so current operator eligibility
   /// (sysio.opreg ACTIVE status) must be enforced separately at the delivery
   /// call site (see msgch::deliver).
   inline void is_batch_operator_active(const name& batch_op_name) {
      require_auth(batch_op_name);
      epoch::epochstate_t epoch_tbl(epoch::EPOCH_ACCOUNT);
      check(epoch_tbl.exists(), "epoch state not initialized");
      auto state = epoch_tbl.get();

      auto cur_group = state.current_batch_op_group;
      check(cur_group < state.batch_op_groups.size(), "active group index out of range");
      auto& active_members = state.batch_op_groups[cur_group];
      check(
         std::find(active_members.begin(), active_members.end(), batch_op_name) != active_members.end(),
         "caller is not in the active batch operator group"
      );
   }

} // namespace sysio
