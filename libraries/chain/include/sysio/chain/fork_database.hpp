#pragma once
#include <sysio/chain/block_state.hpp>

namespace fc { class cfile_datastream; } // forward decl

namespace sysio::chain {

   template<class BSP>
   struct fork_database_impl;

   using block_branch_t = std::vector<signed_block_ptr>;
   enum class ignore_duplicate_t { no, yes };
   enum class include_root_t { no, yes };
   enum class fork_db_add_t {
      failure,            // add failed
      duplicate,          // already added and ignore_duplicate=true
      added,              // inserted into an existing branch or started a new branch, but not best branch
      appended_to_head,   // new best head of current best branch; no fork switch
      fork_switch         // new best head of new branch, fork switch to new branch
   };

   // Used for logging of comparison values used for best fork determination
   std::string log_fork_comparison(const block_state& bs);
   std::string log_fork_comparison(const block_state_legacy& bs);

   /**
    * @class fork_database_type
    * @brief manages light-weight state for all potential unconfirmed forks
    *
    * As new blocks are received, they are pushed into the fork database. The fork
    * database tracks the longest chain and the last irreversible block number. All
    * blocks older than the last irreversible block are freed after emitting the
    * irreversible signal.
    *
    * An internal mutex is used to provide thread-safety.
    *
    * fork_database should be used instead of fork_database_t directly as it manages
    * the different supported types.
    */
   template<class BSP = block_state_ptr>
   class fork_database_type {
   public:
      using bsp_t            = BSP;
      using bs_t             = bsp_t::element_type;
      using bhsp_t           = bs_t::bhsp_t;
      using bhs_t            = bhsp_t::element_type;
      using branch_t         = std::vector<bsp_t>;
      using full_branch_t    = std::vector<bhsp_t>;
      using branch_pair_t    = pair<branch_t, branch_t>;
      static constexpr uint32_t magic_number = 0x30510FDB;
      // Update max_supported_version if the persistent file format changes.
      static constexpr uint32_t min_supported_version = 4;
      static constexpr uint32_t max_supported_version = 4;

      explicit fork_database_type(const std::filesystem::path& data_dir);
      ~fork_database_type();

      // not thread safe, expected to be called from main thread before allowing concurrent access
      void open( validator_t& validator );
      void close();
      bool file_exists() const;

      size_t size() const;

      bsp_t get_block( const block_id_type& id, include_root_t include_root = include_root_t::no ) const;
      bool block_exists( const block_id_type& id ) const;
      bool validated_block_exists( const block_id_type& id, const block_id_type& claimed_id ) const;

      /**
       *  Purges any existing blocks from the fork database and resets the root block_header_state to the provided value.
       *  The head will also be reset to point to the root.
       */
      void reset_root( const bsp_t& root_bhs );

      /**
       *  Advance root block forward to some other block in the tree.
       */
      void advance_root( const block_id_type& id );

      /**
       *  Add block state to fork database.
       *  Must link to existing block in fork database or the root.
       *  @returns fork_db_add_t - result of the add
       *  @throws unlinkable_block_exception - unlinkable to any branch
       *  @throws fork_database_exception - no root, n is nullptr, protocol feature error, duplicate when ignore_duplicate=false
       */
      fork_db_add_t add( const bsp_t& n, ignore_duplicate_t ignore_duplicate );

      void remove( const block_id_type& id );

      /**
       * Remove all blocks >= block_num
       */
      void remove( block_num_type block_num);

      bool is_valid() const; // sanity checks on this fork_db

      bool   has_root() const;

      /**
       * Root of the fork database, not part of the index. Corresponds to head of the block log. Is an irreversible block.
       * On startup from a snapshot the root->block will be nullptr until root is advanced.
       * Undefined if !has_root()
       */
      bsp_t  root() const;

      /**
       * The best branch head of blocks in the fork database, can be null if include_root_t::no and fork_db is empty
       * @param include_root yes if root should be returned if no blocks in fork database
       */
      bsp_t  head(include_root_t include_root = include_root_t::no) const;

      /**
       * The calculated pending savanna LIB ID that will become LIB or is currently LIB
       */
      block_id_type pending_savanna_lib_id() const;
      bool set_pending_savanna_lib_id( const block_id_type& id );

      /**
       * @return true if id is built on top of pending savanna lib or id == pending_savanna_lib
       */
      bool is_descendant_of_pending_savanna_lib( const block_id_type& id ) const;

      /**
       * @param ancestor the id of a possible ancestor block
       * @param descendant the id of a possible descendant block
       * @return false if either ancestor or descendant not found.
       *         true if any descendant->previous.. == ancestor.
       *         false if unable to find ancestor in any descendant->previous..
       */
      bool is_descendant_of(const block_id_type& ancestor, const block_id_type& descendant) const;

      /**
       *  Returns the sequence of block states resulting from trimming the branch from the
       *  root block (exclusive) to the block with an id of `h` (inclusive) by removing any
       *  block states corresponding to block numbers greater than `trim_after_block_num`.
       *
       *  The order of the sequence is in descending block number order.
       *  A block with an id of `h` must exist in the fork database otherwise this method will throw an exception.
       */
      branch_t fetch_branch( const block_id_type& h, uint32_t trim_after_block_num = std::numeric_limits<uint32_t>::max() ) const;
      block_branch_t fetch_block_branch( const block_id_type& h, uint32_t trim_after_block_num = std::numeric_limits<uint32_t>::max() ) const;

      /**
       * Equivalent to fetch_branch(fork_db->head()->id())
       */
      block_branch_t fetch_branch_from_head() const;

      /**
       * Returns the sequence of block states resulting from trimming the branch from the
       * root block (exclusive) to the block with an id of `h` (inclusive) by removing any
       * block states that are after block `b`. Returns empty if `b` not found on `h` branch.
       */
      branch_t fetch_branch( const block_id_type& h, const block_id_type& b ) const;

      /**
       *  Returns full branch of block_header_state pointers including the root.
       *  The order of the sequence is in descending block number order.
       *  A block with an id of `h` must exist in the fork database otherwise this method will throw an exception.
       */
      full_branch_t fetch_full_branch( const block_id_type& h ) const;

      /**
       *  Returns the block state with a block number of `block_num` that is on the branch that
       *  contains a block with an id of`h`, or the empty shared pointer if no such block can be found.
       */
      bsp_t search_on_branch( const block_id_type& h, uint32_t block_num, include_root_t include_root = include_root_t::no ) const;

      /**
       * search_on_branch( head()->id(), block_num)
       */
      bsp_t search_on_head_branch( uint32_t block_num, include_root_t include_root = include_root_t::no ) const;

      /**
       *  Given two head blocks, return two branches of the fork graph that
       *  end with a common ancestor (same prior block)
       */
      branch_pair_t fetch_branch_from(const block_id_type& first, const block_id_type& second) const;

   private:
      void open( const char* desc, const std::filesystem::path& fork_db_file, fc::cfile_datastream& ds, validator_t& validator );
      void close( std::ofstream& out );

   private:
      unique_ptr<fork_database_impl<BSP>> my;
   };

   using fork_database_t = fork_database_type<>;

} /// sysio::chain

FC_REFLECT_ENUM( sysio::chain::fork_db_add_t,
                 (failure)(duplicate)(added)(appended_to_head)(fork_switch) )
