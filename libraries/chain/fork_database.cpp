#include <sysio/chain/fork_database.hpp>
#include <sysio/chain/exceptions.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/global_fun.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/cfile.hpp>
#include <fstream>
#include <mutex>

namespace sysio::chain {
   using boost::multi_index_container;
   using namespace boost::multi_index;

   /**
    * History:
    * Version 1: initial version of the new refactored fork database portable format
    * Version 2: Savanna version, store either `block_state`, `block_state_legacy` or both versions,
    *            root is full `block_state`, not just the header.
    * Version 3: Spring 1.0.1 updated block_header_state (core with policy gen #)
    *            (two possible fork_db, one containing `block_state_legacy`, one containing `block_state`)
    * version 4: Wire 6.0.0 - Only one fork database with Savanna block_state
 */

   std::string log_fork_comparison(const block_state& bs) {
      std::string r;
      r += "[ latest_qc_block_timestamp: " + bs.latest_qc_block_timestamp().to_time_point().to_iso_string() + ", ";
      r += "timestamp: " + bs.timestamp().to_time_point().to_iso_string() + ", ";
      r += "id: " + bs.id().str();
      r += " ]";
      return r;
   }

   struct by_block_id;
   struct by_best_branch;
   struct by_prev;

   template<class BSP>  // block_state_ptr, bhsp_t is block_header_state_ptr
   struct fork_database_impl {
      using bsp_t              = BSP;
      using bs_t               = bsp_t::element_type;
      using bhsp_t             = bs_t::bhsp_t;
      using bhs_t              = bhsp_t::element_type;

      using fork_db_t          = fork_database_type<BSP>;
      using branch_t           = fork_db_t::branch_t;
      using full_branch_t      = fork_db_t::full_branch_t;
      using branch_pair_t      = fork_db_t::branch_pair_t;

      using by_best_branch_t = ordered_unique<
         tag<by_best_branch>,
         composite_key<block_state,
                       const_mem_fun<block_state,     block_timestamp_type, &block_state::latest_qc_block_timestamp>,
                       const_mem_fun<block_state,     block_timestamp_type, &block_state::timestamp>,
                       const_mem_fun<block_state,     const block_id_type&, &block_state::id>>,
         composite_key_compare<std::greater<block_timestamp_type>,
                               std::greater<block_timestamp_type>, std::greater<block_id_type>>>;

      using fork_multi_index_type = multi_index_container<
         bsp_t,
         indexed_by<hashed_unique<tag<by_block_id>, BOOST_MULTI_INDEX_CONST_MEM_FUN(bs_t, const block_id_type&, id),
                                  std::hash<block_id_type>>,
                    ordered_non_unique<tag<by_prev>, const_mem_fun<bs_t, const block_id_type&, &bs_t::previous>>,
                    by_best_branch_t>>;

      const std::filesystem::path data_dir;
      std::mutex             mtx;
      bsp_t                  root;
      block_id_type          pending_savanna_lib_id; // under Savanna the id of what will become root
      fork_multi_index_type  index;

      explicit fork_database_impl(const std::filesystem::path& data_dir) : data_dir(data_dir) {}

      void             open_impl( const char* desc, const std::filesystem::path& fork_db_file, fc::cfile_datastream& ds, validator_t& validator );
      void             close_impl( std::ofstream& out );
      fork_db_add_t    add_impl( const bsp_t& n, ignore_duplicate_t ignore_duplicate, bool validate, validator_t& validator );
      bool             is_valid() const;

      bsp_t            get_block_impl( const block_id_type& id, include_root_t include_root = include_root_t::no ) const;
      bool             block_exists_impl( const block_id_type& id ) const;
      bool             validated_block_exists_impl( const block_id_type& id, const block_id_type& claimed_id ) const;
      void             reset_root_impl( const bsp_t& root_bs );
      void             advance_root_impl( const block_id_type& id );
      void             remove_impl( const block_id_type& id );
      void             remove_impl( block_num_type block_num );
      bsp_t            head_impl(include_root_t include_root) const;
      bool             set_pending_savanna_lib_id_impl(const block_id_type& id);
      bool             is_descendant_of_pending_savanna_lib_impl(const block_id_type& id) const;
      bool             is_descendant_of_impl(const block_id_type& ancestor, const block_id_type& descendant) const;
      branch_t         fetch_branch_impl( const block_id_type& h, uint32_t trim_after_block_num ) const;
      block_branch_t   fetch_block_branch_impl( const block_id_type& h, uint32_t trim_after_block_num ) const;
      branch_t         fetch_branch_impl( const block_id_type& h, const block_id_type& b ) const;
      block_branch_t   fetch_branch_from_head_impl() const;
      full_branch_t    fetch_full_branch_impl(const block_id_type& h) const;
      bsp_t            search_on_branch_impl( const block_id_type& h, uint32_t block_num, include_root_t include_root ) const;
      bsp_t            search_on_head_branch_impl( uint32_t block_num, include_root_t include_root ) const;
      branch_pair_t    fetch_branch_from_impl( const block_id_type& first, const block_id_type& second ) const;

   };

