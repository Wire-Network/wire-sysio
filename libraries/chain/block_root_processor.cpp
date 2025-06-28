#include <sysio/chain/block_root_processor.hpp>
#include <sysio/chain/contract_root_object.hpp>
#include <sysio/chain/merkle.hpp>
#include <fc/bitutil.hpp>

namespace sysio { namespace chain {

block_root_processor::block_root_processor(chainbase::database& db, root_processor_ptr processor)
: _db(db)
, _processor(std::move(processor)) {
   ilog("block_root_processor initialized");
   _db.add_index<contract_root_multi_index>();
}

bool block_root_processor::calculate_root_blocks(uint32_t block_num)
{
   bool stored = false;
   root_storage root_transactions = _processor->retrieve_root_transactions(block_num);
   ilog("calculate_root_blocks block_num: ${bn}, roots size: ${rs}",("bn", block_num)("rs", root_transactions.size()));
   for(auto& instance : root_transactions) {
      const auto& contract = instance.first;
      auto& transactions = instance.second;
      ilog("calculate_root_blocks contract: ${contract}, size: ${size}",
           ("contract",contract.first.to_string())("size",transactions.size()));
      if (transactions.empty()) {
         continue;
      }
      stored = true;
      auto& contract_root_idx = _db.get_index<contract_root_multi_index, by_contract>();
      auto itr = contract_root_idx.find(boost::make_tuple(contract.first, contract.second));
      const auto merkle_root = chain::merkle(transactions);
      if (itr == contract_root_idx.end()) {
         ilog("calculate_root_blocks new root");
         const auto previous_root_id = chain::checksum256_type();
         const auto curr_root_id = compute_curr_root_id(previous_root_id, merkle_root);
         _db.create<contract_root_object>([&](auto& obj) {
            obj.contract = contract.first;
            obj.root_name = contract.second;
            obj.block_num = block_num;
            obj.root_id = curr_root_id;
            obj.prev_root_id = previous_root_id;
            obj.prev_root_bn = 0;
            obj.merkle_root = merkle_root;
         });
      } else {
         ilog("calculate_root_blocks update");
         const auto previous_root_id = itr->root_id;
         const auto curr_root_id = compute_curr_root_id(previous_root_id, merkle_root);
         const auto previous_root_block_number = itr->block_num;
         _db.modify(*itr, [&](auto& obj) {
            obj.block_num = block_num;
            obj.root_id = curr_root_id;
            obj.prev_root_id = previous_root_id;
            obj.prev_root_bn = previous_root_block_number;
            obj.merkle_root = merkle_root;
         });
      }
   }
   return stored;
}

uint32_t block_root_processor::extract_root_block_number(const chain::checksum256_type& root_id) {
   // Extract the pointer to the data from the checksum.
   auto root_id_data = root_id.data();
    
   // Since we are dealing with a checksum256_type, which is typically an array of bytes,
   // we need to correctly handle the conversion to uint32_t considering endianess.
   uint32_t root_block_number;
    
   // Copy the first 4 bytes of the data into a uint32_t variable.
   // memcpy ensures that we handle any alignment issues.
   std::memcpy(&root_block_number, root_id_data, sizeof(uint32_t));

   // Reverse the endianess if necessary.
   // You might need to adjust this based on how the data is stored (big-endian vs little-endian).
   return fc::endian_reverse_u32(root_block_number);
}

chain::checksum256_type block_root_processor::compute_curr_root_id(const chain::checksum256_type& prev_root_id, const chain::checksum256_type& curr_root) {
   // Serialize both the previous Root-ID and the current Root
   auto data = fc::raw::pack(std::make_pair(prev_root_id, curr_root));
   // Hash the serialized data to generate the new Root-ID
   chain::checksum256_type curr_root_id = chain::checksum256_type::hash(data);

   ilog("Computed interim Root-ID: ${curr_root_id}", ("curr_root_id", curr_root_id));
   // Extract the block number from the previous Root-ID and increment it by 1
   const uint32_t prev_root_block_number = extract_root_block_number(prev_root_id);
   const uint32_t next_root_block_number = prev_root_block_number + 1;

   ilog("Extracted prev_root_block_number from prev_root_id: ${prev_root_block_number}, next: ${next_root_block_number}", 
       ("prev_root_block_number", prev_root_block_number)("next_root_block_number", next_root_block_number));

   // Modify the first 4 bytes directly
   uint32_t next_root_block_number_reversed = fc::endian_reverse_u32(next_root_block_number);
   std::memcpy(curr_root_id.data(), &next_root_block_number_reversed, sizeof(uint32_t)); // Modify the first 4 bytes

   // ilog("Computed curr_root_id with block number embedded: ${c}", ("c", curr_root_id.str()));
   return curr_root_id;
}

chain::deque<chain::s_header> block_root_processor::get_s_headers(uint32_t block_num) {
   chain::deque<chain::s_header> s_headers;
   if (calculate_root_blocks(block_num)) {
      const auto& contract_root_idx = _db.get_index<contract_root_multi_index, by_block_num>();
      auto itr = contract_root_idx.lower_bound(boost::make_tuple(block_num));
      while (itr != contract_root_idx.end() && itr->block_num == block_num) {
         s_headers.emplace_back(itr->contract, itr->prev_root_id, itr->root_id, itr->merkle_root, itr->prev_root_bn);
         ++itr;
      }
   }
   return s_headers;
}

} } // namespace sysio::chain