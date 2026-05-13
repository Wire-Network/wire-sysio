#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <fc-lite/crypto/chain_types.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

namespace sysio {

   /**
    * @brief sysio.uwrit — underwriter race resolver + flat lock vector.
    *
    * Per the corrected ledger model in
    * `CLAUDE-WIRE-OPERATOR-COLLATERAL-IMPL-PLAN.md`:
    *
    * - opreg owns the bond ledger (per-(operator, chain, token_kind) aggregate
    *   balance). uwrit owns the **lock vector** — one row per leg of every
    *   in-flight UWREQ. opreg's `available()` rollup reads this table via a
    *   mirror to subtract active locks from the operator's spendable balance.
    *
    * - On `UNDERWRITE_INTENT_COMMIT` arrival (one per outpost; underwriters
    *   call `commit(...)` JSON-RPC on each side), `record_commit` registers
    *   the per-leg arrival in `uwreqs.commits_by`. When BOTH legs land for
    *   the same underwriter, `try_select_winner` checks the underwriter's
    *   `sysio.opreg::available(...)` for each chain; if both legs are
    *   covered, the underwriter wins, two rows are pushed onto `locks`, and
    *   a `REMIT` is queued to the destination outpost.
    *
    * - opreg.balance is **not** mutated when a lock is added — the lock
    *   simply reduces what `available()` rolls up. When a lock releases:
    *     * SLASHED underwriter   — opreg::releaselock decrements balance
    *                               and emits SLASH_OPERATOR (deferred-slash).
    *     * TERMINATED underwriter — opreg::releaselock decrements balance
    *                               and emits WITHDRAW_REMIT to authex
    *                               destination (deferred-remit).
    *     * Healthy underwriter   — no opreg call; freed amount naturally
    *                               reappears in `available()` once the lock
    *                               row is erased.
    */
   class [[sysio::contract("sysio.uwrit")]] uwrit : public contract {
   public:
      using contract::contract;

      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT = "sysio.msgch"_n;
      static constexpr name OPREG_ACCOUNT = "sysio.opreg"_n;
      static constexpr name CHALG_ACCOUNT = "sysio.chalg"_n;
      static constexpr name RESERVE_ACCOUNT = "sysio.reserv"_n;

      // Number of epochs an UWREQ row lives after settlement / abort. 10 epochs
      // matches the bootstrap doc's "losers retained 10 epochs for debugging"
      // requirement; covers SETTLED, REVERTED, EXPIRED uwreqs alike.
      static constexpr uint32_t UWREQ_RETENTION_EPOCHS = 10;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set underwriting fee config. Simplified vs the legacy 4-step model —
      /// fee distribution is deferred to a later task; for now this just
      /// holds the per-spoke fee_bps that the depot applies.
      [[sysio::action]]
      void setconfig(uint32_t fee_bps);

      /// Called inline from `sysio.msgch::dispatch` when a SWAP attestation
      /// arrives. Decodes the SwapRequest, runs the variance-tolerance check
      /// against `sysio.reserve::quote` (skipped when no LP is provisioned
      /// for the (chain, token) pair), and either:
      ///   * creates an OPEN UWREQ with src/dst populated from the swap, or
      ///   * emits a SWAP_REVERT back to `outpost_id` and skips UWREQ creation
      ///     when the gap between quoted_destination_amount and the depot's
      ///     current quote exceeds `quote_tolerance_bps`.
      ///
      /// `outpost_id` is the source outpost the SWAP came from — needed so
      /// the SWAP_REVERT routes back to the user's depositing outpost on
      /// variance failure.
      [[sysio::action]]
      void createuwreq(uint64_t attestation_id,
                       opp::types::AttestationType type,
                       uint64_t outpost_id,
                       std::vector<char> data);

      /// Called inline from `sysio.msgch::dispatch` when an
      /// UNDERWRITE_INTENT_COMMIT attestation arrives. Records the per-leg
      /// arrival in `uwreqs.commits_by`. When both legs land for the same
      /// underwriter, runs `try_select_winner` to resolve the race.
      [[sysio::action]]
      void rcrdcommit(uint64_t uwreq_id,
                      name underwriter,
                      uint64_t outpost_id,
                      opp::types::ChainKind from_chain);

