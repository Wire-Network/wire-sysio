#pragma once
#include <sysio/chain/types.hpp>
#include <sysio/chain/contract_action_match.hpp>
#include <sysio/chain/root_processor.hpp>

namespace sysio { namespace chain {
   struct transaction_trace;
   using transaction_trace_ptr = std::shared_ptr<transaction_trace>;
   struct packed_transaction;
   using packed_transaction_ptr = std::shared_ptr<const packed_transaction>;
   struct block_state;
   using block_state_ptr = std::shared_ptr<block_state>;

   struct root_txn_identification_impl; 
   using root_txn_identification_impl_ptr = std::unique_ptr<root_txn_identification_impl>;
   
   /**
    * This class manages the processing related to the transaction finality status feature.
    */
   class root_txn_identification : public root_processor {
   public:
      using contract_action_matches = std::vector<contract_action_match>;

      /**
       */
      root_txn_identification(contract_action_matches&& matches);

      virtual ~root_txn_identification();

      void signal_applied_transaction( const transaction_trace_ptr& trace, const packed_transaction_ptr& ptrx );

      void signal_accepted_block( const block_state_ptr& bsp );

      void signal_block_start( uint32_t block_num );

      root_storage retrieve_root_transactions(uint32_t block_num) override;;

   private:
      root_txn_identification_impl_ptr _my;
   };
} } // namespace sysio::chain
