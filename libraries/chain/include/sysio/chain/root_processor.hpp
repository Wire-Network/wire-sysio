#pragma once
#include <sysio/chain/types.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/s_root_extension.hpp>

namespace sysio { namespace chain {

   using contract_root = std::pair<name, name>;
   struct root_hash {
      std::size_t operator()(const contract_root& p) const {
         return std::hash<name>()(p.first) ^ std::hash<name>()(p.second);
      }
   };
   struct root_processor;
   using root_processor_ptr = std::shared_ptr<root_processor>;

   struct root_processor {
      using transactions = chain::deque<chain::transaction_id_type>;
      using root_storage = std::unordered_map<contract_root, transactions, root_hash>;

      virtual root_storage retrieve_root_transactions(uint32_t block_num) = 0;
   };
} } // namespace sysio::chain