      /// Called inline from `sysio.msgch::dispatch` when an
      /// UNDERWRITE_INTENT_REJECT attestation arrives. Marks the
      /// underwriter's race entry as REJECTED.
      [[sysio::action]]
      void rcrdreject(uint64_t uwreq_id, name underwriter, std::string reason);

      /// Settle an UWREQ. For each lock entry: erase the row and call
      /// opreg::releaselock so opreg can deferred-slash / deferred-remit /
      /// no-op based on the underwriter's current status. The UWREQ row
      /// itself transitions to SETTLED with `expires_at_epoch = now + 10`
      /// for off-chain debug retention.
      ///
      /// auth=self: invoked inline from msgch dispatch on REMIT_CONFIRM /
      /// SWAP_REVERT, or from `expirelock` when a lock has aged past
      /// the unlock deadline.
      [[sysio::action]]
      void release(uint64_t uwreq_id);

      /// Permissionless: trigger `release(uwreq_id)` if the UWREQ has been
      /// COMMITTED for longer than its unlock deadline without settlement.
      /// Used by watchdog scripts / cron to clear stale locks; the deadline
      /// is intentionally generous to give the destination outpost time to
      /// confirm REMIT.
      [[sysio::action]]
      void expirelock(uint64_t uwreq_id);

      /// Read-only rollup of an underwriter's active lock total on a given
      /// (chain, token_kind). Used by off-chain consumers + (eventually)
      /// other contracts that don't rely on opreg's mirror.
      [[sysio::action, sysio::read_only]]
      uint64_t sumlocks(name underwriter,
                        opp::types::ChainKind chain,
                        opp::types::TokenKind token_kind);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Auto-incrementing id-keyed primary key used by `uwreqs`.
      struct id_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(id_key, (id))
      };

      /// Per-leg lock row. The (underwriter, chain, token_kind) composite is
      /// the indexing key opreg's `available()` rollup uses (cross-contract
      /// kv::table read of `sysio::uwrit::locks_t` from `sysio.opreg`). Rows
      /// are pushed by `try_select_winner` and erased by `release`.
      struct lock_key {
         uint64_t lock_id;
         uint64_t primary_key() const { return lock_id; }
         SYSLIB_SERIALIZE(lock_key, (lock_id))
      };

      struct [[sysio::table("locks")]] lock_entry {
         uint64_t                lock_id          = 0;
         uint64_t                uwreq_id         = 0;
         name                    underwriter;
         opp::types::ChainKind   chain;
         opp::types::TokenKind   token_kind;
         uint64_t                amount           = 0;
         uint32_t                created_at_epoch = 0;

         /// Composite index for opreg's `available()` rollup: 64 bits
         /// underwriter + 32 chain + 32 token_kind.
         uint128_t by_underwriter_ck() const {
            return (static_cast<uint128_t>(underwriter.value) << 64)
                 | (static_cast<uint64_t>(chain) << 32)
                 | static_cast<uint64_t>(token_kind);
         }
         uint64_t by_uwreq() const { return uwreq_id; }

         SYSLIB_SERIALIZE(lock_entry,
            (lock_id)(uwreq_id)(underwriter)(chain)(token_kind)
            (amount)(created_at_epoch))
      };

      using locks_t = sysio::kv::table<"locks"_n, lock_key, lock_entry,
         sysio::kv::index<"byuwck"_n,
            sysio::const_mem_fun<lock_entry, uint128_t, &lock_entry::by_underwriter_ck>>,
         sysio::kv::index<"byuwreq"_n,
            sysio::const_mem_fun<lock_entry, uint64_t, &lock_entry::by_uwreq>>
      >;

      /// Per-underwriter race entry inside an UWREQ row. Tracks when each
      /// leg of a dual-COMMIT pair arrived so `try_select_winner` can
      /// resolve the race deterministically.
      struct commit_entry {
         name      underwriter;
         uint64_t  source_received_at_ms = 0;
         uint64_t  dest_received_at_ms   = 0;
         /// Race outcome — INTENT_SUBMITTED (initial), INTENT_CONFIRMED
         /// (winner), SLASHED (rejected for insufficient bond), or RELEASED
         /// (loser, kept for debugging). Reuses the existing protobuf
         /// UnderwriteStatus enum.
         opp::types::UnderwriteStatus status = opp::types::UNDERWRITE_STATUS_INTENT_SUBMITTED;
         std::string reason;