   template<class BSP>
   fork_database_type<BSP>::fork_database_type(const std::filesystem::path& data_dir)
      :my( new fork_database_impl<BSP>(data_dir) )
   {}

   template<class BSP>
   fork_database_type<BSP>::~fork_database_type() {
      close();
   }

   template<class BSP>
   void fork_database_type<BSP>::open( const char* desc, const std::filesystem::path& fork_db_file, fc::cfile_datastream& ds, validator_t& validator ) {
      std::lock_guard g( my->mtx );
      my->open_impl( desc, fork_db_file, ds, validator );
   }

   template<class BSP>
   void fork_database_impl<BSP>::open_impl( const char* desc, const std::filesystem::path& fork_db_file, fc::cfile_datastream& ds, validator_t& validator ) {
      bsp_t _root = std::make_shared<bs_t>();
      block_id_type savanna_lib_id;
      fc::raw::unpack( ds, savanna_lib_id );
      fc::raw::unpack( ds, *_root );
      reset_root_impl( _root ); // resets pending_savanna_lib_id
      set_pending_savanna_lib_id_impl( savanna_lib_id );

      unsigned_int size; fc::raw::unpack( ds, size );
      for( uint32_t i = 0, n = size.value; i < n; ++i ) {
         bs_t s;
         fc::raw::unpack( ds, s );
         // do not populate transaction_metadatas, they will be created as needed in apply_block with appropriate key recovery
         add_impl( std::make_shared<bs_t>( std::move( s ) ), ignore_duplicate_t::no, true, validator );
      }
   }

   template<class BSP>
   void fork_database_type<BSP>::close(std::ofstream& out) {
      std::lock_guard g( my->mtx );
      my->close_impl(out);
   }

   template<class BSP>
   size_t fork_database_type<BSP>::size() const {
      std::lock_guard g( my->mtx );
      return my->index.size();
   }

   template<class BSP>
   void fork_database_impl<BSP>::close_impl(std::ofstream& out) {
      assert(!!root); // if head or root are null, we don't save and shouldn't get here

      auto head = head_impl(include_root_t::no);
      if (head) {
         ilog("Writing fork_database {} blocks with root {}:{} and head {}:{}",
              head->block_num() - root->block_num(), root->block_num(), root->id(), head->block_num(), head->id());
      } else {
         ilog("Writing empty fork_database with root {}:{}", root->block_num(), root->id());
      }

      fc::raw::pack( out, pending_savanna_lib_id );
      fc::raw::pack( out, *root );

      uint32_t num_blocks_in_fork_db = index.size();
      fc::raw::pack( out, unsigned_int{num_blocks_in_fork_db} );

      const auto& indx = index.template get<by_best_branch>();

      for (auto itr = indx.rbegin(); itr != indx.rend(); ++itr) {
         fc::raw::pack( out, *(*itr) );
      }

      index.clear();
   }

   template<class BSP>
   void fork_database_type<BSP>::reset_root( const bsp_t& root_bsp ) {
      std::lock_guard g( my->mtx );
      my->reset_root_impl(root_bsp);
   }

   template<class BSP>
   void fork_database_impl<BSP>::reset_root_impl( const bsp_t& root_bsp ) {
      assert(root_bsp);
      root = root_bsp;
      root->set_valid(true);
      pending_savanna_lib_id = block_id_type{};
      index.clear();
   }

   template<class BSP>
   void fork_database_type<BSP>::advance_root( const block_id_type& id ) {
      std::lock_guard g( my->mtx );
      my->advance_root_impl( id );
   }

