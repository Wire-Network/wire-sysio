#pragma once

#include <sysio/sysio.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>

namespace sysio {

   class [[sysio::contract("sysio.uwrit")]] uwrit : public contract {
   public:
      using contract::contract;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set underwriting configuration.
      [[sysio::action]]
      void setconfig(uint32_t fee_bps,
                     uint32_t confirm_lock_sec,
                     uint32_t uw_fee_share_pct,
                     uint32_t other_uw_share_pct,
                     uint32_t batch_op_share_pct);

      /// Underwriter submits intent to underwrite a message.
      [[sysio::action]]
      void submituw(name underwriter, uint64_t msg_id,
                    checksum256 source_sig, checksum256 target_sig);

      /// Called when outpost confirms underwriting commitment.
      [[sysio::action]]
      void confirmuw(uint64_t uw_entry_id);

      /// Expire locks past unlock_time (permissionless).
      [[sysio::action]]
      void expirelock(uint64_t uw_entry_id);

      /// Distribute fees after completion.
      [[sysio::action]]
      void distfee(uint64_t uw_entry_id);

      /// Update collateral from outpost attestations.
      [[sysio::action]]
      void updcltrl(name underwriter, uint8_t chain_kind,
                    asset amount, bool is_increase);

      /// Slash underwriter (called by sysio.chalg).
      [[sysio::action]]
      void slash(name underwriter, std::string reason);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Underwriter collateral table.
      struct [[sysio::table, sysio::contract("sysio.uwrit")]] collateral_entry {
         uint64_t    id;
         name        underwriter;
         uint8_t     chain_kind;       // ChainKind protobuf enum
         asset       staked_amount;
         asset       locked_amount;
         asset       available_amount; // staked - locked (precomputed)

         uint64_t primary_key() const { return id; }
         uint128_t by_uw_chain() const {
            return (static_cast<uint128_t>(underwriter.value) << 64) |
                   static_cast<uint64_t>(chain_kind);
         }
         uint64_t by_underwriter() const { return underwriter.value; }
      };

      using collateral_t = multi_index<"collateral"_n, collateral_entry,
         indexed_by<"byuwchain"_n, const_mem_fun<collateral_entry, uint128_t, &collateral_entry::by_uw_chain>>,
         indexed_by<"byuw"_n, const_mem_fun<collateral_entry, uint64_t, &collateral_entry::by_underwriter>>
      >;

      /// Underwriting ledger table.
      struct [[sysio::table, sysio::contract("sysio.uwrit")]] underwriting_entry {
         uint64_t    id;
         name        underwriter;
         uint64_t    message_id;       // FK to sysio.msgch message
         uint8_t     status;           // UnderwriteStatus protobuf enum
         asset       source_amount;
         asset       target_amount;
         uint8_t     source_chain;     // ChainKind
         uint8_t     target_chain;     // ChainKind
         time_point  intent_time;
         time_point  unlock_time;
         asset       fee_earned;
         checksum256 source_sig;
         checksum256 target_sig;

         uint64_t primary_key() const { return id; }
         uint64_t by_underwriter() const { return underwriter.value; }
         uint64_t by_message() const { return message_id; }
         uint64_t by_status() const { return static_cast<uint64_t>(status); }
         uint64_t by_unlock() const { return unlock_time.sec_since_epoch(); }
      };

      using uwledger_t = multi_index<"uwledger"_n, underwriting_entry,
         indexed_by<"byuw"_n, const_mem_fun<underwriting_entry, uint64_t, &underwriting_entry::by_underwriter>>,
         indexed_by<"bymessage"_n, const_mem_fun<underwriting_entry, uint64_t, &underwriting_entry::by_message>>,
         indexed_by<"bystatus"_n, const_mem_fun<underwriting_entry, uint64_t, &underwriting_entry::by_status>>,
         indexed_by<"byunlock"_n, const_mem_fun<underwriting_entry, uint64_t, &underwriting_entry::by_unlock>>
      >;

      /// Fee configuration singleton.
      struct [[sysio::table("uwconfig")]] uw_config {
         uint32_t fee_bps = 10;              // 0.1% = 10 basis points per spoke
         uint32_t confirm_lock_sec = 86400;  // 24 hours challenge window
         uint32_t uw_fee_share_pct = 50;     // 50% to underwriter
         uint32_t other_uw_share_pct = 25;   // 25% to other underwriters
         uint32_t batch_op_share_pct = 25;   // 25% to batch operators

         SYSLIB_SERIALIZE(uw_config,
            (fee_bps)(confirm_lock_sec)
            (uw_fee_share_pct)(other_uw_share_pct)(batch_op_share_pct))
      };

      using uwconfig_t = sysio::singleton<"uwconfig"_n, uw_config>;

   private:
      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT = "sysio.msgch"_n;
      static constexpr name CHALG_ACCOUNT = "sysio.chalg"_n;

      // UnderwriteStatus constants (match protobuf values)
      static constexpr uint8_t UW_INTENT_SUBMITTED = 0;
      static constexpr uint8_t UW_INTENT_CONFIRMED = 1;
      static constexpr uint8_t UW_COMPLETED        = 2;
      static constexpr uint8_t UW_EXPIRED          = 3;
      static constexpr uint8_t UW_SLASHED          = 4;
   };

} // namespace sysio
