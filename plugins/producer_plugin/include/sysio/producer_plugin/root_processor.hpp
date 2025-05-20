#pragma once
#include <sysio/chain/types.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/producer_plugin/contract_root_object.hpp>

namespace sysio {

   using name = sysio::chain::name;
   using contract_root = std::pair<name, name>;
   struct root_hash {
      std::size_t operator()(const contract_root& p) const {
         return std::hash<name>()(p.first) ^ std::hash<name>()(p.second);
      }
   };

   class root_processor {
      public:
         using transactions = std::deque<chain::transaction_id_type>;
         using root_storage = std::unordered_map<contract_root, transactions, root_hash>;

         virtual void calculate_root_blocks(const root_storage& root_transactions) = 0;
      private:
   };
} // namespace sysio