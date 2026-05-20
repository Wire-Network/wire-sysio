#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <fc-lite/crypto/chain_types.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/slug_name.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

namespace sysio {

   /**
    * @brief sysio.uwrit — underwriter race resolver + flat lock vector.
    *
    * Per the v6 data-model refactor (`load-context-and-follow-smooth-flame.md`
    * §3.13, §4.5, §4.6):
    *
    * - opreg owns the bond ledger (per-(operator, chain_code, token_code) aggregate
    *   balance). uwrit owns the **lock vector** — one row per leg of every
    *   in-flight UWREQ. opreg's `available()` rollup reads this table via a
    *   mirror to subtract active locks from the operator's spendable balance.
    *
    * - Identity has been rekeyed onto `sysio::slug_name` (uint64). Each
    *   `lock_entry` carries `(chain_code, token_code, reserve_code)`; the
    *   `reserve_code` records which specific reserve this leg is bound to
    *   so a slash-to-reserve hop on a same-(chain, token) pair with
    *   multiple reserves can route unambiguously. `uw_request_t` carries
    *   `src_*` and `dst_*` slug_name triples for the same reason.
    *
    * - The per-underwriter composite lock index can no longer fit in a
    *   `uint128_t` (3 × uint64 = 192 bits). It is split into two secondary
    *   indexes per the plan's B.2 design:
    *     * `byuwck`         — `checksum256(account || chain_code || token_code)`
    *                          for the per-(chain, token) rollup that opreg's
    *                          `available()` reads.
    *     * `byunderwriter` — uint64 split-index keyed on `underwriter.value`
    *                          for cheap per-operator scans (in-memory filter
    *                          on chain_code / token_code / reserve_code).
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
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name OPREG_ACCOUNT = "sysio.opreg"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;
      static constexpr name CHALG_ACCOUNT = "sysio.chalg"_n;
      static constexpr name RESERVE_ACCOUNT = "sysio.reserv"_n;

      // Number of epochs an UWREQ row lives after settlement / abort. 10 epochs
      // matches the bootstrap doc's "losers retained 10 epochs for debugging"
      // requirement; covers SETTLED, REVERTED, EXPIRED uwreqs alike.
      static constexpr uint32_t UWREQ_RETENTION_EPOCHS = 10;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set underwriting fee + lock config. Fields:
      ///   * `fee_bps` — per-spoke fee charged by the depot.
      ///   * `collateral_lock_duration_epoch_count` — number of epochs after
      ///     `lock_entry.created_at_epoch` that the lock auto-expires (swept
      ///     by `sysio.epoch::advance -> chklocks`).
      ///   * `fee_split_winner_pct` / `fee_split_other_uw_pct` /
      ///     `fee_split_batch_op_pct` — distribution shares (sum to 100).
      ///     Distribution logic itself is deferred to a follow-up; today
      ///     these fields are persisted but not read by any code path.
      [[sysio::action]]
      void setconfig(uint32_t fee_bps,
                     uint32_t collateral_lock_duration_epoch_count,
                     uint8_t  fee_split_winner_pct,
                     uint8_t  fee_split_other_uw_pct,
                     uint8_t  fee_split_batch_op_pct);

      /// Called inline from `sysio.msgch::dispatch` when a SWAP attestation
      /// arrives. Decodes the SwapRequest, runs the variance-tolerance check
      /// against `sysio.reserve::swapquote` (skipped when no LP is provisioned
      /// for the relevant reserves), and either:
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
      /// arrival in `uwreqs.commits_by` and stores the verbatim UIC bytes
      /// so `try_select_winner` can reconstruct + verify the digest. When
      /// both legs land for the same underwriter, runs `try_select_winner`
      /// to resolve the race.
      ///
      /// `(from_chain_code, from_token_code, reserve_code)` together identify
      /// which leg of the swap this UIC covers. Same-chain swaps with
      /// multiple reserves of the same `(chain, token)` need all three
      /// codes to disambiguate src vs dst.
      ///
      /// `uic_bytes` is the raw zpp_bits-encoded `UnderwriteIntentCommit`
      /// payload — the action signature carries bytes, not the proto
      /// message itself, per `feedback_no_proto_messages_in_actions.md`.
      [[sysio::action]]
      void rcrdcommit(uint64_t uwreq_id,
                      name underwriter,
                      uint64_t outpost_id,
                      sysio::slug_name from_chain_code,
                      sysio::slug_name from_token_code,
                      sysio::slug_name reserve_code,
                      std::vector<char> uic_bytes);

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

      /// Sweep all `locks` rows whose `expires_at_epoch <= up_to_epoch`. Inlined
      /// from `sysio.epoch::advance` at every epoch boundary; can also be
      /// invoked by `sysio.uwrit` itself for manual cleanup. The
      /// `byexpire` secondary index walks rows in ascending expiry, so the
      /// loop stops at the first row that hasn't expired yet — the steady-
      /// state cost is O(n) in the number of locks expiring this epoch, not
      /// table size.
      [[sysio::action]]
      void chklocks(uint32_t up_to_epoch);

      /// Read-only rollup of an underwriter's active lock total on a given
      /// `(chain_code, token_code)`. Used by off-chain consumers + (eventually)
      /// other contracts that don't rely on opreg's mirror.
      [[sysio::action, sysio::read_only]]
      uint64_t sumlocks(name underwriter,
                        sysio::slug_name chain_code,
                        sysio::slug_name token_code);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Auto-incrementing id-keyed primary key used by `uwreqs`.
      struct id_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(id_key, (id))
      };

      /// Per-leg lock row. Rows are pushed by `try_select_winner` and erased
      /// by `release`.
      ///
      /// The `(underwriter, chain_code, token_code)` triple is the indexing
      /// surface opreg's `available()` rollup uses (cross-contract read of
      /// `sysio::uwrit::locks_t` from `sysio.opreg`). 3 × uint64 = 192 bits
      /// exceeds `uint128_t`, so the composite is hashed into a `checksum256`
      /// via `by_underwriter_ck`. A separate `by_underwriter` split-index
      /// (uint64 keyed on `underwriter.value`) provides the cheap
      /// per-operator scan path for consumers that filter on chain / token
      /// / reserve in-memory (per plan §B.2).
      ///
      /// `reserve_code` records which specific reserve this leg covers; on
      /// a slash, the outpost routes seized collateral to that reserve via
      /// `ReserveTarget`, even when multiple reserves exist for the same
      /// `(chain_code, token_code)` pair.
      struct lock_key {
         uint64_t lock_id;
         uint64_t primary_key() const { return lock_id; }
         SYSLIB_SERIALIZE(lock_key, (lock_id))
      };

      struct [[sysio::table("locks")]] lock_entry {
         uint64_t                lock_id          = 0;
         uint64_t                uwreq_id         = 0;
         name                    underwriter;
         sysio::slug_name         chain_code;
         sysio::slug_name         token_code;
         sysio::slug_name         reserve_code;
         uint64_t                amount           = 0;
         uint32_t                created_at_epoch = 0;
         /// `created_at_epoch + uwconfig.collateral_lock_duration_epoch_count`,
         /// computed at insert time in `try_select_winner`. Indexed via
         /// `byexpire` so `chklocks` can sweep expired locks in ascending order.
         uint32_t                expires_at_epoch = 0;

         /// Composite checksum index for opreg's `available()` rollup:
         /// `sha256(underwriter.value || chain_code.value || token_code.value)`
         /// packed as 24 little-endian bytes. 3 × uint64 = 192 bits doesn't
         /// fit `uint128_t`, so we hash the triple to land in `checksum256`.
         checksum256 by_underwriter_ck() const {
            std::array<uint8_t, 24> buf{};
            uint64_t uw_v = underwriter.value;
            std::memcpy(buf.data() +  0, &uw_v,             8);
            std::memcpy(buf.data() +  8, &chain_code.value, 8);
            std::memcpy(buf.data() + 16, &token_code.value, 8);
            return sysio::sha256(reinterpret_cast<const char*>(buf.data()), buf.size());
         }
         /// Split-index for cheap per-operator scans (plan §B.2). Callers
         /// pull all rows for a given underwriter and filter on
         /// chain_code / token_code / reserve_code in memory.
         uint64_t by_underwriter()      const { return underwriter.value; }
         uint64_t by_uwreq()            const { return uwreq_id; }
         uint64_t by_expires_at_epoch() const { return expires_at_epoch; }

         SYSLIB_SERIALIZE(lock_entry,
            (lock_id)(uwreq_id)(underwriter)(chain_code)(token_code)(reserve_code)
            (amount)(created_at_epoch)(expires_at_epoch))
      };

      // Per plan §B.2: split-index approach — keep only uint64 secondary
      // indexes. `by_underwriter_ck` (checksum256) is computed on the row
      // when needed for cross-contract composite comparisons, but is NOT a
      // table-managed secondary index (Antelope KV's secondary-index
      // templates expect fixed-width integer keys). opreg's `available()`
      // rollup scans `byunderwriter` (uint64) and filters (chain_code,
      // token_code) in memory — cheap because underwriters hold O(1)
      // concurrent locks.
      using locks_t = sysio::kv::table<"locks"_n, lock_key, lock_entry,
         sysio::kv::index<"byuw"_n,
            sysio::const_mem_fun<lock_entry, uint64_t, &lock_entry::by_underwriter>>,
         sysio::kv::index<"byuwreq"_n,
            sysio::const_mem_fun<lock_entry, uint64_t, &lock_entry::by_uwreq>>,
         sysio::kv::index<"byexpire"_n,
            sysio::const_mem_fun<lock_entry, uint64_t, &lock_entry::by_expires_at_epoch>>
      >;

      /// Per-underwriter race entry inside an UWREQ row. Tracks when each
      /// leg of a dual-COMMIT pair arrived so `try_select_winner` can
      /// resolve the race deterministically. Each leg's COMMIT is an
      /// independent attestation with its own outpost_id + uw_ext_chain_addr
      /// (the underwriter's chain identity on that leg's outpost) + signature
      /// over the whole UIC. The depot stores the full UIC bytes per leg so
      /// `try_select_winner` can reconstruct the signed digest verbatim and
      /// verify against any of the underwriter's WIRE account permissions.
      ///
      /// `commit_entry` does NOT carry codenames — the per-leg
      /// `(chain_code, token_code, reserve_code)` identity is on the
      /// surrounding `uw_request_t::src_*` / `dst_*` fields; the
      /// commit_entry slot is solely a race-tracker.
      struct commit_entry {
         name      underwriter;
         /// Source-leg COMMIT. `source_uic_bytes` is the verbatim zpp_bits
         /// serialization of the `UnderwriteIntentCommit` proto received from
         /// the source-side outpost; the bytes include the underwriter's
         /// signature in the `signature` field. Empty until the source-leg
         /// arrives.
         uint64_t          source_received_at_ms = 0;
         uint64_t          source_outpost_id     = 0;
         std::vector<char> source_uic_bytes;
         /// Destination-leg COMMIT. Same shape, populated when the dest-side
         /// outpost's relay arrives.
         uint64_t          dest_received_at_ms   = 0;
         uint64_t          dest_outpost_id       = 0;
         std::vector<char> dest_uic_bytes;
         /// Race outcome — INTENT_SUBMITTED (initial), INTENT_CONFIRMED
         /// (winner), SLASHED (rejected for insufficient bond), or RELEASED
         /// (loser, kept for debugging). Reuses the existing protobuf
         /// UnderwriteStatus enum.
         opp::types::UnderwriteStatus status = opp::types::UNDERWRITE_STATUS_INTENT_SUBMITTED;
         std::string reason;

         SYSLIB_SERIALIZE(commit_entry,
            (underwriter)
            (source_received_at_ms)(source_outpost_id)(source_uic_bytes)
            (dest_received_at_ms)(dest_outpost_id)(dest_uic_bytes)
            (status)(reason))
      };

      /// UWREQ row — one per inbound SWAP attestation. Tracks the swap's
      /// src/dst pairs, the underwriter race, and the eventual settlement.
      ///
      /// Each side of the swap carries a full `(chain_code, token_code,
      /// reserve_code)` triple per the v6 data-model refactor: identity
      /// is slug_name-keyed throughout, and `reserve_code` lets a same-
      /// `(chain, token)` swap target a specific reserve when multiple
      /// reserves exist for that pair.
      struct [[sysio::table("uwreqs")]] uw_request_t {
         uint64_t                                id;
         opp::types::AttestationType             type;
         opp::types::UnderwriteRequestStatus     status;

         /// Src / dst of the cross-chain swap. Populated by `createuwreq`
         /// from the decoded SwapRequest. Used by `try_select_winner` to
         /// validate per-leg bond coverage. `dst_amount` IS the quoted
         /// destination amount the underwriter must deliver.
         sysio::slug_name                         src_chain_code;
         sysio::slug_name                         src_token_code;
         sysio::slug_name                         src_reserve_code;
         uint64_t                                src_amount        = 0;
         sysio::slug_name                         dst_chain_code;
         sysio::slug_name                         dst_token_code;
         sysio::slug_name                         dst_reserve_code;
         uint64_t                                dst_amount        = 0;
         /// Variance tolerance the user accepted at SWAP_REQUEST time, in
         /// basis points (50 = 0.5%). The depot's createuwreq path validates
         /// the LP quote against this at ingestion; `try_select_winner`
         /// re-validates against the live quote at race-resolution time so
         /// drift between ingestion and race doesn't burn the underwriter.
         uint32_t                                variance_tolerance_bps = 0;

         /// Source-chain id of the deposit transaction that funded this
         /// swap (ETH: 32-byte tx hash; SOL: 64-byte signature). Used by
         /// the off-chain underwriter plugin's `verify_source_deposit`
         /// step to confirm a real on-chain deposit backs the swap
         /// before committing collateral. `createuwreq` rejects any
         /// SwapRequest with an empty `source_tx_id` (emits SwapRevert
         /// for refund) — every outpost must populate this field at
         /// swap-emit time.
         std::vector<char>                       source_tx_id;

         /// Depositor's address on the source chain (decoded from
         /// `SwapRequest.actor.address`). ETH = 20 bytes (left-padded in
         /// 32-byte ABI slots when matched); SOL = 32-byte Ed25519
         /// pubkey. The underwriter plugin matches this against the
         /// `tx.from` (ETH) / fee-payer (SOL) of the source-deposit tx
         /// during verification.
         std::vector<char>                       depositor;

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

         uint64_t by_status() const { return magic_enum::enum_integer(status); }
         uint64_t by_winner() const { return winner.value; }

         SYSLIB_SERIALIZE(uw_request_t,
            (id)(type)(status)
            (src_chain_code)(src_token_code)(src_reserve_code)(src_amount)
            (dst_chain_code)(dst_token_code)(dst_reserve_code)(dst_amount)
            (variance_tolerance_bps)(source_tx_id)(depositor)
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

      /// Fee + lock-duration + fee-split configuration singleton. Distribution
      /// logic for the fee-split fields lands in a follow-up task; today they
      /// are persisted only.
      struct [[sysio::table("uwconfig")]] uw_config {
         uint32_t fee_bps                              = 10;   // 0.1% per spoke
         uint32_t collateral_lock_duration_epoch_count = 10;   // epochs from create to auto-expire
         uint8_t  fee_split_winner_pct                 = 50;
         uint8_t  fee_split_other_uw_pct               = 25;
         uint8_t  fee_split_batch_op_pct               = 25;
         SYSLIB_SERIALIZE(uw_config,
            (fee_bps)
            (collateral_lock_duration_epoch_count)
            (fee_split_winner_pct)(fee_split_other_uw_pct)(fee_split_batch_op_pct))
      };

      using uwconfig_t = sysio::kv::global<"uwconfig"_n, uw_config>;

   private:

      using UnderwriteRequestStatus = opp::types::UnderwriteRequestStatus;
      using UnderwriteStatus        = opp::types::UnderwriteStatus;
      using ChainKind               = opp::types::ChainKind;
      using AttestationType         = opp::types::AttestationType;
   };

} // namespace sysio