   template<class BSP>
   void fork_database_impl<BSP>::advance_root_impl( const block_id_type& id ) {
      SYS_ASSERT( root, fork_database_exception, "root not yet set" );

      auto new_root = get_block_impl( id );
      SYS_ASSERT( new_root, fork_database_exception,
                  "cannot advance root to a block that does not exist in the fork database" );
      SYS_ASSERT( new_root->is_valid(), fork_database_exception,
                  "cannot advance root to a block that has not yet been validated" );


      deque<block_id_type> blocks_to_remove;
      for( auto b = new_root; b; ) {
         blocks_to_remove.emplace_back( b->previous() );
         b = get_block_impl( blocks_to_remove.back() );
         SYS_ASSERT( b || blocks_to_remove.back() == root->id(), fork_database_exception,
                     "invariant violation: orphaned branch was present in forked database" );
      }

      // The new root block should be erased from the fork database index individually rather than with the remove method,
      // because we do not want the blocks branching off of it to be removed from the fork database.
      index.erase( index.find( id ) );

      // The other blocks to be removed are removed using the remove method so that orphaned branches do not remain in the fork database.
      for( const auto& block_id : blocks_to_remove ) {
         remove_impl( block_id );
      }

      // Even though fork database no longer needs block or trxs when a block state becomes a root of the tree,
      // avoid mutating the block state at all, for example clearing the block shared pointer, because other
      // parts of the code which run asynchronously may later expect it remain unmodified.

      root = new_root;
   }

   template <class BSP>
   fork_db_add_t fork_database_impl<BSP>::add_impl(const bsp_t& n, ignore_duplicate_t ignore_duplicate,
                                                   bool validate, validator_t& validator) {
      SYS_ASSERT( root, fork_database_exception, "root not yet set" );
      SYS_ASSERT( n, fork_database_exception, "attempt to add null block state" );

      if constexpr (std::is_same_v<BSP, block_state_ptr>) {
         auto qc_claim = n->extract_qc_claim();
         if (qc_claim.is_strong_qc) {
            // it is not possible to claim a future block, skip if pending is already a higher height
            block_num_type current_lib = block_header::num_from_id(pending_savanna_lib_id);
            if (qc_claim.block_num > current_lib) {
               // claim has already been verified, update LIB even if unable to verify block
               // We evaluate a block extension qc and advance lib if strong.
               // This is done before evaluating the block. It is possible the block
               // will not be valid or forked out. This is safe because the block is
               // just acting as a carrier of this info. It doesn't matter if the block
               // is actually valid as it simply is used as a network message for this data.
               if (auto claimed = search_on_branch_impl(n->previous(), qc_claim.block_num, include_root_t::no)) {
                  auto& latest_qc_claim__block_ref = claimed->core.get_block_reference(claimed->core.latest_qc_claim().block_num);
                  set_pending_savanna_lib_id_impl(latest_qc_claim__block_ref.block_id);
               }
            }
         }
      }

      auto prev_bh = get_block_impl( n->previous(), include_root_t::yes );
      SYS_ASSERT( prev_bh, unlinkable_block_exception,
                  "fork_db unlinkable block {} previous {}", n->id(), n->previous() );

      if (validate) {
         try {
            const auto& exts = n->header_exts;

            if (auto it = exts.find(protocol_feature_activation::extension_id()); it != exts.end()) {
               const auto& new_protocol_features = std::get<protocol_feature_activation>(it->second).protocol_features;
               validator(n->timestamp(), prev_bh->get_activated_protocol_features()->protocol_features, new_protocol_features);
            }
         }
         SYS_RETHROW_EXCEPTIONS( fork_database_exception, "serialized fork database is incompatible with configured protocol features" )
      }

      auto prev_head = head_impl(include_root_t::yes);

      auto inserted = index.insert(n);
      SYS_ASSERT(ignore_duplicate == ignore_duplicate_t::yes || inserted.second, fork_database_exception,
                 "duplicate block added: {}", n->id());

      if (!inserted.second)
         return fork_db_add_t::duplicate;
      const bool new_head = n == head_impl(include_root_t::no);
      if (new_head && n->previous() == prev_head->id())
         return fork_db_add_t::appended_to_head;
      if (new_head)
         return fork_db_add_t::fork_switch;

      return fork_db_add_t::added;
   }

   template<class BSP>
   fork_db_add_t fork_database_type<BSP>::add( const bsp_t& n, ignore_duplicate_t ignore_duplicate ) {
      std::lock_guard g( my->mtx );
      return my->add_impl(n, ignore_duplicate, false,
                          [](block_timestamp_type         timestamp,
                             const flat_set<digest_type>& cur_features,
                             const vector<digest_type>&   new_features) {});
   }

