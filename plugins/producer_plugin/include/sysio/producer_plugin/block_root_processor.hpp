#include <sysio/chain/root_processor.hpp>

namespace sysio {

   class block_root_processor : chain::root_processor {
   public:
      block_root_processor(chainbase::database& db);
      void calculate_root_blocks(uint32_t block_num, root_storage&& root_transactions) override;
      static uint32_t extract_root_block_number(const chain::checksum256_type& root_id);
      static chain::checksum256_type compute_curr_root_id(const chain::checksum256_type& prev_root_id, const chain::checksum256_type& curr_root);
      std::deque<chain::s_header> get_s_headers(uint32_t block_num) const override;

   private:
      chainbase::database& _db;
   };
} // namespace sysio