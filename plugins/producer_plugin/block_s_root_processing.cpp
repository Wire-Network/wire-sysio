#include <sysio/producer_plugin/block_s_root_processing.hpp>
#include <sysio/chain_plugin/finality_status_object.hpp>
#include <sysio/chain/block_state.hpp>
#include <sysio/chain/merkle.hpp>

#include <sysio/chain/trace.hpp>


using namespace sysio;
using namespace sysio::finality_status;

namespace sysio {
   constexpr uint32_t no_block_num = 0;
   using name = block_s_root_processing::name;
   using contract_action_matches = std::vector<contract_action_match>;

   using contract_root = std::pair<name, name>;
   struct root_hash {
      std::size_t operator()(const contract_root& p) const {
         return std::hash<name>()(p.first) ^ std::hash<name>()(p.second);
      }
   };
   struct block_s_root_processing_impl {
      block_s_root_processing_impl(contract_action_matches&& matches);

      void signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx );

      void signal_accepted_block( const chain::block_state_ptr& bsp );

      void clear_transactions();

      void process_action_traces( const std::vector<chain::action_trace>& action_traces );

      bool is_desired_trace(const chain::transaction_trace_ptr& trace) const;

      using transactions = std::deque<chain::transaction_id_type>;

      const contract_action_matches          contract_matches;
      std::unordered_map<contract_root, transactions, root_hash> storage;
   };

   block_s_root_processing::block_s_root_processing(contract_action_matches&& matches)
   : _my(new block_s_root_processing_impl(std::move(matches)) )
   {
   }

   block_s_root_processing_impl::block_s_root_processing_impl(contract_action_matches&& matches)
   : contract_matches(std::move(matches))
   {
   }

   void block_s_root_processing::signal_irreversible_block( const chain::block_state_ptr& bsp ) {
      try {
      } FC_LOG_AND_DROP(("Failed to signal irreversible block for finality status"));
   }

   void block_s_root_processing::signal_block_start( uint32_t block_num ) {
      try {
         // since a new block is started, no block state was received, so the speculative block did not get eventually produced
         _my->clear_transactions();

      } FC_LOG_AND_DROP(("Failed to signal block start for finality status"));
   }

   void block_s_root_processing::signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx ) {
      try {
         _my->signal_applied_transaction(trace, ptrx);
      } FC_LOG_AND_DROP(("Failed to signal applied transaction for finality status"));
   }

   void block_s_root_processing::signal_accepted_block( const chain::block_state_ptr& bsp ) {
      try {
         _my->signal_accepted_block(bsp);
      } FC_LOG_AND_DROP(("Failed to signal accepted block for finality status"));
   }

   void block_s_root_processing_impl::signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx ) {
      if (!is_desired_trace(trace)) {
         return;
      }

      process_action_traces(trace->action_traces);
   }

   void block_s_root_processing_impl::signal_accepted_block( const chain::block_state_ptr& bsp ) {

      clear_transactions();
   }

   void block_s_root_processing_impl::clear_transactions() {
      for (auto& str : storage) {
         str.second.clear();
      }
   }

   void block_s_root_processing_impl::process_action_traces( const std::vector<chain::action_trace>& action_traces ) {
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

   bool block_s_root_processing_impl::is_desired_trace(const chain::transaction_trace_ptr& trace) const {
      if (!trace->receipt ||
         trace->receipt->status != chain::transaction_receipt_header::executed ||
         trace->scheduled ||
         chain::is_onblock(*trace)) {
         return false;
      }
      return true;
   }

} // namespace sysio