   template<class BSP>
   bool fork_database_type<BSP>::is_valid() const {
      std::lock_guard g( my->mtx );
      return my->is_valid();
   }

   template<class BSP>
   bool fork_database_impl<BSP>::is_valid() const {
      return !!root;
   }

   template<class BSP>
   bool fork_database_type<BSP>::has_root() const {
      std::lock_guard g( my->mtx );
      return !!my->root;
   }

   template<class BSP>
   BSP fork_database_type<BSP>::root() const {
      std::lock_guard g( my->mtx );
      return my->root;
   }

   template<class BSP>
   BSP fork_database_type<BSP>::head(include_root_t include_root) const {
      std::lock_guard g( my->mtx );
      return my->head_impl(include_root);
   }

   template<class BSP>
   BSP fork_database_impl<BSP>::head_impl(include_root_t include_root) const {
      if (index.empty()) {
         if (include_root == include_root_t::yes)
            return root;
         return {};
      }
      const auto& indx = index.template get<by_best_branch>();
      return *indx.begin();
   }

   template<class BSP>
   block_id_type fork_database_type<BSP>::pending_savanna_lib_id() const {
      std::lock_guard g( my->mtx );
      return my->pending_savanna_lib_id;
   }

   template<class BSP>
   bool fork_database_type<BSP>::set_pending_savanna_lib_id(const block_id_type& id) {
      std::lock_guard g( my->mtx );
      return my->set_pending_savanna_lib_id_impl(id);
   }

   template<class BSP>
   bool fork_database_impl<BSP>::set_pending_savanna_lib_id_impl(const block_id_type& id) {
      block_num_type new_lib = block_header::num_from_id(id);
      block_num_type old_lib = block_header::num_from_id(pending_savanna_lib_id);
      if (new_lib > old_lib) {
         dlog("set fork db pending savanna lib {}: {}", block_header::num_from_id(id), id);
         pending_savanna_lib_id = id;
         return true;
      }
      return false;
   }

   template<class BSP>
   bool fork_database_type<BSP>::is_descendant_of_pending_savanna_lib( const block_id_type& id ) const {
      std::lock_guard g( my->mtx );
      return my->is_descendant_of_pending_savanna_lib_impl(id);
   }

   template<class BSP>
   bool fork_database_impl<BSP>::is_descendant_of_pending_savanna_lib_impl(const block_id_type& id) const {
      if (pending_savanna_lib_id == id)
         return true;
      return is_descendant_of_impl(pending_savanna_lib_id, id);
   }

   template<class BSP>
   bool fork_database_type<BSP>::is_descendant_of(const block_id_type& ancestor, const block_id_type& descendant) const {
      std::lock_guard g( my->mtx );
      return my->is_descendant_of_impl(ancestor, descendant);
   }

   template<class BSP>
   bool fork_database_impl<BSP>::is_descendant_of_impl(const block_id_type& ancestor, const block_id_type& descendant) const {
      block_num_type ancestor_block_num = block_header::num_from_id(ancestor);
      if (ancestor_block_num >= block_header::num_from_id(descendant))
         return false;

      auto i = index.find(descendant);
      for (; i != index.end(); i = index.find((*i)->previous())) {
         if ((*i)->previous() == ancestor)
            return true;
         if ((*i)->block_num() <= ancestor_block_num+1) // +1 since comparison of previous() already done
            return false;
      }

      // At this point descendant is not found in ancestor, but root has not been checked.
      // However, root is either the ancestor or we can't make determination if descendant is a child because
      // ancestor < root. Therefore, no reason to check root.
      return false;
   }

   template <class BSP>
   sysio::chain::fork_database_type<BSP>::branch_t
   fork_database_type<BSP>::fetch_branch(const block_id_type& h, uint32_t trim_after_block_num) const {
      std::lock_guard g(my->mtx);
      return my->fetch_branch_impl(h, trim_after_block_num);
   }

   template <class BSP>
   fork_database_type<BSP>::branch_t
   fork_database_impl<BSP>::fetch_branch_impl(const block_id_type& h, uint32_t trim_after_block_num) const {
      branch_t result;
      result.reserve(index.size());
      for (auto i = index.find(h); i != index.end(); i = index.find((*i)->previous())) {
         if ((*i)->block_num() <= trim_after_block_num)
            result.push_back(*i);
      }

      return result;
   }

