#pragma once
#include <sysio/chain/types.hpp>
#include <sysio/chain/s_root_extension.hpp>

namespace sysio {
   namespace chain {
      struct transaction_trace;
      using transaction_trace_ptr = std::shared_ptr<transaction_trace>;
      struct packed_transaction;
      using packed_transaction_ptr = std::shared_ptr<const packed_transaction>;
      struct block_state;
      using block_state_ptr = std::shared_ptr<block_state>;
   }
   

   struct block_s_root_processing_impl; 
   using block_s_root_processing_impl_ptr = std::unique_ptr<block_s_root_processing_impl>;
   /**
    * This class manages the processing related to the transaction finality status feature.
    */
   class block_s_root_processing {
   public:

      /**
       */
      block_s_root_processing();

      ~block_s_root_processing();

      void signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx );

      void signal_accepted_block( const chain::block_state_ptr& bsp );

      void signal_irreversible_block( const chain::block_state_ptr& bsp );

      void signal_block_start( uint32_t block_num );

      std::vector<sysio::chain::s_header> get_s_header() const;

   private:
      block_s_root_processing_impl_ptr _my;
   };

   using block_s_root_processing_ptr = std::unique_ptr<block_s_root_processing>;
} // namespace sysio
