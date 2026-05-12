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

namespace sysio {

   /**
    * @brief sysio.cap — depot-side WIRE distribution and claim ledger.
    *
    * Holds the pending-WIRE balances owed to LIQ-token stakers and pre-launch
    * pretoken purchasers. The outpost owns share computation (per-staker
    * `share_bps` arrives pre-computed in `StakingReward`); wire-sysio just
    * credits, holds, and pays out.
    *
    * - `pending_claims` is the per-Wire-account ledger of WIRE owed.
    *   Users call `claim` to drain via inline transfer.
    * - `unmapped_tokens` is the per-(chain, native_pubkey) ledger for
    *   stakers / purchasers who don't have a Wire account yet. Completing
    *   AuthX linking inline-calls `linkswept` which moves the credit into
    *   `pending_claims`.
    * - `positions` carries lifecycle metadata only — status, owner mapping,
    *   timestamps. Shares and principal live at the outpost.
    * - `cooldown_queue` mirrors opreg's `withdraw_queue`. Eligibility-cursor
    *   driven; drained by `flushcd` on each `sysio.epoch::advance` tick.
    *   Cooldown begins when the staking-withdrawal envelope reaches this
    *   contract.
    * - `available_stake` is the read-only rollup analogous to opreg's
    *   `available()`.
    */
   class [[sysio::contract("sysio.cap")]] cap : public contract {
   public:
      using contract::contract;

      // Well-known accounts.
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;

      // WIRE token symbol. 9 decimals system-wide.
      static constexpr symbol WIRE_SYM = symbol("WIRE", 9);

      // Cooldown wait epochs. Mirrors opreg::WITHDRAW_WAIT_EPOCHS.
      static constexpr uint32_t COOLDOWN_WAIT_EPOCHS = 2;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set cap-staking configuration. Placeholder while concrete fields are
      /// still being decided; calling once initializes the singleton.
      [[sysio::action]]
      void setconfig();

      /// User-callable: drain the caller's `pending_claims` row via an inline
      /// transfer of WIRE from `sysio.cap` to `wire_account`. Erases the row.
      /// Reverts if no row exists or the balance is zero.
      [[sysio::action]]
      void claim(name wire_account);

      /// Internal: sweep an `unmapped_tokens` entry into `pending_claims` when
      /// the staker / purchaser completes AuthX linking. Called inline by
      /// `sysio.authex` after a successful link. No-op if no matching unmapped
      /// row exists.
      [[sysio::action]]
      void linkswept(name wire_account,
                     opp::types::ChainKind chain,
                     std::vector<char> native_pubkey);

      /// Internal: drain matured rows from `cooldown_queue`. Called inline
      /// from `sysio.epoch::advance` each tick.
      [[sysio::action]]
      void flushcd(uint32_t current_epoch);

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

      /// Read-only rollup of the staker's spendable (yield-earning) stake on
      /// a given chain. Returns 0 today since position-side principal is not
      /// tracked yet; once lifecycle handlers land, will return
      /// `sum(open_principal) - sum(active_cooldown_amount)`.
      [[sysio::action, sysio::read_only]]
      uint64_t availstake(name wire_account, opp::types::ChainKind chain);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Position lifecycle metadata. Outpost owns shares and principal;
      /// wire-sysio stores only what it uniquely tracks.
      struct position_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(position_key, (id))
      };

      struct [[sysio::table("positions")]] position {
         uint64_t                       id                  = 0;
         opp::types::ChainKind          chain_kind          = opp::types::ChainKind::CHAIN_KIND_UNKNOWN;
         std::vector<char>              native_address;
         uint64_t                       outpost_position_id = 0;
         opp::types::StakeStatus        status              = opp::types::StakeStatus::STAKE_STATUS_UNKNOWN;
         name                           wire_account;
         uint64_t                       opened_at_ms        = 0;
         uint64_t                       last_status_ms      = 0;

         uint64_t  primary_key()     const { return id; }
         uint64_t  by_wire_account() const { return wire_account.value; }
         uint64_t  by_status()       const { return static_cast<uint64_t>(status); }

         /// Composite: (chain << 64 | first 8 bytes of native_address).
         /// 8-byte prefix gives a 2^64 namespace per chain — collisions on
         /// 20-byte ETH addresses or 32-byte SOL pubkeys are negligible at
         /// expected user counts.
         uint128_t by_chain_addr() const {
            if (native_address.empty()) return 0;
            uint64_t prefix = 0;
            const size_t n = native_address.size() < sizeof(uint64_t)
                           ? native_address.size() : sizeof(uint64_t);
            std::memcpy(&prefix, native_address.data(), n);
            return (static_cast<uint128_t>(chain_kind) << 64) | prefix;
         }

         SYSLIB_SERIALIZE(position,
            (id)(chain_kind)(native_address)(outpost_position_id)
            (status)(wire_account)(opened_at_ms)(last_status_ms))
      };