   template <class BSP>
   fork_database_type<BSP>::branch_t
   fork_database_type<BSP>::fetch_branch(const block_id_type& h, const block_id_type& b) const {
      std::lock_guard g(my->mtx);
      return my->fetch_branch_impl(h, b);
   }

   template <class BSP>
   fork_database_type<BSP>::branch_t
   fork_database_impl<BSP>::fetch_branch_impl(const block_id_type& h, const block_id_type& b) const {
      branch_t result;
      result.reserve(index.size());
      bool found_branch = false;
      for (auto i = index.find(h); i != index.end(); i = index.find((*i)->previous())) {
         if ((*i)->id() == b)
            found_branch = true;
         if (found_branch)
            result.push_back(*i);
      }
      return result;
   }

   template <class BSP>
   block_branch_t
   fork_database_type<BSP>::fetch_block_branch(const block_id_type& h, uint32_t trim_after_block_num) const {
      std::lock_guard g(my->mtx);
      return my->fetch_block_branch_impl(h, trim_after_block_num);
   }

   template <class BSP>
   block_branch_t
   fork_database_impl<BSP>::fetch_block_branch_impl(const block_id_type& h, uint32_t trim_after_block_num) const {
      block_branch_t result;
      result.reserve(index.size());
      for (auto i = index.find(h); i != index.end(); i = index.find((*i)->previous())) {
         if ((*i)->block_num() <= trim_after_block_num)
            result.push_back((*i)->block);
      }

      return result;
   }

   template <class BSP>
   block_branch_t fork_database_type<BSP>::fetch_branch_from_head() const {
      std::lock_guard g(my->mtx);
      return my->fetch_branch_from_head_impl();
   }

   template <class BSP>
   block_branch_t fork_database_impl<BSP>::fetch_branch_from_head_impl() const {
      auto head = head_impl(include_root_t::no);
      if (head)
         return this->fetch_block_branch_impl(head->id(), std::numeric_limits<uint32_t>::max());
      return block_branch_t{};
   }

   template <class BSP>
   fork_database_type<BSP>::full_branch_t
   fork_database_type<BSP>::fetch_full_branch(const block_id_type& h) const {
      std::lock_guard g(my->mtx);
      return my->fetch_full_branch_impl(h);
   }

   template <class BSP>
   fork_database_type<BSP>::full_branch_t
   fork_database_impl<BSP>::fetch_full_branch_impl(const block_id_type& h) const {
      full_branch_t result;
      result.reserve(index.size());
      for (auto i = index.find(h); i != index.end(); i = index.find((*i)->previous())) {
         result.push_back(*i);
      }
      result.push_back(root);
      return result;
   }

   template<class BSP>
   BSP fork_database_type<BSP>::search_on_branch( const block_id_type& h, uint32_t block_num, include_root_t include_root /* = include_root_t::no */ ) const {
      std::lock_guard g( my->mtx );
      return my->search_on_branch_impl( h, block_num, include_root );
   }

   template<class BSP>
   BSP fork_database_impl<BSP>::search_on_branch_impl( const block_id_type& h, uint32_t block_num, include_root_t include_root ) const {
      if (!root)
         return {};
      if( include_root == include_root_t::yes && root->block_num() == block_num ) {
         return root; // root is root of every branch, no need to check h
      }
      if (block_num <= root->block_num())
         return {};

      for( auto i = index.find(h); i != index.end(); i = index.find( (*i)->previous() ) ) {
         if ((*i)->block_num() == block_num)
            return *i;
      }

      return {};
   }

   template<class BSP>
   BSP fork_database_type<BSP>::search_on_head_branch( uint32_t block_num, include_root_t include_root /* = include_root_t::no */ ) const {
      std::lock_guard g(my->mtx);
      return my->search_on_head_branch_impl(block_num, include_root);
   }

   template<class BSP>
   BSP fork_database_impl<BSP>::search_on_head_branch_impl( uint32_t block_num, include_root_t include_root ) const {
      auto head = head_impl(include_root);
      if (!head)
         return head;
      return search_on_branch_impl(head->id(), block_num, include_root);
   }

