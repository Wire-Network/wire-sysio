#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace sysio {

   /**
    * @brief sysio.cap — depot-side WIRE distribution and claim ledger.
    *
    * Holds the pending-WIRE balances owed to LIQ-token stakers and pre-launch
    * pretoken purchasers, and the per-staker WIRE-side leg of the
    * `STAKING_REWARD` flow.
    *
    * Inbound `STAKING_REWARD` attestations route through `sysio.msgch`, which
    * dispatches the per-staker body here via `onreward`. The reward arrives
    * already WIRE-denominated — native -> WIRE conversion and source-chain
    * precision scaling happen outpost-side — so `onreward` simply credits the
    * staker's claim ledger.
    *
    * Ledgers:
    * - `pending_claims` — per-Wire-account WIRE owed. `claim` drains a row via
    *   inline transfer.
    * - `unmapped_tokens` — per-(chain, native_pubkey) WIRE owed for stakers /
    *   purchasers without a Wire account yet. Completing AuthX linking
    *   inline-calls `linkswept`, which moves the credit into `pending_claims`.
    * - `reward_cursors` — per-(outpost_id, chain, native_pubkey) high-water
    *   mark of the last processed source-chain epoch reference. Replays /
    *   duplicates (`external_epoch_ref <= last`) are rejected at ingest, so no
    *   per-reward history is retained (roll-up + data-leak safe).
    *
    * Claimable lifespan: every credited / staged balance carries an
    * `expires_at_sec`. `flushexpired` prunes anything past it; the WIRE stays
    * in the `sysio.cap` account balance — i.e. it reverts into the staking
    * capital fund for redistribution. The window is configurable
    * (`setwindow`), defaulting to 180 days.
    *
    * No cooldown/withdrawal machinery for v1 (no withdrawal flow in this
    * wave). When withdrawals come online post-launch, the cooldown-queue +
    * maturation-flush pattern can be added back — see opreg's
    * `withdraw_queue` for reference.
    */
   class [[sysio::contract("sysio.cap")]] cap : public contract {
   public:
      using contract::contract;

      // Well-known accounts.
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      static constexpr name RESERV_ACCOUNT = "sysio.reserv"_n;

      // WIRE token symbol. 9 decimals system-wide.
      static constexpr symbol WIRE_SYM = symbol("WIRE", 9);

      // Default claimable-reward lifespan: 180 days, in seconds. Configurable
      // per deployment via `setwindow`.
      static constexpr uint32_t DEFAULT_CLAIM_WINDOW_SEC = 180u * 24u * 60u * 60u;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Initialize the config singleton (idempotent). The claimable-reward
      /// window defaults to `DEFAULT_CLAIM_WINDOW_SEC`.
      [[sysio::action]]
      void setconfig();

      /// Set the claimable-reward window (seconds). Unclaimed balances older
      /// than this revert to the capital fund on `flushexpired`. Auth=self.
      [[sysio::action]]
      void setwindow(uint32_t window_sec);

      /// User-callable: drain the caller's `pending_claims` row via an inline
      /// transfer of WIRE from `sysio.cap` to `wire_account`. Erases the row.
      /// Reverts if no row exists or the balance is zero.
      [[sysio::action]]
      void claim(name wire_account);

      /// Internal: sweep an `unmapped_tokens` entry into `pending_claims` when
      /// the staker / purchaser completes AuthX linking. Called inline by
      /// `sysio.authex` after a successful link. No-op if nothing matches.
      /// Auth=sysio.authex.
      [[sysio::action]]
      void linkswept(name wire_account,
                     opp::types::ChainKind chain,
                     std::vector<char> native_pubkey);

      /// Per-staker WIRE-side credit of a `STAKING_REWARD`. Dispatched inline
      /// by `sysio.msgch` (the proto body flattened to primitives). Dedupes
      /// on the source-chain epoch reference and credits `pending_claims` (if
      /// linked) or `unmapped_tokens` (if not). The reward arrives already
      /// WIRE-denominated; native -> WIRE conversion is outpost-side.
      /// Auth=sysio.msgch.
      ///
      /// @param outpost_id          Emitting outpost (dedupe scope).
      /// @param staker_wire_account Staker's Wire account name, or "" when not
      ///                            yet AuthX-linked (then parked by native
      ///                            address until the link sweep).
      /// @param reward_chain        Source chain (parking + dedupe key).
      /// @param staker_native_addr  Staker's raw native address (dedupe +
      ///                            parking key; always populated).
      /// @param reward_amount       Absolute WIRE reward amount in atomic
      ///                            units (already the staker's prorated
      ///                            portion).
      /// @param reward_epoch_index  WIRE epoch index (informational / audit).
      /// @param external_epoch_ref  Source-chain epoch reference; monotonic
      ///                            per (outpost, staker) — dedupe key.
      /// @param share_bps           Staker share in bps (informational only).
      [[sysio::action]]
      void onreward(uint64_t              outpost_id,
                    std::string           staker_wire_account,
                    opp::types::ChainKind reward_chain,
                    std::vector<char>     staker_native_addr,
                    uint64_t              reward_amount,
                    uint32_t              reward_epoch_index,
                    uint64_t              external_epoch_ref,
                    uint32_t              share_bps);

      /// Permissionless crank: prune up to `max_rows` expired ledger rows
      /// (`pending_claims`, `unmapped_tokens`). Erasing a credited row leaves
      /// its WIRE in the `sysio.cap` balance — it reverts into the staking
      /// capital fund for redistribution. Bounded.
      [[sysio::action]]
      void flushexpired(uint32_t max_rows);

      /// One row of an import batch: a pre-launch holder's WIRE credit on
      /// `chain`. `native_address` is the raw on-chain key (20 B for ETH,
      /// 32 B for Solana). `wire_atomic` is denominated in WIRE's 9-decimal
      /// atomic units; the off-chain converter floors the source pretoken
      /// value at the 1e9 boundary (sub-atomic dust dropped — total dust
      /// pool bounded at < num_users * 1 atomic WIRE).
      struct import_credit {
         std::vector<char> native_address;
         int64_t           wire_atomic = 0;

         SYSLIB_SERIALIZE(import_credit, (native_address)(wire_atomic))
      };

      /// Bootstrap import: privileged, batched insert/update of
      /// `unmapped_tokens` rows for pre-launch holders. Same `native_address`
      /// across batches sums into the existing row. Aborts once
      /// `cap_config::imported_complete` is true (set by `importdone`).
      [[sysio::action]]
      void importseed(opp::types::ChainKind chain, std::vector<import_credit> credits);

      /// Finalize the bootstrap import. Flips `imported_complete` to true;
      /// subsequent `importseed` calls revert.
      [[sysio::action]]
      void importdone();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Pending-claims: per-Wire-account WIRE owed.
      struct pclaim_key {
         uint64_t wire_account;
         uint64_t primary_key() const { return wire_account; }
         SYSLIB_SERIALIZE(pclaim_key, (wire_account))
      };

      struct [[sysio::table("pclaims")]] pending_claim {
         name     wire_account;
         asset    balance        = asset{0, WIRE_SYM};
         /// Seconds since epoch after which `flushexpired` reverts this
         /// balance to the capital fund. Refreshed on every credit.
         uint32_t expires_at_sec = 0;

         uint64_t primary_key() const { return wire_account.value; }

         SYSLIB_SERIALIZE(pending_claim, (wire_account)(balance)(expires_at_sec))
      };

      using pclaims_t = sysio::kv::table<"pclaims"_n, pclaim_key, pending_claim>;

      /// Unmapped-tokens: per-(chain, native_pubkey) WIRE owed for stakers /
      /// purchasers who don't have a Wire account yet.
      struct unmapped_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(unmapped_key, (id))
      };

      struct [[sysio::table("unmapped")]] unmapped_token {
         uint64_t                  id              = 0;
         opp::types::ChainKind     chain_kind      = opp::types::ChainKind::CHAIN_KIND_UNKNOWN;
         std::vector<char>         native_pubkey;
         asset                     balance         = asset{0, WIRE_SYM};
         uint32_t                  expires_at_sec  = 0;

         uint64_t primary_key() const { return id; }

         uint128_t by_chain_addr() const {
            return chain_addr_key(chain_kind, native_pubkey);
         }

         SYSLIB_SERIALIZE(unmapped_token, (id)(chain_kind)(native_pubkey)(balance)(expires_at_sec))
      };

      using unmapped_t = sysio::kv::table<"unmapped"_n, unmapped_key, unmapped_token,
         sysio::kv::index<"bychainad"_n,
            sysio::const_mem_fun<unmapped_token, uint128_t, &unmapped_token::by_chain_addr>>
      >;

      /// Per-(outpost_id, chain, native_pubkey) dedupe cursor: the highest
      /// source-chain epoch reference processed. Anything `<=` is a replay.
      struct rwdcur_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(rwdcur_key, (id))
      };

      struct [[sysio::table("rwdcursors")]] reward_cursor {
         uint64_t                  id                      = 0;
         uint64_t                  outpost_id              = 0;
         opp::types::ChainKind     chain                   = opp::types::ChainKind::CHAIN_KIND_UNKNOWN;
         std::vector<char>         native_pubkey;
         uint64_t                  last_external_epoch_ref = 0;

         uint64_t primary_key() const { return id; }

         uint128_t by_outpost_addr() const {
            return outpost_addr_key(outpost_id, chain, native_pubkey);
         }

         SYSLIB_SERIALIZE(reward_cursor,
            (id)(outpost_id)(chain)(native_pubkey)(last_external_epoch_ref))
      };

      using rwdcursors_t = sysio::kv::table<"rwdcursors"_n, rwdcur_key, reward_cursor,
         sysio::kv::index<"byoutaddr"_n,
            sysio::const_mem_fun<reward_cursor, uint128_t, &reward_cursor::by_outpost_addr>>
      >;

      /// Cap-staking config singleton.
      struct [[sysio::table("capcfg")]] cap_config {
         /// One-way flag protecting the bootstrap `importseed` action.
         bool     imported_complete = false;
         /// Claimable-reward window in seconds (configurable; default 180d).
         uint32_t claim_window_sec  = DEFAULT_CLAIM_WINDOW_SEC;

         SYSLIB_SERIALIZE(cap_config, (imported_complete)(claim_window_sec))
      };

      using capcfg_t = sysio::kv::global<"capcfg"_n, cap_config>;

      /// Monotonic id counters.
      struct [[sysio::table("capcounters")]] cap_counters {
         uint64_t next_unmapped_id = 1;
         uint64_t next_cursor_id   = 1;

         SYSLIB_SERIALIZE(cap_counters, (next_unmapped_id)(next_cursor_id))
      };

      using capcounters_t = sysio::kv::global<"capcounters"_n, cap_counters>;

      // -----------------------------------------------------------------------
      //  Shared key derivation
      //
      //  uint128 secondary keys only NARROW a scan; the full identity is
      //  re-checked by an exact predicate (see scan_find in the .cpp), so an
      //  address-prefix collision is resolved deterministically rather than
      //  risking a hash collision on a 64-bit primary key.
      // -----------------------------------------------------------------------

      /// (chain, native address) -> uint128 narrowing key. High 64 bits =
      /// chain; low 64 = first 8 bytes of the address.
      static uint128_t chain_addr_key(opp::types::ChainKind chain,
                                      const std::vector<char>& addr) {
         if (addr.empty()) return 0;
         uint64_t prefix = 0;
         const size_t n = addr.size() < sizeof(uint64_t) ? addr.size() : sizeof(uint64_t);
         std::memcpy(&prefix, addr.data(), n);
         return (static_cast<uint128_t>(chain) << 64) | prefix;
      }

      /// (outpost_id, chain, native address) -> uint128 narrowing key. High 64
      /// bits = outpost_id; low 64 = chain (high 32) xored with the first 4
      /// address bytes.
      static uint128_t outpost_addr_key(uint64_t outpost_id,
                                        opp::types::ChainKind chain,
                                        const std::vector<char>& addr) {
         uint32_t prefix = 0;
         const size_t n = addr.size() < sizeof(uint32_t) ? addr.size() : sizeof(uint32_t);
         if (n > 0) std::memcpy(&prefix, addr.data(), n);
         uint64_t lo = (static_cast<uint64_t>(chain) << 32) ^ static_cast<uint64_t>(prefix);
         return (static_cast<uint128_t>(outpost_id) << 64) | lo;
      }

   private:
      using ChainKind   = opp::types::ChainKind;
      using TokenKind   = opp::types::TokenKind;
   };

} // namespace sysio