      using positions_t = sysio::kv::table<"positions"_n, position_key, position,
         sysio::kv::index<"bywire"_n,
            sysio::const_mem_fun<position, uint64_t, &position::by_wire_account>>,
         sysio::kv::index<"bystatus"_n,
            sysio::const_mem_fun<position, uint64_t, &position::by_status>>,
         sysio::kv::index<"bychainad"_n,
            sysio::const_mem_fun<position, uint128_t, &position::by_chain_addr>>
      >;

      /// Pending-claims: per-Wire-account WIRE owed.
      struct pclaim_key {
         uint64_t wire_account;
         uint64_t primary_key() const { return wire_account; }
         SYSLIB_SERIALIZE(pclaim_key, (wire_account))
      };

      struct [[sysio::table("pclaims")]] pending_claim {
         name    wire_account;
         asset   balance       = asset{0, WIRE_SYM};

         uint64_t primary_key() const { return wire_account.value; }

         SYSLIB_SERIALIZE(pending_claim, (wire_account)(balance))
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

         uint64_t primary_key() const { return id; }

         uint128_t by_chain_addr() const {
            if (native_pubkey.empty()) return 0;
            uint64_t prefix = 0;
            const size_t n = native_pubkey.size() < sizeof(uint64_t)
                           ? native_pubkey.size() : sizeof(uint64_t);
            std::memcpy(&prefix, native_pubkey.data(), n);
            return (static_cast<uint128_t>(chain_kind) << 64) | prefix;
         }

         SYSLIB_SERIALIZE(unmapped_token, (id)(chain_kind)(native_pubkey)(balance))
      };

      using unmapped_t = sysio::kv::table<"unmapped"_n, unmapped_key, unmapped_token,
         sysio::kv::index<"bychainad"_n,
            sysio::const_mem_fun<unmapped_token, uint128_t, &unmapped_token::by_chain_addr>>
      >;

      /// Cooldown queue. Mirrors opreg::withdraw_request layout.
      struct cd_key {
         uint64_t request_id;
         uint64_t primary_key() const { return request_id; }
         SYSLIB_SERIALIZE(cd_key, (request_id))
      };

      struct [[sysio::table("cdqueue")]] cooldown_entry {
         uint64_t                  request_id          = 0;
         uint64_t                  position_id         = 0;
         name                      wire_account;
         opp::types::ChainKind     chain_kind          = opp::types::ChainKind::CHAIN_KIND_UNKNOWN;
         asset                     amount              = asset{0, WIRE_SYM};
         uint32_t                  eligible_at_epoch   = 0;
         uint32_t                  requested_at_epoch  = 0;

         uint64_t primary_key() const { return request_id; }

         /// (wire_account, chain_kind) composite for `available_stake` scans.
         uint128_t by_wire_chain() const {
            return (static_cast<uint128_t>(wire_account.value) << 64)
                 | static_cast<uint64_t>(chain_kind);
         }
         uint64_t by_eligible() const { return static_cast<uint64_t>(eligible_at_epoch); }
         uint64_t by_position() const { return position_id; }

         SYSLIB_SERIALIZE(cooldown_entry,
            (request_id)(position_id)(wire_account)(chain_kind)(amount)
            (eligible_at_epoch)(requested_at_epoch))
      };

      using cdqueue_t = sysio::kv::table<"cdqueue"_n, cd_key, cooldown_entry,
         sysio::kv::index<"bywirechn"_n,
            sysio::const_mem_fun<cooldown_entry, uint128_t, &cooldown_entry::by_wire_chain>>,
         sysio::kv::index<"byeligible"_n,
            sysio::const_mem_fun<cooldown_entry, uint64_t, &cooldown_entry::by_eligible>>,
         sysio::kv::index<"byposition"_n,
            sysio::const_mem_fun<cooldown_entry, uint64_t, &cooldown_entry::by_position>>
      >;

      /// Cap-staking config singleton. `imported_complete` is the one-way flag
      /// protecting the bootstrap `importseed` action (added in a follow-up).
      struct [[sysio::table("capcfg")]] cap_config {
         bool      imported_complete = false;

         SYSLIB_SERIALIZE(cap_config, (imported_complete))
      };

      using capcfg_t = sysio::kv::global<"capcfg"_n, cap_config>;

      /// Monotonic id counters for position / unmapped / cooldown rows.
      struct [[sysio::table("capcounters")]] cap_counters {
         uint64_t next_position_id  = 1;
         uint64_t next_unmapped_id  = 1;
         uint64_t next_cd_id        = 1;

         SYSLIB_SERIALIZE(cap_counters, (next_position_id)(next_unmapped_id)(next_cd_id))
      };

      using capcounters_t = sysio::kv::global<"capcounters"_n, cap_counters>;

   private:
      using ChainKind   = opp::types::ChainKind;
      using StakeStatus = opp::types::StakeStatus;
      using TokenKind   = opp::types::TokenKind;
   };

} // namespace sysio