   /**
    *  Given two head blocks, return two branches of the fork graph that
    *  end with a common ancestor (same prior block)
    */
   template <class BSP>
   fork_database_type<BSP>::branch_pair_t
   fork_database_type<BSP>::fetch_branch_from(const block_id_type& first, const block_id_type& second) const {
      std::lock_guard g(my->mtx);
      return my->fetch_branch_from_impl(first, second);
   }

   template <class BSP>
   fork_database_type<BSP>::branch_pair_t
   fork_database_impl<BSP>::fetch_branch_from_impl(const block_id_type& first, const block_id_type& second) const {
      branch_pair_t result;
      auto first_branch = (first == root->id()) ? root : get_block_impl(first);
      auto second_branch = (second == root->id()) ? root : get_block_impl(second);

      SYS_ASSERT(first_branch, fork_db_block_not_found, "block #{} {} does not exist", block_header::num_from_id(first), first);
      SYS_ASSERT(second_branch, fork_db_block_not_found, "block #{} {} does not exist", block_header::num_from_id(second), second);

      while( first_branch->block_num() > second_branch->block_num() )
      {
         result.first.push_back(first_branch);
         const auto& prev = first_branch->previous();
         first_branch = (prev == root->id()) ? root : get_block_impl( prev );
         SYS_ASSERT( first_branch, fork_db_block_not_found, "block {} does not exist", prev);
      }

      while( second_branch->block_num() > first_branch->block_num() )
      {
         result.second.push_back( second_branch );
         const auto& prev = second_branch->previous();
         second_branch = (prev == root->id()) ? root : get_block_impl( prev );
         SYS_ASSERT( second_branch, fork_db_block_not_found, "block {} does not exist", prev);
      }

      if (first_branch->id() == second_branch->id()) return result;

      while( first_branch->previous() != second_branch->previous() )
      {
         result.first.push_back(first_branch);
         result.second.push_back(second_branch);
         const auto &first_prev = first_branch->previous();
         first_branch = get_block_impl( first_prev );
         const auto &second_prev = second_branch->previous();
         second_branch = get_block_impl( second_prev );
         SYS_ASSERT( first_branch, fork_db_block_not_found, "block {} does not exist", first_prev);
         SYS_ASSERT( second_branch, fork_db_block_not_found, "block {} does not exist", second_prev);
      }

      if( first_branch && second_branch )
      {
         result.first.push_back(first_branch);
         result.second.push_back(second_branch);
      }
      return result;
   } /// fetch_branch_from_impl

   /// remove all of the invalid forks built off of this id including this id
   template<class BSP>
   void fork_database_type<BSP>::remove( const block_id_type& id ) {
      std::lock_guard g( my->mtx );
      return my->remove_impl( id );
   }

   template<class BSP>
   void fork_database_impl<BSP>::remove_impl( const block_id_type& id ) {
      deque<block_id_type> remove_queue{id};
      const auto& previdx = index.template get<by_prev>();

      for( uint32_t i = 0; i < remove_queue.size(); ++i ) {
         auto previtr = previdx.lower_bound( remove_queue[i] );
         while( previtr != previdx.end() && (*previtr)->previous() == remove_queue[i] ) {
            remove_queue.emplace_back( (*previtr)->id() );
            ++previtr;
         }
      }

      for( const auto& block_id : remove_queue ) {
         index.erase( block_id );
      }
   }

   template<class BSP>
   void fork_database_type<BSP>::remove( block_num_type block_num ) {
      std::lock_guard g( my->mtx );
      return my->remove_impl( block_num );
   }

   template<class BSP>
   void fork_database_impl<BSP>::remove_impl( block_num_type block_num ) {
      // doesn't matter which index as there is no index over block_num
      for (auto itr = index.template get<0>().begin(); itr != index.template get<0>().end(); ) {
         if ((*itr)->block_num() >= block_num) {
            itr = index.erase(itr);
         } else {
            ++itr;
         }
      }
   }

   template<class BSP>
   BSP fork_database_type<BSP>::get_block(const block_id_type& id,
                                       include_root_t include_root /* = include_root_t::no */) const {
      std::lock_guard g( my->mtx );
      return my->get_block_impl(id, include_root);
   }

   template<class BSP>
   BSP fork_database_impl<BSP>::get_block_impl(const block_id_type& id,
                                               include_root_t include_root /* = include_root_t::no */) const {
      if( include_root == include_root_t::yes && root && root->id() == id ) {
         return root;
      }
      auto itr = index.find( id );
      if( itr != index.end() )
         return *itr;
      return {};
   }

