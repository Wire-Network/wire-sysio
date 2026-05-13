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
    * pretoken purchasers. The depot owns proportional distribution of bulk
    * WIRE allocations (per Jack 2026-05-13: outpost only records events, the
    * depot splits proportionally using pretoken amounts + pretoken yield as
    * the weight; no time-weighting, snapshot-based at allocation time).
    *
    * - `pending_claims` is the per-Wire-account ledger of WIRE owed.
    *   Users call `claim` to drain via inline transfer.
    * - `unmapped_tokens` is the per-(chain, native_pubkey) ledger for
    *   stakers / purchasers who don't have a Wire account yet. Completing
    *   AuthX linking inline-calls `linkswept` which moves the credit into
    *   `pending_claims`.
    *
    * No cooldown/withdrawal machinery for v1 (Jack 2026-05-13: outpost has
    * no cooldown scenarios in this wave). When withdrawals come online
    * post-launch, the cooldown-queue + maturation-flush pattern can be
    * added back — see opreg's `withdraw_queue` for reference.
    *
    * Per-user pretoken-balance state needed for ongoing `StakingReward`
    * proration is deferred -- add when the StakingReward handler lands and
    * we know exactly which attestation(s) maintain the balances.
    */
   class [[sysio::contract("sysio.cap")]] cap : public contract {
   public:
      using contract::contract;

      // Well-known accounts.
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;

      // WIRE token symbol. 9 decimals system-wide.
      static constexpr symbol WIRE_SYM = symbol("WIRE", 9);

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

      /// Cap-staking config singleton. `imported_complete` is the one-way flag
      /// protecting the bootstrap `importseed` action (added in a follow-up).
      struct [[sysio::table("capcfg")]] cap_config {
         bool      imported_complete = false;

         SYSLIB_SERIALIZE(cap_config, (imported_complete))
      };

      using capcfg_t = sysio::kv::global<"capcfg"_n, cap_config>;

      /// Monotonic id counter for unmapped rows.
      struct [[sysio::table("capcounters")]] cap_counters {
         uint64_t next_unmapped_id  = 1;

         SYSLIB_SERIALIZE(cap_counters, (next_unmapped_id))
      };

      using capcounters_t = sysio::kv::global<"capcounters"_n, cap_counters>;

   private:
      using ChainKind   = opp::types::ChainKind;
      using TokenKind   = opp::types::TokenKind;
   };

} // namespace sysio