         SYSLIB_SERIALIZE(commit_entry,
            (underwriter)(source_received_at_ms)(dest_received_at_ms)(status)(reason))
      };

      /// UWREQ row — one per inbound SWAP attestation. Tracks the swap's
      /// src/dst pairs, the underwriter race, and the eventual settlement.
      struct [[sysio::table("uwreqs")]] uw_request_t {
         uint64_t                                id;
         opp::types::AttestationType             type;
         opp::types::UnderwriteRequestStatus     status;

         /// Src / dst of the cross-chain swap. Populated by `createuwreq`
         /// from the decoded SwapRequest. Used by `try_select_winner` to
         /// validate per-leg bond coverage.
         opp::types::ChainKind                   src_chain;
         opp::types::TokenKind                   src_token_kind;
         uint64_t                                src_amount        = 0;
         opp::types::ChainKind                   dst_chain;
         opp::types::TokenKind                   dst_token_kind;
         uint64_t                                dst_amount        = 0;

         /// Race state.
         std::vector<commit_entry>               commits_by;
         name                                    winner;
         uint64_t                                committed_at_ms   = 0;
         uint64_t                                settled_at_ms     = 0;
         /// Epoch after which this row is eligible for prune (kept for
         /// debugging; see `UWREQ_RETENTION_EPOCHS`).
         uint32_t                                expires_at_epoch  = 0;

         /// Inbound attestation payload (zpp_bits-encoded protobuf).
         std::vector<char>                       attestation_inbound_data;

         /// Outbound attestation payload reserved for future flows where
         /// uwrit emits its own response (e.g. underwriter intent acks).
         /// Empty until that flow lands.
         std::vector<char>                       attestation_outbound_data;

         uint64_t by_status() const { return static_cast<uint64_t>(status); }
         uint64_t by_winner() const { return winner.value; }

         SYSLIB_SERIALIZE(uw_request_t,
            (id)(type)(status)
            (src_chain)(src_token_kind)(src_amount)
            (dst_chain)(dst_token_kind)(dst_amount)
            (commits_by)(winner)(committed_at_ms)(settled_at_ms)(expires_at_epoch)
            (attestation_inbound_data)(attestation_outbound_data))
      };

      using uwreqs_t = sysio::kv::table<"uwreqs"_n, id_key, uw_request_t,
         sysio::kv::index<"bystatus"_n,
            sysio::const_mem_fun<uw_request_t, uint64_t, &uw_request_t::by_status>>,
         sysio::kv::index<"bywinner"_n,
            sysio::const_mem_fun<uw_request_t, uint64_t, &uw_request_t::by_winner>>
      >;

      /// Singleton holding the next-issued `lock_id`. Keeps the auto-
      /// increment monotonic across action calls.
      struct [[sysio::table("uwcounters")]] uw_counters {
         uint64_t next_lock_id = 1;
         SYSLIB_SERIALIZE(uw_counters, (next_lock_id))
      };

      using uwcounters_t = sysio::kv::global<"uwcounters"_n, uw_counters>;

      /// Fee configuration singleton. Held over from the legacy contract;
      /// fee distribution itself is deferred to a follow-up task.
      struct [[sysio::table("uwconfig")]] uw_config {
         uint32_t fee_bps = 10;   // 0.1% per spoke
         SYSLIB_SERIALIZE(uw_config, (fee_bps))
      };

      using uwconfig_t = sysio::kv::global<"uwconfig"_n, uw_config>;

   private:

      using UnderwriteRequestStatus = opp::types::UnderwriteRequestStatus;
      using UnderwriteStatus        = opp::types::UnderwriteStatus;
      using ChainKind               = opp::types::ChainKind;
      using TokenKind               = opp::types::TokenKind;
      using AttestationType         = opp::types::AttestationType;
   };

} // namespace sysio
