#pragma once

#include <sysio/sysio.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

namespace sysio {

   class [[sysio::contract("sysio.opreg")]] opreg : public contract {
   public:
      using contract::contract;

      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name CHALG_ACCOUNT  = "sysio.chalg"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;

      // Core token symbol — currently SYS, may change to WIRE
      static constexpr symbol CORE_SYM = symbol("SYS", 4);

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set operator registry configuration.
      [[sysio::action]]
      void setconfig(uint32_t max_available_producers,
                     uint32_t max_available_batch_ops,
                     uint32_t max_available_underwriters,
                     uint64_t terminate_prune_delay_ms);

      /// Register a new operator.
      [[sysio::action]]
      void regoperator(name account,
                       opp::types::OperatorType type,
                       bool is_bootstrapped);

      /// Stake tokens (positive=deposit, negative=withdraw). Piecemeal.
      [[sysio::action]]
      void stake(name account,
                 opp::types::ChainAddress chain_addr,
                 opp::types::TokenAmount amount);

      /// Type-specific processing when eligibility changes.
      [[sysio::action]]
      void processprod(name account, bool was_eligible, bool is_eligible);

      [[sysio::action]]
      void processbatch(name account, bool was_eligible, bool is_eligible);

      [[sysio::action]]
      void processuw(name account, bool was_eligible, bool is_eligible);

      /// Slash an operator. Permanent. Called by sysio.chalg.
      [[sysio::action]]
      void slash(name account, std::string reason);

      /// Prune terminated operator rows past the delay. Permissionless.
      [[sysio::action]]
      void prune();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Stake entry: chain address + token amount + timestamp
      struct stake_entry {
         opp::types::ChainAddress chain_addr;
         opp::types::TokenAmount  amount;
         uint64_t                 timestamp_ms = 0;

         SYSLIB_SERIALIZE(stake_entry, (chain_addr)(amount)(timestamp_ms))
      };

      /// Operator entry — the primary roster.
      struct [[sysio::table, sysio::contract("sysio.opreg")]] operator_entry {
         name                          account;
         opp::types::OperatorType      type;
         opp::types::OperatorStatus    status;
         bool                          is_bootstrapped = false;
         std::vector<stake_entry>      stakes;
         uint64_t                      registered_at   = 0;
         uint64_t                      available_at    = 0;
         uint64_t                      slashed_at      = 0;
         uint64_t                      terminated_at   = 0;

         uint64_t primary_key() const { return account.value; }
         uint64_t by_type()   const { return static_cast<uint64_t>(type); }
         uint64_t by_status() const { return static_cast<uint64_t>(status); }
      };

      using operators_t = multi_index<"operators"_n, operator_entry,
         indexed_by<"bytype"_n,
            const_mem_fun<operator_entry, uint64_t, &operator_entry::by_type>>,
         indexed_by<"bystatus"_n,
            const_mem_fun<operator_entry, uint64_t, &operator_entry::by_status>>
      >;

      /// Stake requirement entry for opconfig.
      struct stake_requirement {
         opp::types::ChainAddress chain_addr;
         opp::types::TokenAmount  min_amount;
         uint64_t                 config_timestamp_ms = 0;

         SYSLIB_SERIALIZE(stake_requirement, (chain_addr)(min_amount)(config_timestamp_ms))
      };

      /// Operator registry configuration singleton.
      struct [[sysio::table("opconfig")]] op_config {
         std::vector<stake_requirement> req_prod_stakes;
         std::vector<stake_requirement> req_batchop_stakes;
         std::vector<stake_requirement> req_uw_stakes;
         uint32_t max_available_producers    = 21;
         uint32_t max_available_batch_ops    = 63;
         uint32_t max_available_underwriters = 21;
         uint64_t terminate_prune_delay_ms   = 86400000; // 24hrs

         SYSLIB_SERIALIZE(op_config,
            (req_prod_stakes)(req_batchop_stakes)(req_uw_stakes)
            (max_available_producers)(max_available_batch_ops)(max_available_underwriters)
            (terminate_prune_delay_ms))
      };

      using opconfig_t = sysio::singleton<"opconfig"_n, op_config>;

   private:

      using OperatorType   = opp::types::OperatorType;
      using OperatorStatus = opp::types::OperatorStatus;
      using ChainKind      = opp::types::ChainKind;
      using ChainAddress   = opp::types::ChainAddress;
      using TokenAmount    = opp::types::TokenAmount;
      using AttestationType = opp::types::AttestationType;
   };

} // namespace sysio
