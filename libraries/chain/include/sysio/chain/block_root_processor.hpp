#include <sysio/chain/root_processor.hpp>

namespace sysio { namespace chain {

struct snapshot_written_row_counter;

class snapshot_writer;
   using snapshot_writer_ptr = std::shared_ptr<snapshot_writer>;
   class snapshot_reader;
   using snapshot_reader_ptr = std::shared_ptr<snapshot_reader>;

   class block_root_processor {
   public:
      using root_storage = root_processor::root_storage;
      block_root_processor(chainbase::database& db, root_processor_ptr&& processor);
      static uint32_t extract_root_block_number(const chain::checksum256_type& root_id);
      static chain::checksum256_type compute_curr_root_id(const chain::checksum256_type& prev_root_id, const chain::checksum256_type& curr_root);
      chain::deque<chain::s_header> get_s_headers(uint32_t block_num);
      void initialize_database();
      void add_indices();
      size_t expected_snapshot_row_count() const;
      void add_to_snapshot( const snapshot_writer_ptr& snapshot, snapshot_written_row_counter& row_counter ) const;
      void read_from_snapshot( const snapshot_reader_ptr& snapshot );

   private:
      bool calculate_root_blocks(uint32_t block_num);  
      chainbase::database& _db;
      root_processor_ptr _processor;
   };
} } // namespace sysio::chain