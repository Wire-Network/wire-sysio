#include <sysio/sub_chain_plugin/root_txn_identification.hpp>
#include <sysio/sub_chain_plugin/contract_action_match.hpp>
#include <sysio/chain/block_state.hpp>
#include <sysio/chain/root_processor.hpp>
#include <sysio/chain/merkle.hpp>

#include <sysio/chain/trace.hpp>
#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>


using namespace sysio;

namespace sysio {
   constexpr uint32_t no_block_num = 0;
   using name = root_txn_identification::name;
   using contract_action_matches = std::vector<contract_action_match>;
   struct root_txn_identification_impl {
      root_txn_identification_impl(contract_action_matches&& matches, chain::root_processor& processor);

      void signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx );

      void signal_accepted_block( const chain::block_state_ptr& bsp );

      void process_action_traces( const std::vector<chain::action_trace>& action_traces );

      bool is_desired_trace(const chain::transaction_trace_ptr& trace) const;

      using transactions = chain::deque<chain::transaction_id_type>;
      using contract_storage = std::unordered_map<chain::contract_root, transactions, chain::root_hash>;

      contract_action_matches       contract_matches;
      contract_storage              storage;
      chain::root_processor&        root_storage_processor;
   };

   root_txn_identification::root_txn_identification(contract_action_matches&& matches, chain::root_processor& processor)
   : _my(new root_txn_identification_impl(std::move(matches), processor) )
   {
   }

   root_txn_identification::~root_txn_identification()
   {
   }

   root_txn_identification_impl::root_txn_identification_impl(contract_action_matches&& matches, chain::root_processor& processor)
   : contract_matches(std::move(matches))
   , root_storage_processor(processor)
   {
   }

   void root_txn_identification::signal_block_start( uint32_t block_num ) {
      try {
         // since a new block is started, no block state was received, so the speculative block did not get eventually produced
         _my->storage = root_txn_identification_impl::contract_storage{};

      } FC_LOG_AND_DROP(("Failed to signal block start for finality status"));
   }

   void root_txn_identification::signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx ) {
      try {
         _my->signal_applied_transaction(trace, ptrx);
      } FC_LOG_AND_DROP(("Failed to signal applied transaction for finality status"));
   }

   void root_txn_identification::signal_accepted_block( const chain::block_state_ptr& bsp ) {
      try {
         _my->signal_accepted_block(bsp);
      } FC_LOG_AND_DROP(("Failed to signal accepted block for finality status"));
   }

   void root_txn_identification_impl::signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx ) {
      if (!is_desired_trace(trace)) {
         return;
      }

      process_action_traces(trace->action_traces);
   }

   void root_txn_identification_impl::signal_accepted_block( const chain::block_state_ptr& bsp ) {
      ilog("Processing accepted block: ${block_num}", ("block_num", bsp->block_num));
      root_storage_processor.calculate_root_blocks(bsp->block_num, std::move(storage));

      storage = contract_storage{};
   }

   void root_txn_identification_impl::process_action_traces( const std::vector<chain::action_trace>& action_traces ) {
      for ( const auto& action_trace : action_traces ) {
         const auto& act = action_trace.act;
         const auto& contract = act.account;
         const auto& action = act.name;

         for (const auto& root_contract_match : contract_matches) {
            if (root_contract_match.is_contract_match(contract) &&
                root_contract_match.is_action_match(action)) {
               // We have a match, so we need to store the transaction id
               // in the storage for this contract
               auto& storage = this->storage[std::make_pair(contract, root_contract_match.root_name)];
               storage.push_back(action_trace.trx_id);
               break;
            }
         }
      }
   }

   bool root_txn_identification_impl::is_desired_trace(const chain::transaction_trace_ptr& trace) const {
      if (!trace->receipt ||
         trace->receipt->status != chain::transaction_receipt_header::executed ||
         trace->scheduled ||
         chain::is_onblock(*trace)) {
         return false;
      }
      return true;
   }

} // namespace sysio
