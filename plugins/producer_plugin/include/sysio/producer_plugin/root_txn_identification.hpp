#pragma once
#include <sysio/chain/types.hpp>
#include <sysio/producer_plugin/contract_action_match.hpp>
#include <sysio/producer_plugin/root_processor.hpp>

namespace sysio {
   namespace chain {
      struct transaction_trace;
      using transaction_trace_ptr = std::shared_ptr<transaction_trace>;
      struct packed_transaction;
      using packed_transaction_ptr = std::shared_ptr<const packed_transaction>;
      struct block_state;
      using block_state_ptr = std::shared_ptr<block_state>;
   }

   struct root_txn_identification_impl; 
   using root_txn_identification_impl_ptr = std::unique_ptr<root_txn_identification_impl>;
   using root_storage = root_processor::root_storage;

   /**
    * This class manages the processing related to the transaction finality status feature.
    */
   class root_txn_identification {
   public:
      using name = sysio::chain::name;
      using contract_action_matches = std::vector<contract_action_match>;

      /**
       */
      root_txn_identification(contract_action_matches&& matches, root_processor& processor);

      ~root_txn_identification() = default;

      void signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx );

      void signal_accepted_block( const chain::block_state_ptr& bsp );

      void signal_irreversible_block( const chain::block_state_ptr& bsp );

      void signal_block_start( uint32_t block_num );

   private:
      root_txn_identification_impl_ptr _my;
   };

   using root_txn_identification_ptr = std::unique_ptr<root_txn_identification>;
} // namespace sysio