   template<class BSP>
   bool fork_database_type<BSP>::block_exists(const block_id_type& id) const {
      std::lock_guard g( my->mtx );
      return my->block_exists_impl(id);
   }

   template<class BSP>
   bool fork_database_impl<BSP>::block_exists_impl(const block_id_type& id) const {
      return index.find( id ) != index.end();
   }

   template<class BSP>
   bool fork_database_type<BSP>::validated_block_exists(const block_id_type& id, const block_id_type& claimed_id) const {
      std::lock_guard g( my->mtx );
      return my->validated_block_exists_impl(id, claimed_id);
   }

   // precondition: claimed_id is either id, or an ancestor of id
   // returns true if block `id`, or one of its ancestors not older than claimed_id, is found in fork_db
   // and `is_valid()`.
   // ------------------------------------------------------------------------------------------------------
   template<class BSP>
   bool fork_database_impl<BSP>::validated_block_exists_impl(const block_id_type& id, const block_id_type& claimed_id) const {
      bool id_present = false;

      for (auto i = index.find(id); i != index.end(); i = index.find((*i)->previous())) {
         id_present = true;
         if ((*i)->is_valid())
            return true;
         if ((*i)->id() == claimed_id)
            return false;
      }

      // if we return `true`, let's validate the precondition and make sure claimed_id is not in another branch
      assert(!id_present || block_header::num_from_id(claimed_id) <= block_header::num_from_id(root->id()));
      return id_present || id == root->id();
   }

   template<class BSP>
   void fork_database_type<BSP>::close() {
      auto fork_db_file {my->data_dir / config::fork_db_filename};

      if (!is_valid()) {
         ilog("No fork_database to persist");
         return;
      }

      ilog("Persisting to fork_database file: {}", fork_db_file.generic_string());
      std::ofstream out( fork_db_file.generic_string().c_str(), std::ios::out | std::ios::binary | std::ofstream::trunc );

      fc::raw::pack( out, magic_number );

      // write out current version which is always max_supported_version
      // version == 1 -> legacy
      // version == 2 -> Spring 1.0.0
      //                 (two possible fork_db, one containing `block_state_legacy`, one containing `block_state`)
      //                  unsupported by Spring 1.0.1 and above
      // version == 3 -> Spring 1.0.1 updated block_header_state (core with policy gen #)
      //                 (two possible fork_db, one containing `block_state_legacy`, one containing `block_state`)
      // version == 4 -> Wire 6.0.0 - Only one fork database with Savanna block_state
      // ---------------------------------------------------------------------------------------------------------
      fc::raw::pack( out, max_supported_version );

      close(out);
   }

   template<class BSP>
   bool fork_database_type<BSP>::file_exists() const {
      auto fork_db_file = my->data_dir / config::fork_db_filename;
      return std::filesystem::exists( fork_db_file );
   };

   template<class BSP>
   void fork_database_type<BSP>::open( validator_t& validator ) {
      if (!std::filesystem::is_directory(my->data_dir))
         std::filesystem::create_directories(my->data_dir);

      assert(!my->is_valid());

      auto fork_db_file = my->data_dir / config::fork_db_filename;
      if( std::filesystem::exists( fork_db_file ) ) {
         try {
            fc::cfile f;
            f.set_file_path(fork_db_file);
            f.open("rb");

            fc::cfile_datastream ds(f);

            // determine file type, validate totem
            uint32_t totem = 0;
            fc::raw::unpack( ds, totem );
            SYS_ASSERT( totem == magic_number, fork_database_exception,
                        "Fork database file '{}' has unexpected magic number: {}. Expected {}",
                        fork_db_file.generic_string(), totem, magic_number);

            uint32_t version = 0;
            fc::raw::unpack( ds, version );
            SYS_ASSERT( version >= min_supported_version && version <= max_supported_version,
                        fork_database_exception,
                       "Unsupported version of fork database file '{}'. "
                       "Fork database version is {} while code supports version(s) [{},{}]",
                       fork_db_file.generic_string(), version, min_supported_version, max_supported_version);

            open("savanna", fork_db_file, ds, validator);
         } FC_CAPTURE_AND_RETHROW( "{}", fork_db_file.generic_string() );
         std::filesystem::remove( fork_db_file );
      }
   }

   // do class instantiations
   template class fork_database_type<block_state_ptr>;
   template struct fork_database_impl<block_state_ptr>;

} /// sysio::chain
