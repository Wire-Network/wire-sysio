#include <sysio/chain/root_processor.hpp>

namespace sysio { namespace chain {

   class block_root_processor {
   public:
      using root_storage = root_processor::root_storage;
      block_root_processor(chainbase::database& db, root_processor_ptr processor);
      static uint32_t extract_root_block_number(const chain::checksum256_type& root_id);
      static chain::checksum256_type compute_curr_root_id(const chain::checksum256_type& prev_root_id, const chain::checksum256_type& curr_root);
      chain::deque<chain::s_header> get_s_headers(uint32_t block_num);

   private:
      bool calculate_root_blocks(uint32_t block_num);  
      chainbase::database& _db;
      root_processor_ptr _processor;
   };
} } // namespace sysio::chain