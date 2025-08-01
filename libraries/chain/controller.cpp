#include <sysio/chain/controller.hpp>
#include <sysio/chain/transaction_context.hpp>

#include <sysio/chain/block_log.hpp>
#include <sysio/chain/fork_database.hpp>
#include <sysio/chain/exceptions.hpp>

#include <sysio/chain/account_object.hpp>
#include <sysio/chain/code_object.hpp>
#include <sysio/chain/block_summary_object.hpp>
#include <sysio/chain/sysio_contract.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/protocol_state_object.hpp>
#include <sysio/chain/contract_table_objects.hpp>
#include <sysio/chain/generated_transaction_object.hpp>
#include <sysio/chain/transaction_object.hpp>
#include <sysio/chain/genesis_intrinsics.hpp>
#include <sysio/chain/whitelisted_intrinsics.hpp>
#include <sysio/chain/database_header_object.hpp>

#include <sysio/chain/protocol_feature_manager.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/block_root_processor.hpp>
#include <sysio/chain/subjective_billing.hpp>
#include <sysio/chain/chain_snapshot.hpp>
#include <sysio/chain/thread_utils.hpp>
#include <sysio/chain/platform_timer.hpp>
#include <sysio/chain/deep_mind.hpp>
#include <sysio/chain/contract_action_match.hpp>
#include <sysio/chain/root_txn_identification.hpp>

#include <chainbase/chainbase.hpp>
#include <sysio/vm/allocator.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/variant_object.hpp>
#include <bls12-381/bls12-381.hpp>

#include <new>
#include <shared_mutex>
#include <utility>

namespace sysio { namespace chain {

using resource_limits::resource_limits_manager;

using controller_index_set = index_set<
   account_index,
   account_metadata_index,
   account_ram_correction_index,
   global_property_multi_index,
   protocol_state_multi_index,
   dynamic_global_property_multi_index,
   block_summary_multi_index,
   transaction_multi_index,
   generated_transaction_multi_index,
   table_id_multi_index,
   code_index,
   database_header_multi_index
>;

using contract_database_index_set = index_set<
   key_value_index,
   index64_index,
   index128_index,
   index256_index,
   index_double_index,
   index_long_double_index
>;

class maybe_session {
   public:
      maybe_session() = default;

      maybe_session( maybe_session&& other)
         :_session(std::move(other._session))
      {
      }

      explicit maybe_session(database& db) {
         _session.emplace(db.start_undo_session(true));
      }

      maybe_session(const maybe_session&) = delete;

      void squash() {
         if (_session)
            _session->squash();
      }

      void undo() {
         if (_session)
            _session->undo();
      }

      void push() {
         if (_session)
            _session->push();
      }

      maybe_session& operator = ( maybe_session&& mv ) {
         if (mv._session) {
            _session.emplace(std::move(*mv._session));
            mv._session.reset();
         } else {
            _session.reset();
         }

         return *this;
      };

   private:
      std::optional<database::session>     _session;
};

struct building_block {
   building_block( const block_header_state& prev,
                   block_timestamp_type when,
                   uint16_t num_prev_blocks_to_confirm,
                   const vector<digest_type>& new_protocol_feature_activations )
   :_pending_block_header_state( prev.next( when, num_prev_blocks_to_confirm ) )
   ,_new_protocol_feature_activations( new_protocol_feature_activations )
   ,_trx_mroot_or_receipt_digests( digests_t{} )
   {}

   pending_block_header_state                 _pending_block_header_state;
   std::optional<producer_authority_schedule> _new_pending_producer_schedule;
   vector<digest_type>                        _new_protocol_feature_activations;
   size_t                                     _num_new_protocol_features_that_have_activated = 0;
   deque<transaction_metadata_ptr>            _pending_trx_metas;
   deque<transaction_receipt>                 _pending_trx_receipts; // boost deque in 1.71 with 1024 elements performs better
   std::variant<checksum256_type, digests_t>  _trx_mroot_or_receipt_digests;
   digests_t                                  _action_receipt_digests;
   deque<s_header>                            _s_header; // Added new functionality to pass many state headers to be included in block header extension
};

struct assembled_block {
   block_id_type                     _id;
   pending_block_header_state        _pending_block_header_state;
   deque<transaction_metadata_ptr>   _trx_metas;
   signed_block_ptr                  _unsigned_block;

   // if the _unsigned_block pre-dates block-signing authorities this may be present.
   std::optional<producer_authority_schedule> _new_producer_authority_cache;
};

struct completed_block {
   block_state_ptr                   _block_state;
};

using block_stage_type = std::variant<building_block, assembled_block, completed_block>;

struct pending_state {
   pending_state( maybe_session&& s, const block_header_state& prev,
                  block_timestamp_type when,
                  uint16_t num_prev_blocks_to_confirm,
                  const vector<digest_type>& new_protocol_feature_activations )
   :_db_session( std::move(s) )
   ,_block_stage( building_block( prev, when, num_prev_blocks_to_confirm, new_protocol_feature_activations ) )
   {}

   maybe_session                      _db_session;
   block_stage_type                   _block_stage;
   controller::block_status           _block_status = controller::block_status::ephemeral;
   std::optional<block_id_type>       _producer_block_id;
   controller::block_report           _block_report{};

   /** @pre _block_stage cannot hold completed_block alternative */
   const pending_block_header_state& get_pending_block_header_state()const {
      if( std::holds_alternative<building_block>(_block_stage) )
         return std::get<building_block>(_block_stage)._pending_block_header_state;

      return std::get<assembled_block>(_block_stage)._pending_block_header_state;
   }

   deque<transaction_metadata_ptr> extract_trx_metas() {
      if( std::holds_alternative<building_block>(_block_stage) )
         return std::move( std::get<building_block>(_block_stage)._pending_trx_metas );

      if( std::holds_alternative<assembled_block>(_block_stage) )
         return std::move( std::get<assembled_block>(_block_stage)._trx_metas );

      return std::get<completed_block>(_block_stage)._block_state->extract_trxs_metas();
   }

   bool is_protocol_feature_activated( const digest_type& feature_digest )const {
      if( std::holds_alternative<building_block>(_block_stage) ) {
        auto& bb = std::get<building_block>(_block_stage);
         const auto& activated_features = bb._pending_block_header_state.prev_activated_protocol_features->protocol_features;

         if( activated_features.find( feature_digest ) != activated_features.end() ) return true;

         if( bb._num_new_protocol_features_that_have_activated == 0 ) return false;

         auto end = bb._new_protocol_feature_activations.begin() + bb._num_new_protocol_features_that_have_activated;
         return (std::find( bb._new_protocol_feature_activations.begin(), end, feature_digest ) != end);
      }

      if( std::holds_alternative<assembled_block>(_block_stage) ) {
         // Calling is_protocol_feature_activated during the assembled_block stage is not efficient.
         // We should avoid doing it.
         // In fact for now it isn't even implemented.
         SYS_THROW( misc_exception,
                    "checking if protocol feature is activated in the assembled_block stage is not yet supported" );
         // TODO: implement this
      }

      const auto& activated_features = std::get<completed_block>(_block_stage)._block_state->activated_protocol_features->protocol_features;
      return (activated_features.find( feature_digest ) != activated_features.end());
   }

   void push() {
      _db_session.push();
   }
};

struct controller_impl {
   using block_root_processor_ptr = std::shared_ptr<block_root_processor>;
   enum class app_window_type {
      write, // Only main thread is running; read-only threads are not running.
             // All read-write and read-only tasks are sequentially executed.
      read   // Main thread and read-only threads are running read-ony tasks in parallel.
             // Read-write tasks are not being executed.
   };

   // LLVM sets the new handler, we need to reset this to throw a bad_alloc exception so we can possibly exit cleanly
   // and not just abort.
   struct reset_new_handler {
      reset_new_handler() { std::set_new_handler([](){ throw std::bad_alloc(); }); }
   };

   reset_new_handler               rnh; // placed here to allow for this to be set before constructing the other fields
   controller&                     self;
   std::function<void()>           shutdown;
   chainbase::database             db;
   block_log<signed_block>         blog;
   using state_log = block_log<block_header_state>;
   std::optional<state_log>        slog;
   std::optional<pending_state>    pending;
   block_state_ptr                 head;
   fork_database                   fork_db;
   resource_limits_manager         resource_limits;
   subjective_billing              subjective_bill;
   authorization_manager           authorization;
   protocol_feature_manager        protocol_features;
   controller::config              conf;
   const chain_id_type             chain_id; // read by thread_pool threads, value will not be changed
   bool                            replaying = false;
   bool                            is_producer_node = false; // true if node is configured as a block producer
   db_read_mode                    read_mode = db_read_mode::HEAD;
   bool                            in_trx_requiring_checks = false; ///< if true, checks that are normally skipped on replay (e.g. auth checks) cannot be skipped
   std::optional<fc::microseconds> subjective_cpu_leeway;
   bool                            trusted_producer_light_validation = false;
   uint32_t                        snapshot_head_block = 0;
   struct chain; // chain is a namespace so use an embedded type for the named_thread_pool tag
   named_thread_pool<chain>        thread_pool;
   deep_mind_handler*              deep_mind_logger = nullptr;
   bool                            okay_to_print_integrity_hash_on_stop = false;
   std::atomic<bool>               writing_snapshot = false;
   block_root_processor_ptr        merkle_processor;

   thread_local static platform_timer timer; // a copy for main thread and each read-only thread
#if defined(SYSIO_SYS_VM_RUNTIME_ENABLED) || defined(SYSIO_SYS_VM_JIT_RUNTIME_ENABLED)
   thread_local static vm::wasm_allocator wasm_alloc; // a copy for main thread and each read-only thread
#endif
   wasm_interface wasmif;
   app_window_type app_window = app_window_type::write;

   typedef pair<scope_name,action_name>                   handler_key;
   map< account_name, map<handler_key, apply_handler> >   apply_handlers;
   unordered_map< builtin_protocol_feature_t, std::function<void(controller_impl&)>, enum_hash<builtin_protocol_feature_t> > protocol_feature_activation_handlers;

   bool are_multiple_state_roots_supported() const;

   void set_s_headers( uint32_t block_num );

   void pop_block() {
      auto prev = fork_db.get_block( head->header.previous );

      if( !prev ) {
         SYS_ASSERT( fork_db.root()->id == head->header.previous, block_validate_exception, "attempt to pop beyond last irreversible block" );
         prev = fork_db.root();
      }

      SYS_ASSERT( head->block, block_validate_exception, "attempting to pop a block that was sparsely loaded from a snapshot");

      head = prev;

      db.undo();

      protocol_features.popped_blocks_to( prev->block_num );
   }

   template<builtin_protocol_feature_t F>
   void on_activation();

   template<builtin_protocol_feature_t F>
   inline void set_activation_handler() {
      auto res = protocol_feature_activation_handlers.emplace( F, &controller_impl::on_activation<F> );
      SYS_ASSERT( res.second, misc_exception, "attempting to set activation handler twice" );
   }

   inline void trigger_activation_handler( builtin_protocol_feature_t f ) {
      auto itr = protocol_feature_activation_handlers.find( f );
      if( itr == protocol_feature_activation_handlers.end() ) return;
      (itr->second)( *this );
   }

   void set_apply_handler( account_name receiver, account_name contract, action_name action, apply_handler v ) {
      apply_handlers[receiver][make_pair(contract,action)] = v;
   }

   std::optional<state_log> create_slog(bool keep_state_log, const std::filesystem::path& blocks_dir, const block_log_config& config) {
      if( !keep_state_log ) {
         return {};
      }
      ilog( "Keep a Block State Log");
      return state_log( blocks_dir, config );
   }

   controller_impl( const controller::config& cfg, controller& s, protocol_feature_set&& pfs, const chain_id_type& chain_id )
   :rnh(),
    self(s),
    db( cfg.state_dir,
        cfg.read_only ? database::read_only : database::read_write,
        cfg.state_size, false, cfg.db_map_mode ),
    blog( cfg.blocks_dir, cfg.blog ),
    slog( create_slog(cfg.keep_state_log, cfg.blocks_dir, cfg.blog ) ),
    fork_db( cfg.blocks_dir / config::reversible_blocks_dir_name ),
    resource_limits( db, [&s](bool is_trx_transient) { return s.get_deep_mind_logger(is_trx_transient); }),
    authorization( s, db ),
    protocol_features( std::move(pfs), [&s](bool is_trx_transient) { return s.get_deep_mind_logger(is_trx_transient); } ),
    conf( cfg ),
    chain_id( chain_id ),
    read_mode( cfg.read_mode ),
    thread_pool(),
    wasmif( conf.wasm_runtime, conf.sysvmoc_tierup, db, conf.state_dir, conf.sysvmoc_config, !conf.profile_accounts.empty() )
   {
      fork_db.open( [this]( block_timestamp_type timestamp,
                            const flat_set<digest_type>& cur_features,
                            const vector<digest_type>& new_features )
                           { check_protocol_features( timestamp, cur_features, new_features ); }
      );

      thread_pool.start( cfg.thread_pool_size, [this]( const fc::exception& e ) {
         elog( "Exception in chain thread pool, exiting: ${e}", ("e", e.to_detail_string()) );
         if( shutdown ) shutdown();
      } );

      set_activation_handler<builtin_protocol_feature_t::preactivate_feature>();
      set_activation_handler<builtin_protocol_feature_t::replace_deferred>();
      set_activation_handler<builtin_protocol_feature_t::get_sender>();
      set_activation_handler<builtin_protocol_feature_t::webauthn_key>();
      set_activation_handler<builtin_protocol_feature_t::wtmsig_block_signatures>();
      set_activation_handler<builtin_protocol_feature_t::action_return_value>();
      set_activation_handler<builtin_protocol_feature_t::configurable_wasm_limits>();
      set_activation_handler<builtin_protocol_feature_t::blockchain_parameters>();
      set_activation_handler<builtin_protocol_feature_t::get_code_hash>();
      set_activation_handler<builtin_protocol_feature_t::get_block_num>();
      set_activation_handler<builtin_protocol_feature_t::crypto_primitives>();
      set_activation_handler<builtin_protocol_feature_t::bls_primitives>();
      set_activation_handler<builtin_protocol_feature_t::disable_deferred_trxs_stage_2>();
      set_activation_handler<builtin_protocol_feature_t::em_key>();
      set_activation_handler<builtin_protocol_feature_t::ed_key>();


      self.irreversible_block.connect([this](const block_state_ptr& bsp) {
         wasmif.current_lib(bsp->block_num);
      });




#define SET_APP_HANDLER( receiver, contract, action) \
   set_apply_handler( account_name(#receiver), account_name(#contract), action_name(#action), \
                      &BOOST_PP_CAT(apply_, BOOST_PP_CAT(contract, BOOST_PP_CAT(_,action) ) ) )

   SET_APP_HANDLER( sysio, sysio, newaccount );
   SET_APP_HANDLER( sysio, sysio, setcode );
   SET_APP_HANDLER( sysio, sysio, setabi );
   SET_APP_HANDLER( sysio, sysio, updateauth );
   SET_APP_HANDLER( sysio, sysio, deleteauth );
   SET_APP_HANDLER( sysio, sysio, linkauth );
   SET_APP_HANDLER( sysio, sysio, unlinkauth );

   // **Roa Change** Handler for native action reducepolicy: Had to manually expand due to sysio.roa account name
   set_apply_handler( account_name("sysio.roa"), account_name("sysio.roa"),
      action_name("reducepolicy"), &apply_roa_reducepolicy );
/*
   SET_APP_HANDLER( sysio, sysio, postrecovery );
   SET_APP_HANDLER( sysio, sysio, passrecovery );
   SET_APP_HANDLER( sysio, sysio, vetorecovery );
*/

   SET_APP_HANDLER( sysio, sysio, canceldelay );
   }

   /**
    *  Plugins / observers listening to signals emited (such as accepted_transaction) might trigger
    *  errors and throw exceptions. Unless those exceptions are caught it could impact consensus and/or
    *  cause a node to fork.
    *
    *  If it is ever desirable to let a signal handler bubble an exception out of this method
    *  a full audit of its uses needs to be undertaken.
    *
    */
   template<typename Signal, typename Arg>
   void emit( const Signal& s, Arg&& a ) {
      try {
         s( std::forward<Arg>( a ));
      } catch (std::bad_alloc& e) {
         wlog( "std::bad_alloc: ${w}", ("w", e.what()) );
         throw e;
      } catch (boost::interprocess::bad_alloc& e) {
         wlog( "boost::interprocess::bad alloc: ${w}", ("w", e.what()) );
         throw e;
      } catch ( controller_emit_signal_exception& e ) {
         wlog( "controller_emit_signal_exception: ${details}", ("details", e.to_detail_string()) );
         throw e;
      } catch ( fc::exception& e ) {
         wlog( "fc::exception: ${details}", ("details", e.to_detail_string()) );
      } catch ( std::exception& e ) {
         wlog( "std::exception: ${details}", ("details", e.what()) );
      } catch ( ... ) {
         wlog( "signal handler threw exception" );
      }
   }

   void dmlog_applied_transaction(const transaction_trace_ptr& t, const signed_transaction* trx = nullptr) {
      // dmlog_applied_transaction is called by push_scheduled_transaction
      // where transient transactions are not possible, and by push_transaction
      // only when the transaction is not transient
      if (auto dm_logger = get_deep_mind_logger(false)) {
         if (trx && is_onblock(*t))
            dm_logger->on_onblock(*trx);
         dm_logger->on_applied_transaction(self.head_block_num() + 1, t);
      }
   }

   void log_irreversible() {
      SYS_ASSERT( fork_db.root(), fork_database_exception, "fork database not properly initialized" );

      const std::optional<block_id_type> log_head_id = blog.head_id();
      const bool valid_log_head = !!log_head_id;

      const auto lib_num = valid_log_head ? block_header::num_from_id(*log_head_id) : (blog.first_block_num() - 1);

      auto root_id = fork_db.root()->id;

      if( valid_log_head ) {
         SYS_ASSERT( root_id == log_head_id, fork_database_exception, "fork database root does not match block log head" );
      } else {
         SYS_ASSERT( fork_db.root()->block_num == lib_num, fork_database_exception,
                     "The first block ${lib_num} when starting with an empty block log should be the block after fork database root ${bn}.",
                     ("lib_num", lib_num)("bn", fork_db.root()->block_num) );
      }

      const auto fork_head = fork_db_head();

      if( fork_head->dpos_irreversible_blocknum <= lib_num )
         return;

      auto branch = fork_db.fetch_branch( fork_head->id, fork_head->dpos_irreversible_blocknum );
      try {

         std::vector<std::future<std::vector<char>>> v;
         std::vector<std::future<std::vector<char>>> sv;
         v.reserve( branch.size() );
         if (slog.has_value()) {
            sv.reserve( branch.size() );
         }
         for( auto bitr = branch.rbegin(); bitr != branch.rend(); ++bitr ) {
            v.emplace_back( post_async_task( thread_pool.get_executor(), [b=(*bitr)->block]() { return fc::raw::pack(*b); } ) );
            if (slog.has_value()) {
               sv.emplace_back( post_async_task( thread_pool.get_executor(), [b=(*bitr)]() {
                  return fc::raw::pack(static_cast<const block_header_state&>(*b));
               } ) );
            }
         }
         auto it = v.begin();
         auto sit = sv.begin();

         for( auto bitr = branch.rbegin(); bitr != branch.rend(); ++bitr ) {
            if( read_mode == db_read_mode::IRREVERSIBLE ) {
               controller::block_report br;
               apply_block( br, *bitr, controller::block_status::complete, trx_meta_cache_lookup{} );
            }

            emit( self.irreversible_block, *bitr );

            // blog.append could fail due to failures like running out of space.
            // Do it before commit so that in case it throws, DB can be rolled back.
            blog.append( (*bitr)->block, (*bitr)->id, it->get() );
            if( slog.has_value() ) {
               slog->append( *bitr, (*bitr)->id, sit->get() );
               ++sit;
            }

            ++it;

            db.commit( (*bitr)->block_num );
            root_id = (*bitr)->id;
         }
      } catch( std::exception& ) {
         if( root_id != fork_db.root()->id ) {
            fork_db.advance_root( root_id );
         }
         throw;
      }

      //db.commit( fork_head->dpos_irreversible_blocknum ); // redundant

      if( root_id != fork_db.root()->id ) {
         branch.emplace_back(fork_db.root());
         fork_db.advance_root( root_id );
      }

      // delete branch in thread pool
      boost::asio::post( thread_pool.get_executor(), [branch{std::move(branch)}]() {} );
   }

   /**
    *  Sets fork database head to the genesis state.
    */
   void initialize_blockchain_state(const genesis_state& genesis) {
      wlog( "Initializing new blockchain with genesis state" );
      producer_authority_schedule initial_schedule = { 0, { producer_authority{config::system_account_name, block_signing_authority_v0{ 1, {{genesis.initial_key, 1}} } } } };
      legacy::producer_schedule_type initial_legacy_schedule{ 0, {{config::system_account_name, genesis.initial_key}} };

      block_header_state genheader;
      genheader.active_schedule                = initial_schedule;
      genheader.pending_schedule.schedule      = initial_schedule;
      // NOTE: if wtmsig block signatures are enabled at genesis time this should be the hash of a producer authority schedule
      genheader.pending_schedule.schedule_hash = fc::sha256::hash(initial_legacy_schedule);
      genheader.header.timestamp               = genesis.initial_timestamp;
      genheader.header.action_mroot            = genesis.compute_chain_id();
      genheader.id                             = genheader.header.calculate_id();
      genheader.block_num                      = genheader.header.block_num();

      head = std::make_shared<block_state>();
      static_cast<block_header_state&>(*head) = genheader;
      head->activated_protocol_features = std::make_shared<protocol_feature_activation_set>();
      head->block = std::make_shared<signed_block>(genheader.header);
      db.set_revision( head->block_num );
      initialize_database(genesis);
   }

   void replay(std::function<bool()> check_shutdown) {
      auto blog_head = blog.head();
      if( !fork_db.root() ) {
         fork_db.reset( *head );
         if (!blog_head)
            return;
      }

      replaying = true;
      auto start_block_num = head->block_num + 1;
      auto start = fc::time_point::now();

      std::exception_ptr except_ptr;

      if( blog_head && start_block_num <= blog_head->block_num() ) {
         ilog( "existing block log, attempting to replay from ${s} to ${n} blocks",
               ("s", start_block_num)("n", blog_head->block_num()) );
         try {
            while( auto next = blog.read_block_by_num( head->block_num + 1 ) ) {
               replay_push_block( next, controller::block_status::irreversible );
               if( check_shutdown() ) break;
               if( next->block_num() % 500 == 0 ) {
                  ilog( "${n} of ${head}", ("n", next->block_num())("head", blog_head->block_num()) );
               }
            }
         } catch(  const database_guard_exception& e ) {
            except_ptr = std::current_exception();
         }
         ilog( "${n} irreversible blocks replayed", ("n", 1 + head->block_num - start_block_num) );

         auto pending_head = fork_db.pending_head();
         if( pending_head ) {
            ilog( "fork database head ${h}, root ${r}", ("h", pending_head->block_num)( "r", fork_db.root()->block_num ) );
            if( pending_head->block_num < head->block_num || head->block_num < fork_db.root()->block_num ) {
               ilog( "resetting fork database with new last irreversible block as the new root: ${id}", ("id", head->id) );
               fork_db.reset( *head );
            } else if( head->block_num != fork_db.root()->block_num ) {
               auto new_root = fork_db.search_on_branch( pending_head->id, head->block_num );
               SYS_ASSERT( new_root, fork_database_exception,
                           "unexpected error: could not find new LIB in fork database" );
               ilog( "advancing fork database root to new last irreversible block within existing fork database: ${id}",
                     ("id", new_root->id) );
               fork_db.mark_valid( new_root );
               fork_db.advance_root( new_root->id );
            }
         }

         // if the irreverible log is played without undo sessions enabled, we need to sync the
         // revision ordinal to the appropriate expected value here.
         if( self.skip_db_sessions( controller::block_status::irreversible ) )
            db.set_revision( head->block_num );
      } else {
         ilog( "no irreversible blocks need to be replayed" );
      }

      if (snapshot_head_block != 0 && !blog_head) {
         // loading from snapshot without a block log so fork_db can't be considered valid
         fork_db.reset( *head );
      } else if( !except_ptr && !check_shutdown() && fork_db.head() ) {
         auto head_block_num = head->block_num;
         auto branch = fork_db.fetch_branch( fork_db.head()->id );
         int rev = 0;
         for( auto i = branch.rbegin(); i != branch.rend(); ++i ) {
            if( check_shutdown() ) break;
            if( (*i)->block_num <= head_block_num ) continue;
            ++rev;
            replay_push_block( (*i)->block, controller::block_status::validated );
         }
         ilog( "${n} reversible blocks replayed", ("n",rev) );
      }

      if( !fork_db.head() ) {
         fork_db.reset( *head );
      }

      auto end = fc::time_point::now();
      ilog( "replayed ${n} blocks in ${duration} seconds, ${mspb} ms/block",
            ("n", head->block_num + 1 - start_block_num)("duration", (end-start).count()/1000000)
            ("mspb", ((end-start).count()/1000.0)/(head->block_num-start_block_num)) );
      replaying = false;

      if( except_ptr ) {
         std::rethrow_exception( except_ptr );
      }
   }

   void startup(std::function<void()> shutdown, std::function<bool()> check_shutdown, const snapshot_reader_ptr& snapshot) {
      SYS_ASSERT( snapshot, snapshot_exception, "No snapshot reader provided" );
      this->shutdown = shutdown;
      try {
         auto snapshot_load_start_time = fc::time_point::now();
         snapshot->validate();
         if( auto blog_head = blog.head() ) {
            ilog( "Starting initialization from snapshot and block log ${b}-${e}, this may take a significant amount of time",
                  ("b", blog.first_block_num())("e", blog_head->block_num()) );
            read_from_snapshot( snapshot, blog.first_block_num(), blog_head->block_num() );
         } else {
            ilog( "Starting initialization from snapshot and no block log, this may take a significant amount of time" );
            read_from_snapshot( snapshot, 0, std::numeric_limits<uint32_t>::max() );
            SYS_ASSERT( head->block_num > 0, snapshot_exception,
                        "Snapshot indicates controller head at block number 0, but that is not allowed. "
                        "Snapshot is invalid." );
            blog.reset( chain_id, head->block_num + 1 );
            // slog will be reset, if need be, after replay is complete
         }
         ilog( "Snapshot loaded, lib: ${lib}", ("lib", head->block_num) );

         init(std::move(check_shutdown));
         auto snapshot_load_time = (fc::time_point::now() - snapshot_load_start_time).to_seconds();
         ilog( "Finished initialization from snapshot (snapshot load time was ${t}s)", ("t", snapshot_load_time) );
      } catch (boost::interprocess::bad_alloc& e) {
         elog( "Failed initialization from snapshot - db storage not configured to have enough storage for the provided snapshot, please increase and retry snapshot" );
         shutdown();
      }
   }

   void startup(std::function<void()> shutdown, std::function<bool()> check_shutdown, const genesis_state& genesis) {
      SYS_ASSERT( db.revision() < 1, database_exception, "This version of controller::startup only works with a fresh state database." );
      const auto& genesis_chain_id = genesis.compute_chain_id();
      SYS_ASSERT( genesis_chain_id == chain_id, chain_id_type_exception,
                  "genesis state provided to startup corresponds to a chain ID (${genesis_chain_id}) that does not match the chain ID that controller was constructed with (${controller_chain_id})",
                  ("genesis_chain_id", genesis_chain_id)("controller_chain_id", chain_id)
      );

      this->shutdown = std::move(shutdown);
      if( fork_db.head() ) {
         if( read_mode == db_read_mode::IRREVERSIBLE && fork_db.head()->id != fork_db.root()->id ) {
            fork_db.rollback_head_to_root();
         }
         wlog( "No existing chain state. Initializing fresh blockchain state." );
      } else {
         wlog( "No existing chain state or fork database. Initializing fresh blockchain state and resetting fork database.");
      }
      initialize_blockchain_state(genesis); // sets head to genesis state

      if( !fork_db.head() ) {
         fork_db.reset( *head );
      }

      if( blog.head() ) {
         SYS_ASSERT( blog.first_block_num() == 1, block_log_exception,
                     "block log does not start with genesis block"
         );
         // slog will be adjusted after replay, if it needs to be
      } else {
         blog.reset( genesis, head->block );
         if( slog.has_value() ) {
            slog->reset( genesis, std::dynamic_pointer_cast<block_header_state>(head) );
         }
      }
      init(std::move(check_shutdown));
   }

   void startup(std::function<void()> shutdown, std::function<bool()> check_shutdown) {
      SYS_ASSERT( db.revision() >= 1, database_exception, "This version of controller::startup does not work with a fresh state database." );
      SYS_ASSERT( fork_db.head(), fork_database_exception, "No existing fork database despite existing chain state. Replay required." );

      this->shutdown = std::move(shutdown);
      uint32_t lib_num = fork_db.root()->block_num;
      auto first_block_num = blog.first_block_num();
      if( auto blog_head = blog.head() ) {
         SYS_ASSERT( first_block_num <= lib_num && lib_num <= blog_head->block_num(),
                     block_log_exception,
                     "block log (ranging from ${block_log_first_num} to ${block_log_last_num}) does not contain the last irreversible block (${fork_db_lib})",
                     ("block_log_first_num", first_block_num)
                     ("block_log_last_num", blog_head->block_num())
                     ("fork_db_lib", lib_num)
         );
         lib_num = blog_head->block_num();
      } else {
         if( first_block_num != (lib_num + 1) ) {
            blog.reset( chain_id, lib_num + 1 );
            // slog will be reset, if need be, after replay is complete
         }
      }

      if( read_mode == db_read_mode::IRREVERSIBLE && fork_db.head()->id != fork_db.root()->id ) {
         fork_db.rollback_head_to_root();
      }
      head = fork_db.head();

      init(std::move(check_shutdown));
   }


   static auto validate_db_version( const chainbase::database& db ) {
      // check database version
      const auto& header_idx = db.get_index<database_header_multi_index>().indices().get<by_id>();

      SYS_ASSERT(header_idx.begin() != header_idx.end(), bad_database_version_exception,
                 "state database version pre-dates versioning, please restore from a compatible snapshot or replay!");

      auto header_itr = header_idx.begin();
      header_itr->validate();

      return header_itr;
   }

   void init(std::function<bool()> check_shutdown) {
      auto header_itr = validate_db_version( db );

      {
         const auto& state_chain_id = db.get<global_property_object>().chain_id;
         SYS_ASSERT( state_chain_id == chain_id, chain_id_type_exception,
                     "chain ID in state (${state_chain_id}) does not match the chain ID that controller was constructed with (${controller_chain_id})",
                     ("state_chain_id", state_chain_id)("controller_chain_id", chain_id)
         );
      }

      // upgrade to the latest compatible version
      if (header_itr->version != database_header_object::current_version) {
         db.modify(*header_itr, [](auto& header) {
            header.version = database_header_object::current_version;
         });
      }

      // At this point head != nullptr
      SYS_ASSERT( db.revision() >= head->block_num, fork_database_exception,
                  "fork database head (${head}) is inconsistent with state (${db})",
                  ("db",db.revision())("head",head->block_num) );

      if( db.revision() > head->block_num ) {
         wlog( "database revision (${db}) is greater than head block number (${head}), "
               "attempting to undo pending changes",
               ("db",db.revision())("head",head->block_num) );
      }
      while( db.revision() > head->block_num ) {
         db.undo();
      }

      protocol_features.init( db );

      // At startup, no transaction specific logging is possible
      if (auto dm_logger = get_deep_mind_logger(false)) {
         dm_logger->on_startup(db, head->block_num);
      }

      if( conf.integrity_hash_on_start )
         ilog( "chain database started with hash: ${hash}", ("hash", calculate_integrity_hash()) );
      okay_to_print_integrity_hash_on_stop = true;

      replay( check_shutdown ); // replay any irreversible and reversible blocks ahead of current head

      if( check_shutdown() ) return;

      if( slog.has_value() ) {
         // need to reset the slog if
         // it is not initialized
         // its span of block nums are not consistent with the current block number
         if( !slog->version() ||
             !slog->head() ) {
            // since the slog is not needed for replay, just initialize it with a chain id
            slog->reset( chain_id, blog.head()->block_num() + 1 );
         }
         else {
            auto command = [state_block_num=slog->head()->block_num,head_block_num=blog.head()->block_num()]() {
               return (state_block_num < head_block_num) ? std::string("block-log") : std::string("block-state-log");
            };
            SYS_ASSERT( slog->head()->block_num == blog.head()->block_num(),
                        block_log_exception,
                        "Block log and block state log need to be at the same block number. This can be fixed by running"
                        "\"sys-util ${command} trim-blocklog --first 1 --last ${last} --blocks-dir <your blocks dir>\". "
                        "The other log is at block num: ${bn} ",
                        ("command", command())
                        ("last", std::min(slog->head()->block_num, blog.head()->block_num()))
                        ("bn", std::max(slog->head()->block_num, blog.head()->block_num()))
            );
         }
      }

      // At this point head != nullptr && fork_db.head() != nullptr && fork_db.root() != nullptr.
      // Furthermore, fork_db.root()->block_num <= lib_num.
      // Also, even though blog.head() may still be nullptr, blog.first_block_num() is guaranteed to be lib_num + 1.

      if( read_mode != db_read_mode::IRREVERSIBLE
          && fork_db.pending_head()->id != fork_db.head()->id
          && fork_db.head()->id == fork_db.root()->id
      ) {
         wlog( "read_mode has changed from irreversible: applying best branch from fork database" );

         for( auto pending_head = fork_db.pending_head();
              pending_head->id != fork_db.head()->id;
              pending_head = fork_db.pending_head()
         ) {
            wlog( "applying branch from fork database ending with block: ${id}", ("id", pending_head->id) );
            controller::block_report br;
            maybe_switch_forks( br, pending_head, controller::block_status::complete, forked_branch_callback{}, trx_meta_cache_lookup{} );
         }
      }
   }

   ~controller_impl() {
      thread_pool.stop();
      pending.reset();
      //only log this not just if configured to, but also if initialization made it to the point we'd log the startup too
      if(okay_to_print_integrity_hash_on_stop && conf.integrity_hash_on_stop)
         ilog( "chain database stopped with hash: ${hash}", ("hash", calculate_integrity_hash()) );
   }

   void add_indices() {
      controller_index_set::add_indices(db);
      contract_database_index_set::add_indices(db);

      authorization.add_indices();
      resource_limits.add_indices();
      if (merkle_processor) {
         merkle_processor->add_indices();
      }
   }

   void clear_all_undo() {
      // Rewind the database to the last irreversible block
      db.undo_all();
      /*
      FC_ASSERT(db.revision() == self.head_block_num(),
                  "Chainbase revision does not match head block num",
                  ("rev", db.revision())("head_block", self.head_block_num()));
                  */
   }

   void add_contract_tables_to_snapshot( const snapshot_writer_ptr& snapshot ) const {
      snapshot->write_section("contract_tables", [this]( auto& section ) {
         index_utils<table_id_multi_index>::walk(db, [this, &section]( const table_id_object& table_row ){
            // add a row for the table
            section.add_row(table_row, db);

            // followed by a size row and then N data rows for each type of table
            contract_database_index_set::walk_indices([this, &section, &table_row]( auto utils ) {
               using utils_t = decltype(utils);
               using value_t = typename decltype(utils)::index_t::value_type;
               using by_table_id = object_to_table_id_tag_t<value_t>;

               auto tid_key = boost::make_tuple(table_row.id);
               auto next_tid_key = boost::make_tuple(table_id_object::id_type(table_row.id._id + 1));

               unsigned_int size = utils_t::template size_range<by_table_id>(db, tid_key, next_tid_key);
               section.add_row(size, db);

               utils_t::template walk_range<by_table_id>(db, tid_key, next_tid_key, [this, &section]( const auto &row ) {
                  section.add_row(row, db);
               });
            });
         });
      });
   }

   void read_contract_tables_from_snapshot( const snapshot_reader_ptr& snapshot ) {
      snapshot->read_section("contract_tables", [this]( auto& section ) {
         bool more = !section.empty();
         while (more) {
            // read the row for the table
            table_id_object::id_type t_id;
            index_utils<table_id_multi_index>::create(db, [this, &section, &t_id](auto& row) {
               section.read_row(row, db);
               t_id = row.id;
            });

            // read the size and data rows for each type of table
            contract_database_index_set::walk_indices([this, &section, &t_id, &more](auto utils) {
               using utils_t = decltype(utils);

               unsigned_int size;
               more = section.read_row(size, db);

               for (size_t idx = 0; idx < size.value; idx++) {
                  utils_t::create(db, [this, &section, &more, &t_id](auto& row) {
                     row.t_id = t_id;
                     more = section.read_row(row, db);
                  });
               }
            });
         }
      });
   }

   void add_to_snapshot( const snapshot_writer_ptr& snapshot ) {
      // clear in case the previous call to clear did not finish in time of deadline
      clear_expired_input_transactions( fc::time_point::maximum() );

      snapshot->write_section<chain_snapshot_header>([this]( auto &section ){
         section.add_row(chain_snapshot_header(), db);
      });

      snapshot->write_section<block_state>([this]( auto &section ){
         section.template add_row<block_header_state>(*head, db);
      });

      controller_index_set::walk_indices([this, &snapshot]( auto utils ){
         using value_t = typename decltype(utils)::index_t::value_type;

         // skip the table_id_object as its inlined with contract tables section
         if (std::is_same<value_t, table_id_object>::value) {
            return;
         }

         // skip the database_header as it is only relevant to in-memory database
         if (std::is_same<value_t, database_header_object>::value) {
            return;
         }

         snapshot->write_section<value_t>([this]( auto& section ){
            decltype(utils)::walk(db, [this, &section]( const auto &row ) {
               section.add_row(row, db);
            });
         });
      });

      add_contract_tables_to_snapshot(snapshot);

      authorization.add_to_snapshot(snapshot);
      resource_limits.add_to_snapshot(snapshot);
      if (merkle_processor) {
         merkle_processor->add_to_snapshot(snapshot);
      }
   }

   void read_from_snapshot( const snapshot_reader_ptr& snapshot, uint32_t blog_start, uint32_t blog_end ) {
      chain_snapshot_header header;
      snapshot->read_section<chain_snapshot_header>([this, &header]( auto &section ){
         section.read_row(header, db);
         header.validate();
      });

      { /// load and upgrade the block header state
         block_header_state head_header_state;

         snapshot->read_section<block_state>([this,&head_header_state]( auto &section ){
            section.read_row(head_header_state, db);
         });

         snapshot_head_block = head_header_state.block_num;
         SYS_ASSERT( blog_start <= (snapshot_head_block + 1) && snapshot_head_block <= blog_end,
                     block_log_exception,
                     "Block log is provided with snapshot but does not contain the head block from the snapshot nor a block right after it",
                     ("snapshot_head_block", snapshot_head_block)
                     ("block_log_first_num", blog_start)
                     ("block_log_last_num", blog_end)
         );

         head = std::make_shared<block_state>();
         static_cast<block_header_state&>(*head) = head_header_state;
      }

      controller_index_set::walk_indices([this, &snapshot]( auto utils ){
         using value_t = typename decltype(utils)::index_t::value_type;

         // skip the table_id_object as its inlined with contract tables section
         if (std::is_same<value_t, table_id_object>::value) {
            return;
         }

         // skip the database_header as it is only relevant to in-memory database
         if (std::is_same<value_t, database_header_object>::value) {
            return;
         }

         snapshot->read_section<value_t>([this]( auto& section ) {
            bool more = !section.empty();
            while(more) {
               decltype(utils)::create(db, [this, &section, &more]( auto &row ) {
                  more = section.read_row(row, db);
               });
            }
         });
      });

      read_contract_tables_from_snapshot(snapshot);

      authorization.read_from_snapshot(snapshot);
      resource_limits.read_from_snapshot(snapshot);

      if (merkle_processor) {
         merkle_processor->read_from_snapshot(snapshot);
      }

      db.set_revision( head->block_num );
      db.create<database_header_object>([](const auto& header){
         // nothing to do
      });

      const auto& gpo = db.get<global_property_object>();
      SYS_ASSERT( gpo.chain_id == chain_id, chain_id_type_exception,
                  "chain ID in snapshot (${snapshot_chain_id}) does not match the chain ID that controller was constructed with (${controller_chain_id})",
                  ("snapshot_chain_id", gpo.chain_id)("controller_chain_id", chain_id)
      );
   }

   fc::sha256 calculate_integrity_hash() {
      fc::sha256::encoder enc;
      auto hash_writer = std::make_shared<integrity_hash_snapshot_writer>(enc);
      add_to_snapshot(hash_writer);
      hash_writer->finalize();

      return enc.result();
   }

   void create_native_account( const fc::time_point& initial_timestamp, account_name name, const authority& owner, const authority& active, bool is_privileged = false ) {
      db.create<account_object>([&](auto& a) {
         a.name = name;
         a.creation_date = initial_timestamp;

         if( name == config::system_account_name ) {
            // The initial sysio ABI value affects consensus; see  https://github.com/SYSIO/sys/issues/7794
            // TODO: This doesn't charge RAM; a fix requires a consensus upgrade.
            a.abi.assign(sysio_abi_bin, sizeof(sysio_abi_bin));
         }
      });
      db.create<account_metadata_object>([&](auto & a) {
         a.name = name;
         a.set_privileged( is_privileged );
      });

      const auto& owner_permission  = authorization.create_permission(name, config::owner_name, 0,
                                                                      owner, false, initial_timestamp );
      const auto& active_permission = authorization.create_permission(name, config::active_name, owner_permission.id,
                                                                      active, false, initial_timestamp );

      resource_limits.initialize_account(name, false);

      int64_t ram_delta = config::overhead_per_account_ram_bytes;
      ram_delta += 2*config::billable_size_v<permission_object>;
      ram_delta += owner_permission.auth.get_billable_size();
      ram_delta += active_permission.auth.get_billable_size();

      // This is only called at startup, no transaction specific logging is possible
      if (auto dm_logger = get_deep_mind_logger(false)) {
         dm_logger->on_ram_trace(RAM_EVENT_ID("${name}", ("name", name)), "account", "add", "newaccount");
      }

      resource_limits.add_pending_ram_usage(name, ram_delta, false); // false for doing dm logging
      resource_limits.verify_account_ram_usage(name);
   }

   void initialize_database(const genesis_state& genesis) {
      // create the database header sigil
      db.create<database_header_object>([&]( auto& header ){
         // nothing to do for now
      });

      // Initialize block summary index
      for (int i = 0; i < 0x10000; i++)
         db.create<block_summary_object>([&](block_summary_object&) {});

      const auto& tapos_block_summary = db.get<block_summary_object>(1);
      db.modify( tapos_block_summary, [&]( auto& bs ) {
         bs.block_id = head->id;
      });

      genesis.initial_configuration.validate();
      db.create<global_property_object>([&genesis,&chain_id=this->chain_id](auto& gpo ){
         gpo.configuration = genesis.initial_configuration;
         // TODO: Update this when genesis protocol features are enabled.
         gpo.wasm_configuration = genesis_state::default_initial_wasm_configuration;
         gpo.chain_id = chain_id;
      });

      db.create<protocol_state_object>([&](auto& pso ){
         pso.num_supported_key_types = config::genesis_num_supported_key_types;
         for( const auto& i : genesis_intrinsics ) {
            add_intrinsic_to_whitelist( pso.whitelisted_intrinsics, i );
         }
      });

      db.create<dynamic_global_property_object>([](auto&){});

      authorization.initialize_database();
      resource_limits.initialize_database();
      if (merkle_processor) {
         merkle_processor->initialize_database();
      }

      authority system_auth(genesis.initial_key);
      create_native_account( genesis.initial_timestamp, config::system_account_name, system_auth, system_auth, true );

      auto empty_authority = authority(1, {}, {});
      auto active_producers_authority = authority(1, {}, {});
      active_producers_authority.accounts.push_back({{config::system_account_name, config::active_name}, 1});

      create_native_account( genesis.initial_timestamp, config::null_account_name, empty_authority, empty_authority );
      create_native_account( genesis.initial_timestamp, config::producers_account_name, empty_authority, active_producers_authority );
      const auto& active_permission       = authorization.get_permission({config::producers_account_name, config::active_name});
      const auto& majority_permission     = authorization.create_permission( config::producers_account_name,
                                                                             config::majority_producers_permission_name,
                                                                             active_permission.id,
                                                                             active_producers_authority,
                                                                             false,
                                                                             genesis.initial_timestamp );
                                            authorization.create_permission( config::producers_account_name,
                                                                             config::minority_producers_permission_name,
                                                                             majority_permission.id,
                                                                             active_producers_authority,
                                                                             false,
                                                                             genesis.initial_timestamp );

   }

   // The returned scoped_exit should not exceed the lifetime of the pending which existed when make_block_restore_point was called.
   fc::scoped_exit<std::function<void()>> make_block_restore_point( bool is_read_only = false ) {
      if ( is_read_only ) {
         std::function<void()> callback = []() { };
         return fc::make_scoped_exit( std::move(callback) );
      }

      auto& bb = std::get<building_block>(pending->_block_stage);
      auto orig_trx_receipts_size           = bb._pending_trx_receipts.size();
      auto orig_trx_metas_size              = bb._pending_trx_metas.size();
      auto orig_trx_receipt_digests_size    = std::holds_alternative<digests_t>(bb._trx_mroot_or_receipt_digests) ?
                                              std::get<digests_t>(bb._trx_mroot_or_receipt_digests).size() : 0;
      auto orig_action_receipt_digests_size = bb._action_receipt_digests.size();
      std::function<void()> callback = [this,
            orig_trx_receipts_size,
            orig_trx_metas_size,
            orig_trx_receipt_digests_size,
            orig_action_receipt_digests_size]()
      {
         auto& bb = std::get<building_block>(pending->_block_stage);
         bb._pending_trx_receipts.resize(orig_trx_receipts_size);
         bb._pending_trx_metas.resize(orig_trx_metas_size);
         if( std::holds_alternative<digests_t>(bb._trx_mroot_or_receipt_digests) )
            std::get<digests_t>(bb._trx_mroot_or_receipt_digests).resize(orig_trx_receipt_digests_size);
         bb._action_receipt_digests.resize(orig_action_receipt_digests_size);
      };

      return fc::make_scoped_exit( std::move(callback) );
   }

   transaction_trace_ptr apply_onerror( const generated_transaction& gtrx,
                                        fc::time_point block_deadline,
                                        fc::microseconds max_transaction_time,
                                        fc::time_point start,
                                        uint32_t& cpu_time_to_bill_us, // only set on failure
                                        uint32_t billed_cpu_time_us,
                                        bool explicit_billed_cpu_time = false,
                                        bool enforce_whiteblacklist = true
                                      )
   {
      signed_transaction etrx;
      // Deliver onerror action containing the failed deferred transaction directly back to the sender.
      etrx.actions.emplace_back( vector<permission_level>{{gtrx.sender, config::active_name}},
                                 onerror( gtrx.sender_id, gtrx.packed_trx.data(), gtrx.packed_trx.size() ) );
      if( self.is_builtin_activated( builtin_protocol_feature_t::no_duplicate_deferred_id ) ) {
         etrx.expiration = time_point_sec();
         etrx.ref_block_num = 0;
         etrx.ref_block_prefix = 0;
      } else {
         etrx.expiration = time_point_sec{self.pending_block_time() + fc::microseconds(999'999)}; // Round up to nearest second to avoid appearing expired
         etrx.set_reference_block( self.head_block_id() );
      }

      transaction_checktime_timer trx_timer(timer);
      const packed_transaction trx( std::move( etrx ) );
      transaction_context trx_context( self, trx, trx.id(), std::move(trx_timer), start );

      if (auto dm_logger = get_deep_mind_logger(trx_context.is_transient())) {
         dm_logger->on_onerror(etrx);
      }

      trx_context.block_deadline = block_deadline;
      trx_context.max_transaction_time_subjective = max_transaction_time;
      trx_context.explicit_billed_cpu_time = explicit_billed_cpu_time;
      trx_context.billed_cpu_time_us = billed_cpu_time_us;
      trx_context.enforce_whiteblacklist = enforce_whiteblacklist;
      transaction_trace_ptr trace = trx_context.trace;

      auto handle_exception = [&](const auto& e)
      {
         cpu_time_to_bill_us = trx_context.update_billed_cpu_time( fc::time_point::now() );
         trace->error_code = controller::convert_exception_to_error_code( e );
         trace->except = e;
         trace->except_ptr = std::current_exception();
      };

      try {
         trx_context.init_for_implicit_trx();
         trx_context.published = gtrx.published;
         trx_context.execute_action( trx_context.schedule_action( trx.get_transaction().actions.back(), gtrx.sender, false, 0, 0 ), 0 );
         trx_context.finalize(); // Automatically rounds up network and CPU usage in trace and bills payers if successful

         auto restore = make_block_restore_point();
         trace->receipt = push_receipt( gtrx.trx_id, transaction_receipt::soft_fail,
                                        trx_context.billed_cpu_time_us, trace->net_usage );
         fc::move_append( std::get<building_block>(pending->_block_stage)._action_receipt_digests,
                          std::move(trx_context.executed_action_receipt_digests) );

         trx_context.squash();
         restore.cancel();
         return trace;
      } catch( const disallowed_transaction_extensions_bad_block_exception& ) {
         throw;
      } catch( const protocol_feature_bad_block_exception& ) {
         throw;
      } catch ( const std::bad_alloc& ) {
         throw;
      } catch ( const boost::interprocess::bad_alloc& ) {
         throw;
      } catch( const fc::exception& e ) {
         handle_exception(e);
      } catch ( const std::exception& e ) {
         auto wrapper = fc::std_exception_wrapper::from_current_exception(e);
         handle_exception(wrapper);
      }
      return trace;
   }

   int64_t remove_scheduled_transaction( const generated_transaction_object& gto ) {
      throw std::runtime_error("Deferred transaction implementation has been removed.  Please activate disable_deferred_trxs_stage_1!");
   }

   bool failure_is_subjective( const fc::exception& e ) const {
      auto code = e.code();
      return    (code == subjective_block_production_exception::code_value)
             || (code == block_net_usage_exceeded::code_value)
             || (code == greylist_net_usage_exceeded::code_value)
             || (code == block_cpu_usage_exceeded::code_value)
             || (code == greylist_cpu_usage_exceeded::code_value)
             || (code == deadline_exception::code_value)
             || (code == leeway_deadline_exception::code_value)
             || (code == actor_whitelist_exception::code_value)
             || (code == actor_blacklist_exception::code_value)
             || (code == contract_whitelist_exception::code_value)
             || (code == contract_blacklist_exception::code_value)
             || (code == action_blacklist_exception::code_value)
             || (code == key_blacklist_exception::code_value)
             || (code == sig_variable_size_limit_exception::code_value);
   }

   bool scheduled_failure_is_subjective( const fc::exception& e ) const {
      auto code = e.code();
      return    (code == tx_cpu_usage_exceeded::code_value)
             || failure_is_subjective(e);
   }

   transaction_trace_ptr push_scheduled_transaction( const transaction_id_type& trxid,
                                                     fc::time_point block_deadline, fc::microseconds max_transaction_time,
                                                     uint32_t billed_cpu_time_us, bool explicit_billed_cpu_time = false )
   {
      const auto& idx = db.get_index<generated_transaction_multi_index,by_trx_id>();
      auto itr = idx.find( trxid );
      SYS_ASSERT( itr != idx.end(), unknown_transaction_exception, "unknown transaction" );
      return push_scheduled_transaction( *itr, block_deadline, max_transaction_time, billed_cpu_time_us, explicit_billed_cpu_time );
   }

   transaction_trace_ptr push_scheduled_transaction( const generated_transaction_object& gto,
                                                     fc::time_point block_deadline, fc::microseconds max_transaction_time,
                                                     uint32_t billed_cpu_time_us, bool explicit_billed_cpu_time = false )
   { try {

      auto start = fc::time_point::now();
      const bool validating = !self.is_speculative_block();
      SYS_ASSERT( !validating || explicit_billed_cpu_time, transaction_exception, "validating requires explicit billing" );

      maybe_session undo_session;
      if ( !self.skip_db_sessions() )
         undo_session = maybe_session(db);

      auto gtrx = generated_transaction(gto);

      // remove the generated transaction object after making a copy
      // this will ensure that anything which affects the GTO multi-index-container will not invalidate
      // data we need to successfully retire this transaction.
      //
      // IF the transaction FAILs in a subjective way, `undo_session` should expire without being squashed
      // resulting in the GTO being restored and available for a future block to retire.
      int64_t trx_removal_ram_delta = remove_scheduled_transaction(gto);

      fc::datastream<const char*> ds( gtrx.packed_trx.data(), gtrx.packed_trx.size() );

      signed_transaction dtrx;
      fc::raw::unpack(ds,static_cast<transaction&>(dtrx) );
      transaction_metadata_ptr trx =
            transaction_metadata::create_no_recover_keys( std::make_shared<packed_transaction>( std::move(dtrx)  ),
                                                          transaction_metadata::trx_type::scheduled );
      trx->accepted = true;

      // After disable_deferred_trxs_stage_1 is activated, a deferred transaction
      // can only be retired as expired, and it can be retired as expired
      // regardless of whether its delay_util or expiration times have been reached.
      transaction_trace_ptr trace;
      if( self.is_builtin_activated( builtin_protocol_feature_t::disable_deferred_trxs_stage_1 ) || gtrx.expiration < self.pending_block_time() ) {
         trace = std::make_shared<transaction_trace>();
         trace->id = gtrx.trx_id;
         trace->block_num = self.head_block_num() + 1;
         trace->block_time = self.pending_block_time();
         trace->producer_block_id = self.pending_producer_block_id();
         trace->scheduled = true;
         trace->receipt = push_receipt( gtrx.trx_id, transaction_receipt::expired, billed_cpu_time_us, 0 ); // expire the transaction
         trace->account_ram_delta = account_delta( gtrx.sender, trx_removal_ram_delta );
         trace->elapsed = fc::time_point::now() - start;
         pending->_block_report.total_cpu_usage_us += billed_cpu_time_us;
         pending->_block_report.total_elapsed_time += trace->elapsed;
         pending->_block_report.total_time += trace->elapsed;
         emit( self.accepted_transaction, trx );
         dmlog_applied_transaction(trace);
         emit( self.applied_transaction, std::tie(trace, trx->packed_trx()) );
         undo_session.squash();
         return trace;
      }

      throw std::runtime_error("Deferred transaction implementation has been removed.  Please activate disable_deferred_trxs_stage_1!");
   } FC_CAPTURE_AND_RETHROW() } /// push_scheduled_transaction


   /**
    *  Adds the transaction receipt to the pending block and returns it.
    */
   template<typename T>
   const transaction_receipt& push_receipt( const T& trx, transaction_receipt_header::status_enum status,
                                            uint64_t cpu_usage_us, uint64_t net_usage ) {
      uint64_t net_usage_words = net_usage / 8;
      SYS_ASSERT( net_usage_words*8 == net_usage, transaction_exception, "net_usage is not divisible by 8" );
      auto& receipts = std::get<building_block>(pending->_block_stage)._pending_trx_receipts;
      receipts.emplace_back( trx );
      transaction_receipt& r = receipts.back();
      r.cpu_usage_us         = cpu_usage_us;
      r.net_usage_words      = net_usage_words;
      r.status               = status;
      auto& bb = std::get<building_block>(pending->_block_stage);
      if( std::holds_alternative<digests_t>(bb._trx_mroot_or_receipt_digests) ) {
         if( self.is_builtin_activated( builtin_protocol_feature_t::disable_compression_in_transaction_merkle ) )
            std::get<digests_t>(bb._trx_mroot_or_receipt_digests).emplace_back( r.digest() );
         else
            std::get<digests_t>(bb._trx_mroot_or_receipt_digests).emplace_back( r.packed_digest() );
      }
      return r;
   }

   /**
    *  This is the entry point for new transactions to the block state. It will check authorization and
    *  determine whether to execute it now or to delay it. Lastly it inserts a transaction receipt into
    *  the pending block.
    */
   transaction_trace_ptr push_transaction( const transaction_metadata_ptr& trx,
                                           fc::time_point block_deadline,
                                           fc::microseconds max_transaction_time,
                                           uint32_t billed_cpu_time_us,
                                           bool explicit_billed_cpu_time,
                                           int64_t subjective_cpu_bill_us )
   {
      SYS_ASSERT(block_deadline != fc::time_point(), transaction_exception, "deadline cannot be uninitialized");

      transaction_trace_ptr trace;
      try {
         auto start = fc::time_point::now();
         const bool check_auth = !self.skip_auth_check() && !trx->implicit() && !trx->is_read_only();
         const fc::microseconds sig_cpu_usage = trx->signature_cpu_usage();

         if( !explicit_billed_cpu_time ) {
            fc::microseconds already_consumed_time( SYS_PERCENT(sig_cpu_usage.count(), conf.sig_cpu_bill_pct) );

            if( start.time_since_epoch() <  already_consumed_time ) {
               start = fc::time_point();
            } else {
               start -= already_consumed_time;
            }
         }

         const signed_transaction& trn = trx->packed_trx()->get_signed_transaction();
         transaction_checktime_timer trx_timer(timer);
         transaction_context trx_context(self, *trx->packed_trx(), trx->id(), std::move(trx_timer), start, trx->get_trx_type());
         if ((bool)subjective_cpu_leeway && self.is_speculative_block()) {
            trx_context.leeway = *subjective_cpu_leeway;
         }
         trx_context.block_deadline = block_deadline;
         trx_context.max_transaction_time_subjective = max_transaction_time;
         trx_context.explicit_billed_cpu_time = explicit_billed_cpu_time;
         trx_context.billed_cpu_time_us = billed_cpu_time_us;
         trx_context.subjective_cpu_bill_us = subjective_cpu_bill_us;
         trace = trx_context.trace;

         auto handle_exception =[&](const auto& e)
         {
            trace->error_code = controller::convert_exception_to_error_code( e );
            trace->except = e;
            trace->except_ptr = std::current_exception();
            trace->elapsed = fc::time_point::now() - trx_context.start;
         };

         try {
            if( trx->implicit() ) {
               trx_context.init_for_implicit_trx();
               trx_context.enforce_whiteblacklist = false;
            } else {
               trx_context.init_for_input_trx( trx->packed_trx()->get_unprunable_size(),
                                               trx->packed_trx()->get_prunable_size() );
            }

            trx_context.delay = fc::seconds(trn.delay_sec);

            if( check_auth ) {
               authorization.check_authorization(
                       trn.actions,
                       trx->recovered_keys(),
                       {},
                       trx_context.delay,
                       [&trx_context](){ trx_context.checktime(); },
                       false,
                       trx->is_dry_run()
               );
            }
            trx_context.exec();
            trx_context.finalize(); // Automatically rounds up network and CPU usage in trace and bills payers if successful

            auto restore = make_block_restore_point( trx->is_read_only() );

            trx->billed_cpu_time_us = trx_context.billed_cpu_time_us;
            if (!trx->implicit() && !trx->is_read_only()) {
               transaction_receipt::status_enum s = (trx_context.delay == fc::seconds(0))
                                                    ? transaction_receipt::executed
                                                    : transaction_receipt::delayed;
               trace->receipt = push_receipt(*trx->packed_trx(), s, trx_context.billed_cpu_time_us, trace->net_usage);
               std::get<building_block>(pending->_block_stage)._pending_trx_metas.emplace_back(trx);
            } else {
               transaction_receipt_header r;
               r.status = transaction_receipt::executed;
               r.cpu_usage_us = trx_context.billed_cpu_time_us;
               r.net_usage_words = trace->net_usage / 8;
               trace->receipt = r;
            }

            if ( !trx->is_read_only() ) {
               fc::move_append( std::get<building_block>(pending->_block_stage)._action_receipt_digests,
                                std::move(trx_context.executed_action_receipt_digests) );
                if ( !trx->is_dry_run() ) {
                  // call the accept signal but only once for this transaction
                   if (!trx->accepted) {
                       trx->accepted = true;
                       emit(self.accepted_transaction, trx);
                   }

                   dmlog_applied_transaction(trace, &trn);
                   emit(self.applied_transaction, std::tie(trace, trx->packed_trx()));
                }
            }

            if ( trx->is_transient() ) {
               // remove trx from pending block by not canceling 'restore'
               trx_context.undo(); // this will happen automatically in destructor, but make it more explicit
            } else if ( read_mode != db_read_mode::SPECULATIVE && pending->_block_status == controller::block_status::ephemeral ) {
               // An ephemeral block will never become a full block, but on a producer node the trxs should be saved
               // in the un-applied transaction queue for execution during block production. For a non-producer node
               // save the trxs in the un-applied transaction queue for use during block validation to skip signature
               // recovery.
               restore.cancel();   // maintain trx metas for abort block
               trx_context.undo(); // this will happen automatically in destructor, but make it more explicit
            } else {
               restore.cancel();
               trx_context.squash();
            }

            if( !trx->is_transient() ) {
               pending->_block_report.total_net_usage += trace->net_usage;
               pending->_block_report.total_cpu_usage_us += trace->receipt->cpu_usage_us;
               pending->_block_report.total_elapsed_time += trace->elapsed;
               pending->_block_report.total_time += fc::time_point::now() - start;
            }

            return trace;
         } catch( const disallowed_transaction_extensions_bad_block_exception& ) {
            throw;
         } catch( const protocol_feature_bad_block_exception& ) {
            throw;
         } catch ( const std::bad_alloc& ) {
           throw;
         } catch ( const boost::interprocess::bad_alloc& ) {
           throw;
         } catch (const fc::exception& e) {
           handle_exception(e);
         } catch (const std::exception& e) {
           auto wrapper = fc::std_exception_wrapper::from_current_exception(e);
           handle_exception(wrapper);
         }

         if (!trx->is_transient()) {
            emit(self.accepted_transaction, trx);
            dmlog_applied_transaction(trace);
            emit(self.applied_transaction, std::tie(trace, trx->packed_trx()));

            pending->_block_report.total_net_usage += trace->net_usage;
            if( trace->receipt ) pending->_block_report.total_cpu_usage_us += trace->receipt->cpu_usage_us;
            pending->_block_report.total_elapsed_time += trace->elapsed;
            pending->_block_report.total_time += fc::time_point::now() - start;
         }

         return trace;
      } FC_CAPTURE_AND_RETHROW((trace))
   } /// push_transaction

   void start_block( block_timestamp_type when,
                     uint16_t confirm_block_count,
                     const vector<digest_type>& new_protocol_feature_activations,
                     controller::block_status s,
                     const std::optional<block_id_type>& producer_block_id,
                     const fc::time_point& deadline )
   {
      SYS_ASSERT( !pending, block_validate_exception, "pending block already exists" );

      emit( self.block_start, head->block_num + 1 );

      // at block level, no transaction specific logging is possible
      if (auto dm_logger = get_deep_mind_logger(false)) {
         // The head block represents the block just before this one that is about to start, so add 1 to get this block num
         dm_logger->on_start_block(head->block_num + 1);
      }

      auto guard_pending = fc::make_scoped_exit([this, head_block_num=head->block_num](){
         protocol_features.popped_blocks_to( head_block_num );
         pending.reset();
      });

      if (!self.skip_db_sessions(s)) {
         SYS_ASSERT( db.revision() == head->block_num, database_exception, "db revision is not on par with head block",
                     ("db.revision()", db.revision())("controller_head_block", head->block_num)("fork_db_head_block", fork_db.head()->block_num) );

         pending.emplace( maybe_session(db), *head, when, confirm_block_count, new_protocol_feature_activations );
      } else {
         pending.emplace( maybe_session(), *head, when, confirm_block_count, new_protocol_feature_activations );
      }

      pending->_block_status = s;
      pending->_producer_block_id = producer_block_id;

      auto& bb = std::get<building_block>(pending->_block_stage);
      const auto& pbhs = bb._pending_block_header_state;

      // block status is either ephemeral or incomplete. Modify state of speculative block only if we are building a
      // speculative incomplete block (otherwise we need clean state for head mode, ephemeral block)
      if ( pending->_block_status != controller::block_status::ephemeral )
      {
         const auto& pso = db.get<protocol_state_object>();

         auto num_preactivated_protocol_features = pso.preactivated_protocol_features.size();
         bool handled_all_preactivated_features = (num_preactivated_protocol_features == 0);

         if( new_protocol_feature_activations.size() > 0 ) {
            flat_map<digest_type, bool> activated_protocol_features;
            activated_protocol_features.reserve( std::max( num_preactivated_protocol_features,
                                                           new_protocol_feature_activations.size() ) );
            for( const auto& feature_digest : pso.preactivated_protocol_features ) {
               activated_protocol_features.emplace( feature_digest, false );
            }

            size_t num_preactivated_features_that_have_activated = 0;

            const auto& pfs = protocol_features.get_protocol_feature_set();
            for( const auto& feature_digest : new_protocol_feature_activations ) {
               const auto& f = pfs.get_protocol_feature( feature_digest );

               auto res = activated_protocol_features.emplace( feature_digest, true );
               if( res.second ) {
                  // feature_digest was not preactivated
                  SYS_ASSERT( !f.preactivation_required, protocol_feature_exception,
                              "attempted to activate protocol feature without prior required preactivation: ${digest}",
                              ("digest", feature_digest)
                  );
               } else {
                  SYS_ASSERT( !res.first->second, block_validate_exception,
                              "attempted duplicate activation within a single block: ${digest}",
                              ("digest", feature_digest)
                  );
                  // feature_digest was preactivated
                  res.first->second = true;
                  ++num_preactivated_features_that_have_activated;
               }

               if( f.builtin_feature ) {
                  trigger_activation_handler( *f.builtin_feature );
               }

               protocol_features.activate_feature( feature_digest, pbhs.block_num );

               ++bb._num_new_protocol_features_that_have_activated;
            }

            if( num_preactivated_features_that_have_activated == num_preactivated_protocol_features ) {
               handled_all_preactivated_features = true;
            }
         }

         SYS_ASSERT( handled_all_preactivated_features, block_validate_exception,
                     "There are pre-activated protocol features that were not activated at the start of this block"
         );

         if( new_protocol_feature_activations.size() > 0 ) {
            db.modify( pso, [&]( auto& ps ) {
               ps.preactivated_protocol_features.clear();

               ps.activated_protocol_features.reserve( ps.activated_protocol_features.size()
                                                         + new_protocol_feature_activations.size() );
               for( const auto& feature_digest : new_protocol_feature_activations ) {
                  ps.activated_protocol_features.emplace_back( feature_digest, pbhs.block_num );
               }
            });
         }

         const auto& gpo = self.get_global_properties();

         if( gpo.proposed_schedule_block_num && // if there is a proposed schedule that was proposed in a block ...
             ( *gpo.proposed_schedule_block_num <= pbhs.dpos_irreversible_blocknum ) && // ... that has now become irreversible ...
             pbhs.prev_pending_schedule.schedule.producers.size() == 0 // ... and there was room for a new pending schedule prior to any possible promotion
         )
         {
            // Promote proposed schedule to pending schedule.
            if( !replaying ) {
               ilog( "promoting proposed schedule (set in block ${proposed_num}) to pending; current block: ${n} lib: ${lib} schedule: ${schedule} ",
                     ("proposed_num", *gpo.proposed_schedule_block_num)("n", pbhs.block_num)
                     ("lib", pbhs.dpos_irreversible_blocknum)
                     ("schedule", producer_authority_schedule::from_shared(gpo.proposed_schedule) ) );
            }

            SYS_ASSERT( gpo.proposed_schedule.version == pbhs.active_schedule_version + 1,
                        producer_schedule_exception, "wrong producer schedule version specified" );

            std::get<building_block>(pending->_block_stage)._new_pending_producer_schedule = producer_authority_schedule::from_shared(gpo.proposed_schedule);
            db.modify( gpo, [&]( auto& gp ) {
               gp.proposed_schedule_block_num = std::optional<block_num_type>();
               gp.proposed_schedule.version=0;
               gp.proposed_schedule.producers.clear();
            });
         }

         try {
            transaction_metadata_ptr onbtrx =
                  transaction_metadata::create_no_recover_keys( std::make_shared<packed_transaction>( get_on_block_transaction() ),
                                                                transaction_metadata::trx_type::implicit );
            auto reset_in_trx_requiring_checks = fc::make_scoped_exit([old_value=in_trx_requiring_checks,this](){
                  in_trx_requiring_checks = old_value;
               });
            in_trx_requiring_checks = true;
            auto trace = push_transaction( onbtrx, fc::time_point::maximum(), fc::microseconds::maximum(),
                                           gpo.configuration.min_transaction_cpu_usage, true, 0 );
            if( trace->except ) {
               wlog("onblock ${block_num} is REJECTING: ${entire_trace}",("block_num", head->block_num + 1)("entire_trace", trace));
            }
         } catch( const std::bad_alloc& e ) {
            elog( "on block transaction failed due to a std::bad_alloc" );
            throw;
         } catch( const boost::interprocess::bad_alloc& e ) {
            elog( "on block transaction failed due to a bad allocation" );
            throw;
         } catch( const fc::exception& e ) {
            wlog( "on block transaction failed, but shouldn't impact block generation, system contract needs update" );
            edump((e.to_detail_string()));
         } catch( const std::exception& e ) {
            wlog( "on block transaction failed due to unexpected exception" );
            edump((e.what()));
         } catch( ... ) {
            elog( "on block transaction failed due to unknown exception" );
         }

         clear_expired_input_transactions(deadline);
         update_producers_authority();
      }

      guard_pending.cancel();
   } /// start_block

   void finalize_block()
   {
      SYS_ASSERT( pending, block_validate_exception, "it is not valid to finalize when there is no pending block");
      SYS_ASSERT( std::holds_alternative<building_block>(pending->_block_stage), block_validate_exception, "already called finalize_block");

      try {

      auto& pbhs = pending->get_pending_block_header_state();

      auto& bb = std::get<building_block>(pending->_block_stage);

      set_s_headers( pbhs.block_num );

      for (const auto& header : bb._s_header){
        ilog("s_header present in finalize block, adding to block header: ${s}", ("s", header.to_string()));
      }

      auto action_merkle_fut = post_async_task( thread_pool.get_executor(),
                                                [ids{std::move( bb._action_receipt_digests )}]() mutable {
                                                   return merkle( std::move( ids ) );
                                                } );
      const bool calc_trx_merkle = !std::holds_alternative<checksum256_type>(bb._trx_mroot_or_receipt_digests);
      std::future<checksum256_type> trx_merkle_fut;
      if( calc_trx_merkle ) {
         trx_merkle_fut = post_async_task( thread_pool.get_executor(),
                                           [ids{std::move( std::get<digests_t>(bb._trx_mroot_or_receipt_digests) )}]() mutable {
                                              return merkle( std::move( ids ) );
                                           } );
      }

      // Update resource limits:
      resource_limits.process_account_limit_updates();
      const auto& chain_config = self.get_global_properties().configuration;
      uint64_t CPU_TARGET = SYS_PERCENT(chain_config.max_block_cpu_usage, chain_config.target_block_cpu_usage_pct);
      resource_limits.set_block_parameters(
         { CPU_TARGET, chain_config.max_block_cpu_usage, config::block_cpu_usage_average_window_ms / config::block_interval_ms, config::maximum_elastic_resource_multiplier, {99, 100}, {1000, 999}},
         {SYS_PERCENT(chain_config.max_block_net_usage, chain_config.target_block_net_usage_pct), chain_config.max_block_net_usage, config::block_size_average_window_ms / config::block_interval_ms, config::maximum_elastic_resource_multiplier, {99, 100}, {1000, 999}}
      );
      resource_limits.process_block_usage(pbhs.block_num);

      // Create (unsigned) block:
      auto block_ptr = std::make_shared<signed_block>( pbhs.make_block_header(
         calc_trx_merkle ? trx_merkle_fut.get() : std::get<checksum256_type>(bb._trx_mroot_or_receipt_digests),
         action_merkle_fut.get(),
         bb._new_pending_producer_schedule,
         std::move( bb._new_protocol_feature_activations ),
         protocol_features.get_protocol_feature_set(),
         bb._s_header // Include optional s_headers in block header
      ) );

      block_ptr->transactions = std::move( bb._pending_trx_receipts );

      auto id = block_ptr->calculate_id();

      // Update TaPoS table:
      create_block_summary( id );

      /*
      ilog( "finalized block ${n} (${id}) at ${t} by ${p} (${signing_key}); schedule_version: ${v} lib: ${lib} #dtrxs: ${ndtrxs} ${np}",
            ("n",pbhs.block_num)
            ("id",id)
            ("t",pbhs.timestamp)
            ("p",pbhs.producer)
            ("signing_key", pbhs.block_signing_key)
            ("v",pbhs.active_schedule_version)
            ("lib",pbhs.dpos_irreversible_blocknum)
            ("ndtrxs",db.get_index<generated_transaction_multi_index,by_trx_id>().size())
            ("np",block_ptr->new_producers)
      );
      */

      pending->_block_stage = assembled_block{
                                 id,
                                 std::move( bb._pending_block_header_state ),
                                 std::move( bb._pending_trx_metas ),
                                 std::move( block_ptr ),
                                 std::move( bb._new_pending_producer_schedule )
                              };
   } FC_CAPTURE_AND_RETHROW() } /// finalize_block

   /**
    * @post regardless of the success of commit block there is no active pending block
    */
   void commit_block( controller::block_status s ) {
      auto reset_pending_on_exit = fc::make_scoped_exit([this]{
         pending.reset();
      });

      try {
         SYS_ASSERT( std::holds_alternative<completed_block>(pending->_block_stage), block_validate_exception,
                     "cannot call commit_block until pending block is completed" );

         const auto& bsp = std::get<completed_block>(pending->_block_stage)._block_state;

         if( s == controller::block_status::incomplete ) {
            fork_db.add( bsp );
            fork_db.mark_valid( bsp );
            emit( self.accepted_block_header, bsp );
            SYS_ASSERT( bsp == fork_db.head(), fork_database_exception, "committed block did not become the new head in fork database");
         } else if (s != controller::block_status::irreversible) {
            fork_db.mark_valid( bsp );
         }
         head = bsp;

         // at block level, no transaction specific logging is possible
         if (auto* dm_logger = get_deep_mind_logger(false)) {
            dm_logger->on_accepted_block(bsp);
         }

         emit( self.accepted_block, bsp );

         if( s == controller::block_status::incomplete ) {
            log_irreversible();
         }
      } catch (...) {
         // dont bother resetting pending, instead abort the block
         reset_pending_on_exit.cancel();
         abort_block();
         throw;
      }

      // push the state for pending.
      pending->push();
   }

   /**
    *  This method is called from other threads. The controller_impl should outlive those threads.
    *  However, to avoid race conditions, it means that the behavior of this function should not change
    *  after controller_impl construction.

    *  This should not be an issue since the purpose of this function is to ensure all of the protocol features
    *  in the supplied vector are recognized by the software, and the set of recognized protocol features is
    *  determined at startup and cannot be changed without a restart.
    */
   void check_protocol_features( block_timestamp_type timestamp,
                                 const flat_set<digest_type>& currently_activated_protocol_features,
                                 const vector<digest_type>& new_protocol_features )
   {
      const auto& pfs = protocol_features.get_protocol_feature_set();

      for( auto itr = new_protocol_features.begin(); itr != new_protocol_features.end(); ++itr ) {
         const auto& f = *itr;

         auto status = pfs.is_recognized( f, timestamp );
         switch( status ) {
            case protocol_feature_set::recognized_t::unrecognized:
               SYS_THROW( protocol_feature_exception,
                          "protocol feature with digest '${digest}' is unrecognized", ("digest", f) );
            break;
            case protocol_feature_set::recognized_t::disabled:
               SYS_THROW( protocol_feature_exception,
                          "protocol feature with digest '${digest}' is disabled", ("digest", f) );
            break;
            case protocol_feature_set::recognized_t::too_early:
               SYS_THROW( protocol_feature_exception,
                          "${timestamp} is too early for the earliest allowed activation time of the protocol feature with digest '${digest}'", ("digest", f)("timestamp", timestamp) );
            break;
            case protocol_feature_set::recognized_t::ready:
            break;
            default:
               SYS_THROW( protocol_feature_exception, "unexpected recognized_t status" );
            break;
         }

         SYS_ASSERT( currently_activated_protocol_features.find( f ) == currently_activated_protocol_features.end(),
                     protocol_feature_exception,
                     "protocol feature with digest '${digest}' has already been activated",
                     ("digest", f)
         );

         auto dependency_checker = [&currently_activated_protocol_features, &new_protocol_features, &itr]
                                   ( const digest_type& f ) -> bool
         {
            if( currently_activated_protocol_features.find( f ) != currently_activated_protocol_features.end() )
               return true;

            return (std::find( new_protocol_features.begin(), itr, f ) != itr);
         };

         SYS_ASSERT( pfs.validate_dependencies( f, dependency_checker ), protocol_feature_exception,
                     "not all dependencies of protocol feature with digest '${digest}' have been activated",
                     ("digest", f)
         );
      }
   }

   void report_block_header_diff( const block_header& b, const block_header& ab ) {

#define SYS_REPORT(DESC,A,B) \
   if( A != B ) { \
      elog("${desc}: ${bv} != ${abv}", ("desc", DESC)("bv", A)("abv", B)); \
   }

      SYS_REPORT( "timestamp", b.timestamp, ab.timestamp )
      SYS_REPORT( "producer", b.producer, ab.producer )
      SYS_REPORT( "confirmed", b.confirmed, ab.confirmed )
      SYS_REPORT( "previous", b.previous, ab.previous )
      SYS_REPORT( "transaction_mroot", b.transaction_mroot, ab.transaction_mroot )
      SYS_REPORT( "action_mroot", b.action_mroot, ab.action_mroot )
      SYS_REPORT( "schedule_version", b.schedule_version, ab.schedule_version )
      SYS_REPORT( "new_producers", b.new_producers, ab.new_producers )
      SYS_REPORT( "header_extensions", b.header_extensions, ab.header_extensions )

#undef SYS_REPORT
   }


   void apply_block( controller::block_report& br, const block_state_ptr& bsp, controller::block_status s,
                     const trx_meta_cache_lookup& trx_lookup )
   { try {
      try {
         auto start = fc::time_point::now();
         const signed_block_ptr& b = bsp->block;
         const auto& new_protocol_feature_activations = bsp->get_new_protocol_feature_activations();

         auto producer_block_id = bsp->id;
         start_block( b->timestamp, b->confirmed, new_protocol_feature_activations, s, producer_block_id, fc::time_point::maximum() );

         // validated in create_block_state_future()
         std::get<building_block>(pending->_block_stage)._trx_mroot_or_receipt_digests = b->transaction_mroot;

         const bool existing_trxs_metas = !bsp->trxs_metas().empty();
         const bool pub_keys_recovered = bsp->is_pub_keys_recovered();
         const bool skip_auth_checks = self.skip_auth_check();
         std::vector<std::tuple<transaction_metadata_ptr, recover_keys_future>> trx_metas;
         bool use_bsp_cached = false;
         if( pub_keys_recovered || (skip_auth_checks && existing_trxs_metas) ) {
            use_bsp_cached = true;
         } else {
            trx_metas.reserve( b->transactions.size() );
            for( const auto& receipt : b->transactions ) {
               if( std::holds_alternative<packed_transaction>(receipt.trx)) {
                  const auto& pt = std::get<packed_transaction>(receipt.trx);
                  transaction_metadata_ptr trx_meta_ptr = trx_lookup ? trx_lookup( pt.id() ) : transaction_metadata_ptr{};
                  if( trx_meta_ptr && *trx_meta_ptr->packed_trx() != pt ) trx_meta_ptr = nullptr;
                  if( trx_meta_ptr && ( skip_auth_checks || !trx_meta_ptr->recovered_keys().empty() ) ) {
                     trx_metas.emplace_back( std::move( trx_meta_ptr ), recover_keys_future{} );
                  } else if( skip_auth_checks ) {
                     packed_transaction_ptr ptrx( b, &pt ); // alias signed_block_ptr
                     trx_metas.emplace_back(
                           transaction_metadata::create_no_recover_keys( std::move(ptrx), transaction_metadata::trx_type::input ),
                           recover_keys_future{} );
                  } else {
                     packed_transaction_ptr ptrx( b, &pt ); // alias signed_block_ptr
                     auto fut = transaction_metadata::start_recover_keys(
                           std::move( ptrx ), thread_pool.get_executor(), chain_id, fc::microseconds::maximum(), transaction_metadata::trx_type::input  );
                     trx_metas.emplace_back( transaction_metadata_ptr{}, std::move( fut ) );
                  }
               }
            }
         }

         transaction_trace_ptr trace;

         size_t packed_idx = 0;
         const auto& trx_receipts = std::get<building_block>(pending->_block_stage)._pending_trx_receipts;
         for( const auto& receipt : b->transactions ) {
            auto num_pending_receipts = trx_receipts.size();
            if( std::holds_alternative<packed_transaction>(receipt.trx) ) {
               const auto& trx_meta = ( use_bsp_cached ? bsp->trxs_metas().at( packed_idx )
                                                       : ( !!std::get<0>( trx_metas.at( packed_idx ) ) ?
                                                             std::get<0>( trx_metas.at( packed_idx ) )
                                                             : std::get<1>( trx_metas.at( packed_idx ) ).get() ) );
               trace = push_transaction( trx_meta, fc::time_point::maximum(), fc::microseconds::maximum(), receipt.cpu_usage_us, true, 0 );
               ++packed_idx;
            } else if( std::holds_alternative<transaction_id_type>(receipt.trx) ) {
               trace = push_scheduled_transaction( std::get<transaction_id_type>(receipt.trx), fc::time_point::maximum(), fc::microseconds::maximum(), receipt.cpu_usage_us, true );
            } else {
               SYS_ASSERT( false, block_validate_exception, "encountered unexpected receipt type" );
            }

            bool transaction_failed =  trace && trace->except;
            bool transaction_can_fail = receipt.status == transaction_receipt_header::hard_fail && std::holds_alternative<transaction_id_type>(receipt.trx);
            if( transaction_failed && !transaction_can_fail) {
               edump((*trace));
               throw *trace->except;
            }

            SYS_ASSERT( trx_receipts.size() > 0,
                        block_validate_exception, "expected a receipt, block_num ${bn}, block_id ${id}, receipt ${e}",
                        ("bn", b->block_num())("id", producer_block_id)("e", receipt)
                      );
            SYS_ASSERT( trx_receipts.size() == num_pending_receipts + 1,
                        block_validate_exception, "expected receipt was not added, block_num ${bn}, block_id ${id}, receipt ${e}",
                        ("bn", b->block_num())("id", producer_block_id)("e", receipt)
                      );
            const transaction_receipt_header& r = trx_receipts.back();
            SYS_ASSERT( r == static_cast<const transaction_receipt_header&>(receipt),
                        block_validate_exception, "receipt does not match, ${lhs} != ${rhs}",
                        ("lhs", r)("rhs", static_cast<const transaction_receipt_header&>(receipt)) );
         }


         finalize_block();

         auto& ab = std::get<assembled_block>(pending->_block_stage);

         if( producer_block_id != ab._id ) {
            report_block_header_diff( *b, *ab._unsigned_block );
            // this implicitly asserts that all header fields (less the signature) are identical
            SYS_ASSERT( producer_block_id == ab._id, block_validate_exception, "Block ID does not match",
                        ("producer_block_id", producer_block_id)("validator_block_id", ab._id) );
         }
         if( are_multiple_state_roots_supported() ) {
            // New: Process the s_header from block header extensions
            auto rcvd_it = b->header_extensions.begin();
            const auto next_rcvd = [&itr=rcvd_it, end=b->header_extensions.end()](bool include_start) {
               if( !include_start ) ++itr;
               // find the first s_root_extension in the received block header extensions
               return std::find_if(itr, end,
                  [](const auto& ext) { return ext.first == s_root_extension::extension_id(); }); };
            auto crtd_it = ab._unsigned_block->header_extensions.begin();
            const auto next_crtd = [&itr=crtd_it, end=ab._unsigned_block->header_extensions.end()](bool include_start) {
               if( !include_start ) ++itr;
               // find the first s_root_extension in the locally constructed block header extensions
               // (which should be the same as the received block header extensions)
               return std::find_if(itr, end,
                  [](const auto& ext) { return ext.first == s_root_extension::extension_id(); }); };
            uint32_t count = 0;
            bool rcvd = true;
            bool crtd = true;
            bool include_start = true;
            while( rcvd ) {
               rcvd_it = next_rcvd(include_start);
               crtd_it = next_crtd(include_start);
               ++count;
               include_start = false; // only include start for the first iteration
               rcvd = rcvd_it != b->header_extensions.end();
               crtd = crtd_it != ab._unsigned_block->header_extensions.end();
               SYS_ASSERT( rcvd == crtd, block_validate_exception,
                           "The received block did${neg1} have $(count) root header extensions, but the locally constructed one did${neg2}",
                           ("neg1", (rcvd ? "" : " not"))("count", count)("neg2", (crtd ? "" : " not")) );
               if( rcvd && rcvd_it->second != crtd_it->second ) {
                  s_header rcvd_s_header = fc::raw::unpack<s_header>(rcvd_it->second);
                  s_header crtd_s_header = fc::raw::unpack<s_header>(crtd_it->second);
                  SYS_THROW( block_validate_exception,
                             "The received block root header extension, at slot number ${count}: ${rcvd}; and the locally "
                             "constructed one: ${crdt}; don't match!",
                             ("count", count)("rcvd", rcvd_s_header.to_string())("crdt", crtd_s_header.to_string()) );
               }
            }
         }

         if( !use_bsp_cached ) {
            bsp->set_trxs_metas( std::move( ab._trx_metas ), !skip_auth_checks );
         }
         // create completed_block with the existing block_state as we just verified it is the same as assembled_block
         pending->_block_stage = completed_block{ bsp };

         br = pending->_block_report; // copy before commit block destroys pending
         commit_block(s);
         br.total_time = fc::time_point::now() - start;
         return;
      } catch ( const std::bad_alloc& ) {
         throw;
      } catch ( const boost::interprocess::bad_alloc& ) {
         throw;
      } catch ( const fc::exception& e ) {
         edump((e.to_detail_string()));
         abort_block();
         throw;
      } catch ( const std::exception& e ) {
         edump((e.what()));
         abort_block();
         throw;
      }
   } FC_CAPTURE_AND_RETHROW() } /// apply_block


   // thread safe, expected to be called from thread other than the main thread
   block_state_ptr create_block_state_i( const block_id_type& id, const signed_block_ptr& b, const block_header_state& prev ) {
      const bool activated = self.is_builtin_activated( builtin_protocol_feature_t::disable_compression_in_transaction_merkle );
      auto trx_mroot = calculate_trx_merkle( b->transactions, activated );
      SYS_ASSERT( b->transaction_mroot == trx_mroot, block_validate_exception,
                  "invalid block transaction merkle root ${b} != ${c}", ("b", b->transaction_mroot)("c", trx_mroot) );

      const bool skip_validate_signee = false;
      auto bsp = std::make_shared<block_state>(
            prev,
            b,
            protocol_features.get_protocol_feature_set(),
            [this]( block_timestamp_type timestamp,
                    const flat_set<digest_type>& cur_features,
                    const vector<digest_type>& new_features )
            { check_protocol_features( timestamp, cur_features, new_features ); },
            skip_validate_signee
      );

      SYS_ASSERT( id == bsp->id, block_validate_exception,
                  "provided id ${id} does not match block id ${bid}", ("id", id)("bid", bsp->id) );
      return bsp;
   }

   std::future<block_state_ptr> create_block_state_future( const block_id_type& id, const signed_block_ptr& b ) {
      SYS_ASSERT( b, block_validate_exception, "null block" );

      return post_async_task( thread_pool.get_executor(), [b, id, control=this]() {
         // no reason for a block_state if fork_db already knows about block
         auto existing = control->fork_db.get_block( id );
         SYS_ASSERT( !existing, fork_database_exception, "we already know about this block: ${id}", ("id", id) );

         auto prev = control->fork_db.get_block_header( b->previous );
         SYS_ASSERT( prev, unlinkable_block_exception,
                     "unlinkable block ${id}", ("id", id)("previous", b->previous) );

         return control->create_block_state_i( id, b, *prev );
      } );
   }

   // thread safe, expected to be called from thread other than the main thread
   block_state_ptr create_block_state( const block_id_type& id, const signed_block_ptr& b ) {
      SYS_ASSERT( b, block_validate_exception, "null block" );

      // no reason for a block_state if fork_db already knows about block
      auto existing = fork_db.get_block( id );
      SYS_ASSERT( !existing, fork_database_exception, "we already know about this block: ${id}", ("id", id) );

      // previous not found could mean that previous block not applied yet
      auto prev = fork_db.get_block_header( b->previous );
      if( !prev ) return {};

      return create_block_state_i( id, b, *prev );
   }

   void push_block( controller::block_report& br,
                    const block_state_ptr& bsp,
                    const forked_branch_callback& forked_branch_cb,
                    const trx_meta_cache_lookup& trx_lookup )
   {
      controller::block_status s = controller::block_status::complete;
      SYS_ASSERT(!pending, block_validate_exception, "it is not valid to push a block when there is a pending block");

      auto reset_prod_light_validation = fc::make_scoped_exit([old_value=trusted_producer_light_validation, this]() {
         trusted_producer_light_validation = old_value;
      });
      try {
         SYS_ASSERT( bsp, block_validate_exception, "null block" );
         const auto& b = bsp->block;

         if( conf.terminate_at_block > 0 && conf.terminate_at_block <= self.head_block_num()) {
            ilog("Reached configured maximum block ${num}; terminating", ("num", conf.terminate_at_block) );
            shutdown();
            return;
         }

         emit( self.pre_accepted_block, b );

         fork_db.add( bsp );

         if (self.is_trusted_producer(b->producer)) {
            trusted_producer_light_validation = true;
         };

         emit( self.accepted_block_header, bsp );

         if( read_mode != db_read_mode::IRREVERSIBLE ) {
            maybe_switch_forks( br, fork_db.pending_head(), s, forked_branch_cb, trx_lookup );
         } else {
            log_irreversible();
         }

      } FC_LOG_AND_RETHROW( )
   }

   void replay_push_block( const signed_block_ptr& b, controller::block_status s ) {
      self.validate_db_available_size();

      SYS_ASSERT(!pending, block_validate_exception, "it is not valid to push a block when there is a pending block");

      try {
         SYS_ASSERT( b, block_validate_exception, "trying to push empty block" );
         SYS_ASSERT( (s == controller::block_status::irreversible || s == controller::block_status::validated),
                     block_validate_exception, "invalid block status for replay" );

         if( conf.terminate_at_block > 0 && conf.terminate_at_block <= self.head_block_num() ) {
            ilog("Reached configured maximum block ${num}; terminating", ("num", conf.terminate_at_block) );
            shutdown();
            return;
         }

         emit( self.pre_accepted_block, b );
         const bool skip_validate_signee = !conf.force_all_checks;

         auto bsp = std::make_shared<block_state>(
                        *head,
                        b,
                        protocol_features.get_protocol_feature_set(),
                        [this]( block_timestamp_type timestamp,
                                const flat_set<digest_type>& cur_features,
                                const vector<digest_type>& new_features )
                        { check_protocol_features( timestamp, cur_features, new_features ); },
                        skip_validate_signee
         );

         if( s != controller::block_status::irreversible ) {
            fork_db.add( bsp, true );
         }

         emit( self.accepted_block_header, bsp );

         controller::block_report br;
         if( s == controller::block_status::irreversible ) {
            apply_block( br, bsp, s, trx_meta_cache_lookup{} );

            // On replay, log_irreversible is not called and so no irreversible_block signal is emitted.
            // So emit it explicitly here.
            emit( self.irreversible_block, bsp );

            if (!self.skip_db_sessions(s)) {
               db.commit(bsp->block_num);
            }

         } else {
            SYS_ASSERT( read_mode != db_read_mode::IRREVERSIBLE, block_validate_exception,
                        "invariant failure: cannot replay reversible blocks while in irreversible mode" );
            maybe_switch_forks( br, bsp, s, forked_branch_callback{}, trx_meta_cache_lookup{} );
         }

      } FC_LOG_AND_RETHROW( )
   }

   void maybe_switch_forks( controller::block_report& br, const block_state_ptr& new_head, controller::block_status s,
                            const forked_branch_callback& forked_branch_cb, const trx_meta_cache_lookup& trx_lookup )
   {
      bool head_changed = true;
      if( new_head->header.previous == head->id ) {
         try {
            apply_block( br, new_head, s, trx_lookup );
         } catch ( const std::exception& e ) {
            fork_db.remove( new_head->id );
            throw;
         }
      } else if( new_head->id != head->id ) {
         auto old_head = head;
         ilog("switching forks from ${current_head_id} (block number ${current_head_num}) to ${new_head_id} (block number ${new_head_num})",
              ("current_head_id", head->id)("current_head_num", head->block_num)("new_head_id", new_head->id)("new_head_num", new_head->block_num) );

         // not possible to log transaction specific infor when switching forks
         if (auto dm_logger = get_deep_mind_logger(false)) {
            dm_logger->on_switch_forks(head->id, new_head->id);
         }

         auto branches = fork_db.fetch_branch_from( new_head->id, head->id );

         if( branches.second.size() > 0 ) {
            for( auto itr = branches.second.begin(); itr != branches.second.end(); ++itr ) {
               pop_block();
            }
            SYS_ASSERT( self.head_block_id() == branches.second.back()->header.previous, fork_database_exception,
                     "loss of sync between fork_db and chainbase during fork switch" ); // _should_ never fail

            if( forked_branch_cb ) forked_branch_cb( branches.second );
         }

         for( auto ritr = branches.first.rbegin(); ritr != branches.first.rend(); ++ritr ) {
            auto except = std::exception_ptr{};
            try {
               br = controller::block_report{};
               apply_block( br, *ritr, (*ritr)->is_valid() ? controller::block_status::validated
                                                           : controller::block_status::complete, trx_lookup );
            } catch ( const std::bad_alloc& ) {
              throw;
            } catch ( const boost::interprocess::bad_alloc& ) {
              throw;
            } catch (const fc::exception& e) {
               elog("exception thrown while switching forks ${e}", ("e", e.to_detail_string()));
               except = std::current_exception();
            } catch (const std::exception& e) {
               elog("exception thrown while switching forks ${e}", ("e", e.what()));
               except = std::current_exception();
            }

            if( except ) {
               // ritr currently points to the block that threw
               // Remove the block that threw and all forks built off it.
               fork_db.remove( (*ritr)->id );

               // pop all blocks from the bad fork, discarding their transactions
               // ritr base is a forward itr to the last block successfully applied
               auto applied_itr = ritr.base();
               for( auto itr = applied_itr; itr != branches.first.end(); ++itr ) {
                  pop_block();
               }
               SYS_ASSERT( self.head_block_id() == branches.second.back()->header.previous, fork_database_exception,
                           "loss of sync between fork_db and chainbase during fork switch reversal" ); // _should_ never fail

               // re-apply good blocks
               for( auto ritr = branches.second.rbegin(); ritr != branches.second.rend(); ++ritr ) {
                  br = controller::block_report{};
                  apply_block( br, *ritr, controller::block_status::validated /* we previously validated these blocks*/, trx_lookup );
               }
               std::rethrow_exception(except);
            } // end if exception
         } /// end for each block in branch

         if (fc::logger::get(DEFAULT_LOGGER).is_enabled(fc::log_level::info)) {
            auto get_ids = [&](auto& container)->std::string {
               std::string ids;
               for(auto ritr = container.rbegin(), e = container.rend(); ritr != e; ++ritr) {
                  ids += std::to_string((*ritr)->block_num) + ":" + (*ritr)->id.str() + ",";
               }
               if (!ids.empty()) ids.resize(ids.size()-1);
               return ids;
            };
            ilog("successfully switched fork to new head ${new_head_id}, removed {${rm_ids}}, applied {${new_ids}}",
                ("new_head_id", new_head->id)("rm_ids", get_ids(branches.second))("new_ids", get_ids(branches.first)));
         }
      } else {
         head_changed = false;
      }

      if( head_changed )
         log_irreversible();

   } /// push_block

   deque<transaction_metadata_ptr> abort_block() {
      deque<transaction_metadata_ptr> applied_trxs;
      if( pending ) {
         applied_trxs = pending->extract_trx_metas();
         pending.reset();
         protocol_features.popped_blocks_to( head->block_num );
      }
      return applied_trxs;
   }

   const checksum256_type calculate_trx_merkle( const deque<transaction_receipt>& trxs, bool disable_compression_in_transaction_merkle )const {
      deque<digest_type> trx_digests;
      if (disable_compression_in_transaction_merkle) {
         for( const auto& a : trxs ) {
            trx_digests.emplace_back( (a.digest)() );
         }
      }
      else {
         for( const auto& a : trxs ) {
            trx_digests.emplace_back( (a.packed_digest)() );
         }
      }

      return merkle( std::move(trx_digests) );
   }

   void update_producers_authority() {
      const auto& producers = pending->get_pending_block_header_state().active_schedule.producers;

      auto update_permission = [&]( auto& permission, auto threshold ) {
         auto auth = authority( threshold, {}, {});
         for( auto& p : producers ) {
            auth.accounts.push_back({{p.producer_name, config::active_name}, 1});
         }

         if( permission.auth != auth ) {
            db.modify(permission, [&]( auto& po ) {
               po.auth = auth;
            });
         }
      };

      uint32_t num_producers = producers.size();
      auto calculate_threshold = [=]( uint32_t numerator, uint32_t denominator ) {
         return ( (num_producers * numerator) / denominator ) + 1;
      };

      update_permission( authorization.get_permission({config::producers_account_name,
                                                       config::active_name}),
                         calculate_threshold( 2, 3 ) /* more than two-thirds */                      );

      update_permission( authorization.get_permission({config::producers_account_name,
                                                       config::majority_producers_permission_name}),
                         calculate_threshold( 1, 2 ) /* more than one-half */                        );

      update_permission( authorization.get_permission({config::producers_account_name,
                                                       config::minority_producers_permission_name}),
                         calculate_threshold( 1, 3 ) /* more than one-third */                       );

      //TODO: Add tests
   }

   void create_block_summary(const block_id_type& id) {
      auto block_num = block_header::num_from_id(id);
      auto sid = block_num & 0xffff;
      db.modify( db.get<block_summary_object,by_id>(sid), [&](block_summary_object& bso ) {
          bso.block_id = id;
      });
   }


   void clear_expired_input_transactions(const fc::time_point& deadline) {
      //Look for expired transactions in the deduplication list, and remove them.
      auto& transaction_idx = db.get_mutable_index<transaction_multi_index>();
      const auto& dedupe_index = transaction_idx.indices().get<by_expiration>();
      auto now = self.is_building_block() ? self.pending_block_time() : self.head_block_time();
      const auto total = dedupe_index.size();
      uint32_t num_removed = 0;
      while( (!dedupe_index.empty()) && ( now > dedupe_index.begin()->expiration.to_time_point() ) ) {
         transaction_idx.remove(*dedupe_index.begin());
         ++num_removed;
         if( deadline <= fc::time_point::now() ) {
            break;
         }
      }
      dlog("removed ${n} expired transactions of the ${t} input dedup list, pending block time ${pt}",
           ("n", num_removed)("t", total)("pt", now));
   }

   bool sender_avoids_whitelist_blacklist_enforcement( account_name sender )const {
      if( conf.sender_bypass_whiteblacklist.size() > 0 &&
          ( conf.sender_bypass_whiteblacklist.find( sender ) != conf.sender_bypass_whiteblacklist.end() ) )
      {
         return true;
      }

      return false;
   }

   void check_actor_list( const flat_set<account_name>& actors )const {
      if( actors.size() == 0 ) return;

      if( conf.actor_whitelist.size() > 0 ) {
         // throw if actors is not a subset of whitelist
         const auto& whitelist = conf.actor_whitelist;
         bool is_subset = true;

         // quick extents check, then brute force the check actors
         if (*actors.cbegin() >= *whitelist.cbegin() && *actors.crbegin() <= *whitelist.crbegin() ) {
            auto lower_bound = whitelist.cbegin();
            for (const auto& actor: actors) {
               lower_bound = std::lower_bound(lower_bound, whitelist.cend(), actor);

               // if the actor is not found, this is not a subset
               if (lower_bound == whitelist.cend() || *lower_bound != actor ) {
                  is_subset = false;
                  break;
               }

               // if the actor was found, we are guaranteed that other actors are either not present in the whitelist
               // or will be present in the range defined as [next actor,end)
               lower_bound = std::next(lower_bound);
            }
         } else {
            is_subset = false;
         }

         // helper lambda to lazily calculate the actors for error messaging
         static auto generate_missing_actors = [](const flat_set<account_name>& actors, const flat_set<account_name>& whitelist) -> vector<account_name> {
            vector<account_name> excluded;
            excluded.reserve( actors.size() );
            set_difference( actors.begin(), actors.end(),
                            whitelist.begin(), whitelist.end(),
                            std::back_inserter(excluded) );
            return excluded;
         };

         SYS_ASSERT( is_subset,  actor_whitelist_exception,
                     "authorizing actor(s) in transaction are not on the actor whitelist: ${actors}",
                     ("actors", generate_missing_actors(actors, whitelist))
                   );
      } else if( conf.actor_blacklist.size() > 0 ) {
         // throw if actors intersects blacklist
         const auto& blacklist = conf.actor_blacklist;
         bool intersects = false;

         // quick extents check then brute force check actors
         if( *actors.cbegin() <= *blacklist.crbegin() && *actors.crbegin() >= *blacklist.cbegin() ) {
            auto lower_bound = blacklist.cbegin();
            for (const auto& actor: actors) {
               lower_bound = std::lower_bound(lower_bound, blacklist.cend(), actor);

               // if the lower bound in the blacklist is at the end, all other actors are guaranteed to
               // not exist in the blacklist
               if (lower_bound == blacklist.cend()) {
                  break;
               }

               // if the lower bound of an actor IS the actor, then we have an intersection
               if (*lower_bound == actor) {
                  intersects = true;
                  break;
               }
            }
         }

         // helper lambda to lazily calculate the actors for error messaging
         static auto generate_blacklisted_actors = [](const flat_set<account_name>& actors, const flat_set<account_name>& blacklist) -> vector<account_name> {
            vector<account_name> blacklisted;
            blacklisted.reserve( actors.size() );
            set_intersection( actors.begin(), actors.end(),
                              blacklist.begin(), blacklist.end(),
                              std::back_inserter(blacklisted)
                            );
            return blacklisted;
         };

         SYS_ASSERT( !intersects, actor_blacklist_exception,
                     "authorizing actor(s) in transaction are on the actor blacklist: ${actors}",
                     ("actors", generate_blacklisted_actors(actors, blacklist))
                   );
      }
   }

   void check_contract_list( account_name code )const {
      if( conf.contract_whitelist.size() > 0 ) {
         SYS_ASSERT( conf.contract_whitelist.find( code ) != conf.contract_whitelist.end(),
                     contract_whitelist_exception,
                     "account '${code}' is not on the contract whitelist", ("code", code)
                   );
      } else if( conf.contract_blacklist.size() > 0 ) {
         SYS_ASSERT( conf.contract_blacklist.find( code ) == conf.contract_blacklist.end(),
                     contract_blacklist_exception,
                     "account '${code}' is on the contract blacklist", ("code", code)
                   );
      }
   }

   void check_action_list( account_name code, action_name action )const {
      if( conf.action_blacklist.size() > 0 ) {
         SYS_ASSERT( conf.action_blacklist.find( std::make_pair(code, action) ) == conf.action_blacklist.end(),
                     action_blacklist_exception,
                     "action '${code}::${action}' is on the action blacklist",
                     ("code", code)("action", action)
                   );
      }
   }

   void check_key_list( const public_key_type& key )const {
      if( conf.key_blacklist.size() > 0 ) {
         SYS_ASSERT( conf.key_blacklist.find( key ) == conf.key_blacklist.end(),
                     key_blacklist_exception,
                     "public key '${key}' is on the key blacklist",
                     ("key", key)
                   );
      }
   }

   /*
   bool should_check_tapos()const { return true; }

   void validate_tapos( const transaction& trx )const {
      if( !should_check_tapos() ) return;

      const auto& tapos_block_summary = db.get<block_summary_object>((uint16_t)trx.ref_block_num);

      //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
      SYS_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), invalid_ref_block_exception,
                 "Transaction's reference block did not match. Is this transaction from a different fork?",
                 ("tapos_summary", tapos_block_summary));
   }
   */


   /**
    *  At the start of each block we notify the system contract with a transaction that passes in
    *  the block header of the prior block (which is currently our head block)
    */
   signed_transaction get_on_block_transaction()
   {
      action on_block_act;
      on_block_act.account = config::system_account_name;
      on_block_act.name = "onblock"_n;
      on_block_act.authorization = vector<permission_level>{{config::system_account_name, config::active_name}};
      on_block_act.data = fc::raw::pack(self.head_block_header());

      signed_transaction trx;
      trx.actions.emplace_back(std::move(on_block_act));
      if( self.is_builtin_activated( builtin_protocol_feature_t::no_duplicate_deferred_id ) ) {
         trx.expiration = time_point_sec();
         trx.ref_block_num = 0;
         trx.ref_block_prefix = 0;
      } else {
         trx.expiration = time_point_sec{self.pending_block_time() + fc::microseconds(999'999)}; // Round up to nearest second to avoid appearing expired
         trx.set_reference_block( self.head_block_id() );
      }

      return trx;
   }

   inline deep_mind_handler* get_deep_mind_logger(bool is_trx_transient) const {
      // do not perform deep mind logging for read-only and dry-run transactions
      return is_trx_transient ? nullptr : deep_mind_logger;
   }

   uint32_t earliest_available_block_num() const {
      return (blog.first_block_num() != 0) ? blog.first_block_num() : fork_db.root()->block_num;
   }

   void set_to_write_window() {
      app_window = app_window_type::write;
   }
   void set_to_read_window() {
      app_window = app_window_type::read;
   }
   bool is_write_window() const {
      return app_window == app_window_type::write;
   }

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   bool is_sys_vm_oc_enabled() const {
      return wasmif.is_sys_vm_oc_enabled();
   }
#endif

   // Only called from read-only trx execution threads when producer_plugin
   // starts them. Only OC requires initialize thread specific data.
   void init_thread_local_data() {
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
      if ( is_sys_vm_oc_enabled() ) {
         wasmif.init_thread_local_data();
      }
#endif
   }

   wasm_interface& get_wasm_interface() {
      return wasmif;
   }

   void code_block_num_last_used(const digest_type& code_hash, uint8_t vm_type, uint8_t vm_version, uint32_t block_num) {
      wasmif.code_block_num_last_used(code_hash, vm_type, vm_version, block_num);
   }

   block_state_ptr fork_db_head() const;
}; /// controller_impl

thread_local platform_timer controller_impl::timer;
#if defined(SYSIO_SYS_VM_RUNTIME_ENABLED) || defined(SYSIO_SYS_VM_JIT_RUNTIME_ENABLED)
thread_local sysio::vm::wasm_allocator controller_impl::wasm_alloc;
#endif

const resource_limits_manager&   controller::get_resource_limits_manager()const
{
   return my->resource_limits;
}
resource_limits_manager&         controller::get_mutable_resource_limits_manager()
{
   return my->resource_limits;
}

const authorization_manager&   controller::get_authorization_manager()const
{
   return my->authorization;
}
authorization_manager&         controller::get_mutable_authorization_manager()
{
   return my->authorization;
}

const protocol_feature_manager& controller::get_protocol_feature_manager()const
{
   return my->protocol_features;
}

const subjective_billing& controller::get_subjective_billing()const {
   return my->subjective_bill;
}

subjective_billing& controller::get_mutable_subjective_billing() {
   return my->subjective_bill;
}


controller::controller( const controller::config& cfg, const chain_id_type& chain_id )
:my( new controller_impl( cfg, *this, protocol_feature_set{}, chain_id ) )
{
}

controller::controller( const config& cfg, protocol_feature_set&& pfs, const chain_id_type& chain_id )
:my( new controller_impl( cfg, *this, std::move(pfs), chain_id ) )
{
}

controller::~controller() {
   my->abort_block();
   /* Shouldn't be needed anymore.
   //close fork_db here, because it can generate "irreversible" signal to this controller,
   //in case if read-mode == IRREVERSIBLE, we will apply latest irreversible block
   //for that we need 'my' to be valid pointer pointing to valid controller_impl.
   my->fork_db.close();
   */
}

void controller::add_indices() {
   my->add_indices();
}

void controller::startup( std::function<void()> shutdown, std::function<bool()> check_shutdown, const snapshot_reader_ptr& snapshot ) {
   my->startup(shutdown, check_shutdown, snapshot);
}

void controller::startup( std::function<void()> shutdown, std::function<bool()> check_shutdown, const genesis_state& genesis ) {
   my->startup(shutdown, check_shutdown, genesis);
}

void controller::startup(std::function<void()> shutdown, std::function<bool()> check_shutdown) {
   my->startup(shutdown, check_shutdown);
}

const chainbase::database& controller::db()const { return my->db; }

chainbase::database& controller::mutable_db()const { return my->db; }

const fork_database& controller::fork_db()const { return my->fork_db; }

void controller::preactivate_feature( const digest_type& feature_digest, bool is_trx_transient ) {
   const auto& pfs = my->protocol_features.get_protocol_feature_set();
   auto cur_time = pending_block_time();

   auto status = pfs.is_recognized( feature_digest, cur_time );
   switch( status ) {
      case protocol_feature_set::recognized_t::unrecognized:
         if( is_speculative_block() ) {
            SYS_THROW( subjective_block_production_exception,
                       "protocol feature with digest '${digest}' is unrecognized", ("digest", feature_digest) );
         } else {
            SYS_THROW( protocol_feature_bad_block_exception,
                       "protocol feature with digest '${digest}' is unrecognized", ("digest", feature_digest) );
         }
      break;
      case protocol_feature_set::recognized_t::disabled:
         if( is_speculative_block() ) {
            SYS_THROW( subjective_block_production_exception,
                       "protocol feature with digest '${digest}' is disabled", ("digest", feature_digest) );
         } else {
            SYS_THROW( protocol_feature_bad_block_exception,
                       "protocol feature with digest '${digest}' is disabled", ("digest", feature_digest) );
         }
      break;
      case protocol_feature_set::recognized_t::too_early:
         if( is_speculative_block() ) {
            SYS_THROW( subjective_block_production_exception,
                       "${timestamp} is too early for the earliest allowed activation time of the protocol feature with digest '${digest}'", ("digest", feature_digest)("timestamp", cur_time) );
         } else {
            SYS_THROW( protocol_feature_bad_block_exception,
                       "${timestamp} is too early for the earliest allowed activation time of the protocol feature with digest '${digest}'", ("digest", feature_digest)("timestamp", cur_time) );
         }
      break;
      case protocol_feature_set::recognized_t::ready:
      break;
      default:
         if( is_speculative_block() ) {
            SYS_THROW( subjective_block_production_exception, "unexpected recognized_t status" );
         } else {
            SYS_THROW( protocol_feature_bad_block_exception, "unexpected recognized_t status" );
         }
      break;
   }

   // The above failures depend on subjective information.
   // Because of deferred transactions, this complicates things considerably.

   // If producing a block, we throw a subjective failure if the feature is not properly recognized in order
   // to try to avoid retiring into a block a deferred transacton driven by subjective information.

   // But it is still possible for a producer to retire a deferred transaction that deals with this subjective
   // information. If they recognized the feature, they would retire it successfully, but a validator that
   // does not recognize the feature should reject the entire block (not just fail the deferred transaction).
   // Even if they don't recognize the feature, the producer could change their nodeop code to treat it like an
   // objective failure thus leading the deferred transaction to retire with soft_fail or hard_fail.
   // In this case, validators that don't recognize the feature would reject the whole block immediately, and
   // validators that do recognize the feature would likely lead to a different retire status which would
   // ultimately cause a validation failure and thus rejection of the block.
   // In either case, it results in rejection of the block which is the desired behavior in this scenario.

   // If the feature is properly recognized by producer and validator, we have dealt with the subjectivity and
   // now only consider the remaining failure modes which are deterministic and objective.
   // Thus the exceptions that can be thrown below can be regular objective exceptions
   // that do not cause immediate rejection of the block.

   SYS_ASSERT( !is_protocol_feature_activated( feature_digest ),
               protocol_feature_exception,
               "protocol feature with digest '${digest}' is already activated",
               ("digest", feature_digest)
   );

   const auto& pso = my->db.get<protocol_state_object>();

   SYS_ASSERT( std::find( pso.preactivated_protocol_features.begin(),
                          pso.preactivated_protocol_features.end(),
                          feature_digest
               ) == pso.preactivated_protocol_features.end(),
               protocol_feature_exception,
               "protocol feature with digest '${digest}' is already pre-activated",
               ("digest", feature_digest)
   );

   auto dependency_checker = [&]( const digest_type& d ) -> bool
   {
      if( is_protocol_feature_activated( d ) ) return true;

      return ( std::find( pso.preactivated_protocol_features.begin(),
                          pso.preactivated_protocol_features.end(),
                          d ) != pso.preactivated_protocol_features.end() );
   };

   SYS_ASSERT( pfs.validate_dependencies( feature_digest, dependency_checker ),
               protocol_feature_exception,
               "not all dependencies of protocol feature with digest '${digest}' have been activated or pre-activated",
               ("digest", feature_digest)
   );

   if (auto dm_logger = get_deep_mind_logger(is_trx_transient)) {
      const auto feature = pfs.get_protocol_feature(feature_digest);

      dm_logger->on_preactivate_feature(feature);
   }

   my->db.modify( pso, [&]( auto& ps ) {
      ps.preactivated_protocol_features.push_back( feature_digest );
   } );
}

vector<digest_type> controller::get_preactivated_protocol_features()const {
   const auto& pso = my->db.get<protocol_state_object>();

   if( pso.preactivated_protocol_features.size() == 0 ) return {};

   vector<digest_type> preactivated_protocol_features;

   for( const auto& f : pso.preactivated_protocol_features ) {
      preactivated_protocol_features.emplace_back( f );
   }

   return preactivated_protocol_features;
}

void controller::validate_protocol_features( const vector<digest_type>& features_to_activate )const {
   my->check_protocol_features( my->head->header.timestamp,
                                my->head->activated_protocol_features->protocol_features,
                                features_to_activate );
}

void controller::start_block( block_timestamp_type when,
                              uint16_t confirm_block_count,
                              const vector<digest_type>& new_protocol_feature_activations,
                              block_status bs,
                              const fc::time_point& deadline )
{
   validate_db_available_size();

   if( new_protocol_feature_activations.size() > 0 ) {
      validate_protocol_features( new_protocol_feature_activations );
   }

   SYS_ASSERT( bs == block_status::incomplete || bs == block_status::ephemeral, block_validate_exception, "speculative block type required" );

   my->start_block( when, confirm_block_count, new_protocol_feature_activations,
                    bs, std::optional<block_id_type>(), deadline );
}

block_state_ptr controller::finalize_block( block_report& br, const signer_callback_type& signer_callback ) {
   validate_db_available_size();

   my->finalize_block();

   auto& ab = std::get<assembled_block>(my->pending->_block_stage);

   auto bsp = std::make_shared<block_state>(
                  std::move( ab._pending_block_header_state ),
                  std::move( ab._unsigned_block ),
                  std::move( ab._trx_metas ),
                  my->protocol_features.get_protocol_feature_set(),
                  []( block_timestamp_type timestamp,
                      const flat_set<digest_type>& cur_features,
                      const vector<digest_type>& new_features )
                  {},
                  signer_callback
              );

   my->pending->_block_stage = completed_block{ bsp };

   br = my->pending->_block_report;

   return bsp;
}

void controller::commit_block() {
   validate_db_available_size();
   my->commit_block(block_status::incomplete);
}

deque<transaction_metadata_ptr> controller::abort_block() {
   return my->abort_block();
}

boost::asio::io_context& controller::get_thread_pool() {
   return my->thread_pool.get_executor();
}

std::future<block_state_ptr> controller::create_block_state_future( const block_id_type& id, const signed_block_ptr& b ) {
   return my->create_block_state_future( id, b );
}

block_state_ptr controller::create_block_state( const block_id_type& id, const signed_block_ptr& b ) const {
   return my->create_block_state( id, b );
}

void controller::push_block( controller::block_report& br,
                             const block_state_ptr& bsp,
                             const forked_branch_callback& forked_branch_cb,
                             const trx_meta_cache_lookup& trx_lookup )
{
   validate_db_available_size();
   my->push_block( br, bsp, forked_branch_cb, trx_lookup );
}

transaction_trace_ptr controller::push_transaction( const transaction_metadata_ptr& trx,
                                                    fc::time_point block_deadline, fc::microseconds max_transaction_time,
                                                    uint32_t billed_cpu_time_us, bool explicit_billed_cpu_time,
                                                    int64_t subjective_cpu_bill_us ) {
   validate_db_available_size();
   SYS_ASSERT( get_read_mode() != db_read_mode::IRREVERSIBLE, transaction_type_exception, "push transaction not allowed in irreversible mode" );
   SYS_ASSERT( trx && !trx->implicit() && !trx->scheduled(), transaction_type_exception, "Implicit/Scheduled transaction not allowed" );
   return my->push_transaction(trx, block_deadline, max_transaction_time, billed_cpu_time_us, explicit_billed_cpu_time, subjective_cpu_bill_us );
}

transaction_trace_ptr controller::push_scheduled_transaction( const transaction_id_type& trxid,
                                                              fc::time_point block_deadline, fc::microseconds max_transaction_time,
                                                              uint32_t billed_cpu_time_us, bool explicit_billed_cpu_time )
{
   SYS_ASSERT( get_read_mode() != db_read_mode::IRREVERSIBLE, transaction_type_exception, "push scheduled transaction not allowed in irreversible mode" );
   validate_db_available_size();
   return my->push_scheduled_transaction( trxid, block_deadline, max_transaction_time, billed_cpu_time_us, explicit_billed_cpu_time );
}

const flat_set<account_name>& controller::get_actor_whitelist() const {
   return my->conf.actor_whitelist;
}
const flat_set<account_name>& controller::get_actor_blacklist() const {
   return my->conf.actor_blacklist;
}
const flat_set<account_name>& controller::get_contract_whitelist() const {
   return my->conf.contract_whitelist;
}
const flat_set<account_name>& controller::get_contract_blacklist() const {
   return my->conf.contract_blacklist;
}
const flat_set< pair<account_name, action_name> >& controller::get_action_blacklist() const {
   return my->conf.action_blacklist;
}
const flat_set<public_key_type>& controller::get_key_blacklist() const {
   return my->conf.key_blacklist;
}

void controller::set_actor_whitelist( const flat_set<account_name>& new_actor_whitelist ) {
   my->conf.actor_whitelist = new_actor_whitelist;
}
void controller::set_actor_blacklist( const flat_set<account_name>& new_actor_blacklist ) {
   my->conf.actor_blacklist = new_actor_blacklist;
}
void controller::set_contract_whitelist( const flat_set<account_name>& new_contract_whitelist ) {
   my->conf.contract_whitelist = new_contract_whitelist;
}
void controller::set_contract_blacklist( const flat_set<account_name>& new_contract_blacklist ) {
   my->conf.contract_blacklist = new_contract_blacklist;
}
void controller::set_action_blacklist( const flat_set< pair<account_name, action_name> >& new_action_blacklist ) {
   for (auto& act: new_action_blacklist) {
      SYS_ASSERT(act.first != account_name(), name_type_exception, "Action blacklist - contract name should not be empty");
      SYS_ASSERT(act.second != action_name(), action_type_exception, "Action blacklist - action name should not be empty");
   }
   my->conf.action_blacklist = new_action_blacklist;
}
void controller::set_key_blacklist( const flat_set<public_key_type>& new_key_blacklist ) {
   my->conf.key_blacklist = new_key_blacklist;
}

bool controller_impl::are_multiple_state_roots_supported() const {
   if ( merkle_processor && self.is_builtin_activated( builtin_protocol_feature_t::multiple_state_roots_supported ) ) {
      return true;
   }

   return false;
}

void controller_impl::set_s_headers( uint32_t block_num ) {
   if( !are_multiple_state_roots_supported() ) {
      return; // No need to set s_headers if multiple state roots are not supported
   }
   SYS_ASSERT( pending, block_validate_exception, "it is not valid to set_s_headers when there is no pending block");
   SYS_ASSERT( std::holds_alternative<building_block>(pending->_block_stage), block_validate_exception, "already called finalize_block");
   // Try to set it first in the pending block
   auto& bb = std::get<building_block>(pending->_block_stage);
   bb._s_header = merkle_processor->get_s_headers( block_num );
}

void controller::set_disable_replay_opts( bool v ) {
   my->conf.disable_replay_opts = v;
}

uint32_t controller::head_block_num()const {
   return my->head->block_num;
}
time_point controller::head_block_time()const {
   return my->head->header.timestamp;
}
block_id_type controller::head_block_id()const {
   return my->head->id;
}
account_name  controller::head_block_producer()const {
   return my->head->header.producer;
}
const block_header& controller::head_block_header()const {
   return my->head->header;
}
block_state_ptr controller::head_block_state()const {
   return my->head;
}

block_state_ptr controller_impl::fork_db_head() const {
   if( read_mode == db_read_mode::IRREVERSIBLE ) {
      // When in IRREVERSIBLE mode fork_db blocks are marked valid when they become irreversible so that
      // fork_db.head() returns irreversible block
      // Use pending_head since this method should return the chain head and not last irreversible.
      return fork_db.pending_head();
   } else {
      return fork_db.head();
   }
}

uint32_t controller::fork_db_head_block_num()const {
   return my->fork_db_head()->block_num;
}

block_id_type controller::fork_db_head_block_id()const {
   return my->fork_db_head()->id;
}

block_timestamp_type controller::pending_block_timestamp()const {
   SYS_ASSERT( my->pending, block_validate_exception, "no pending block" );

   if( std::holds_alternative<completed_block>(my->pending->_block_stage) )
      return std::get<completed_block>(my->pending->_block_stage)._block_state->header.timestamp;

   return my->pending->get_pending_block_header_state().timestamp;
}

time_point controller::pending_block_time()const {
   return pending_block_timestamp();
}

uint32_t controller::pending_block_num()const {
   SYS_ASSERT( my->pending, block_validate_exception, "no pending block" );

   if( std::holds_alternative<completed_block>(my->pending->_block_stage) )
      return std::get<completed_block>(my->pending->_block_stage)._block_state->header.block_num();

   return my->pending->get_pending_block_header_state().block_num;
}

account_name controller::pending_block_producer()const {
   SYS_ASSERT( my->pending, block_validate_exception, "no pending block" );

   if( std::holds_alternative<completed_block>(my->pending->_block_stage) )
      return std::get<completed_block>(my->pending->_block_stage)._block_state->header.producer;

   return my->pending->get_pending_block_header_state().producer;
}

const block_signing_authority& controller::pending_block_signing_authority()const {
   SYS_ASSERT( my->pending, block_validate_exception, "no pending block" );

   if( std::holds_alternative<completed_block>(my->pending->_block_stage) )
      return std::get<completed_block>(my->pending->_block_stage)._block_state->valid_block_signing_authority;

   return my->pending->get_pending_block_header_state().valid_block_signing_authority;
}

std::optional<block_id_type> controller::pending_producer_block_id()const {
   SYS_ASSERT( my->pending, block_validate_exception, "no pending block" );
   return my->pending->_producer_block_id;
}

const deque<transaction_receipt>& controller::get_pending_trx_receipts()const {
   SYS_ASSERT( my->pending, block_validate_exception, "no pending block" );
   const auto& bb = std::get<building_block>(my->pending->_block_stage);
   return bb._pending_trx_receipts;
}

uint32_t controller::last_irreversible_block_num() const {
   return my->fork_db.root()->block_num;
}

block_id_type controller::last_irreversible_block_id() const {
   return my->fork_db.root()->id;
}

time_point controller::last_irreversible_block_time() const {
   return my->fork_db.root()->header.timestamp.to_time_point();
}


const dynamic_global_property_object& controller::get_dynamic_global_properties()const {
  return my->db.get<dynamic_global_property_object>();
}
const global_property_object& controller::get_global_properties()const {
  return my->db.get<global_property_object>();
}

signed_block_ptr controller::fetch_block_by_id( const block_id_type& id )const {
   auto state = my->fork_db.get_block(id);
   if( state && state->block ) return state->block;
   auto bptr = my->blog.read_block_by_num( block_header::num_from_id(id) );
   if( bptr && bptr->calculate_id() == id ) return bptr;
   return signed_block_ptr();
}

std::optional<signed_block_header> controller::fetch_block_header_by_id( const block_id_type& id )const {
   auto state = my->fork_db.get_block(id);
   if( state && state->block ) return state->header;
   auto result = my->blog.read_block_header_by_num( block_header::num_from_id(id) );
   if( result && result->calculate_id() == id ) return result;
   return {};
}

signed_block_ptr controller::fetch_block_by_number( uint32_t block_num )const  { try {
   auto blk_state = fetch_block_state_by_number( block_num );
   if( blk_state ) {
      return blk_state->block;
   }

   return my->blog.read_block_by_num(block_num);
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

std::optional<signed_block_header> controller::fetch_block_header_by_number( uint32_t block_num )const  { try {
   auto blk_state = fetch_block_state_by_number( block_num );
   if( blk_state ) {
      return blk_state->header;
   }

   return my->blog.read_block_header_by_num(block_num);
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

block_state_ptr controller::fetch_block_state_by_id( block_id_type id )const {
   auto state = my->fork_db.get_block(id);
   return state;
}

block_state_ptr controller::fetch_block_state_by_number( uint32_t block_num )const  { try {
   return my->fork_db.search_on_branch( fork_db_head_block_id(), block_num );
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

block_header_state_ptr controller::fetch_irr_block_header_state_by_number( uint32_t block_num )const  { try {
   SYS_ASSERT(my->slog, block_log_exception, "Block State log is not configured.");
   return my->slog->read_block_by_num(block_num);
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

block_id_type controller::get_block_id_for_num( uint32_t block_num )const { try {
   const auto& blog_head = my->blog.head();

   bool find_in_blog = (blog_head && block_num <= blog_head->block_num());

   if( !find_in_blog ) {
      auto bsp = fetch_block_state_by_number( block_num );
      if( bsp ) return bsp->id;
   }

   auto id = my->blog.read_block_id_by_num(block_num);

   SYS_ASSERT( BOOST_LIKELY( id != block_id_type() ), unknown_block_exception,
               "Could not find block: ${block}", ("block", block_num) );

   return id;
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

fc::sha256 controller::calculate_integrity_hash() { try {
   return my->calculate_integrity_hash();
} FC_LOG_AND_RETHROW() }

void controller::write_snapshot( const snapshot_writer_ptr& snapshot ) {
   SYS_ASSERT( !my->pending, block_validate_exception, "cannot take a consistent snapshot with a pending block" );
   my->writing_snapshot.store(true, std::memory_order_release);
   fc::scoped_exit<std::function<void()>> e = [&] {
      my->writing_snapshot.store(false, std::memory_order_release);
   };
   my->add_to_snapshot(snapshot);
}

bool controller::is_writing_snapshot() const {
   return my->writing_snapshot.load(std::memory_order_acquire);
}

int64_t controller::set_proposed_producers( vector<producer_authority> producers ) {
   const auto& gpo = get_global_properties();
   auto cur_block_num = head_block_num() + 1;

   if( producers.size() == 0 && is_builtin_activated( builtin_protocol_feature_t::disallow_empty_producer_schedule ) ) {
      return -1;
   }

   if( gpo.proposed_schedule_block_num ) {
      if( *gpo.proposed_schedule_block_num != cur_block_num )
         return -1; // there is already a proposed schedule set in a previous block, wait for it to become pending

      if( std::equal( producers.begin(), producers.end(),
                      gpo.proposed_schedule.producers.begin(), gpo.proposed_schedule.producers.end() ) )
         return -1; // the proposed producer schedule does not change
   }

   producer_authority_schedule sch;

   decltype(sch.producers.cend()) end;
   decltype(end)                  begin;

   const auto& pending_sch = pending_producers();

   if( pending_sch.producers.size() == 0 ) {
      const auto& active_sch = active_producers();
      begin = active_sch.producers.begin();
      end   = active_sch.producers.end();
      sch.version = active_sch.version + 1;
   } else {
      begin = pending_sch.producers.begin();
      end   = pending_sch.producers.end();
      sch.version = pending_sch.version + 1;
   }

   if( std::equal( producers.begin(), producers.end(), begin, end ) )
      return -1; // the producer schedule would not change

   sch.producers = std::move(producers);

   int64_t version = sch.version;

   ilog( "proposed producer schedule with version ${v}", ("v", version) );

   my->db.modify( gpo, [&]( auto& gp ) {
      gp.proposed_schedule_block_num = cur_block_num;
      gp.proposed_schedule = sch.to_shared(gp.proposed_schedule.producers.get_allocator());
   });
   return version;
}

const producer_authority_schedule&    controller::active_producers()const {
   if( !(my->pending) )
      return  my->head->active_schedule;

   if( std::holds_alternative<completed_block>(my->pending->_block_stage) )
      return std::get<completed_block>(my->pending->_block_stage)._block_state->active_schedule;

   return my->pending->get_pending_block_header_state().active_schedule;
}

const producer_authority_schedule& controller::pending_producers()const {
   if( !(my->pending) )
      return  my->head->pending_schedule.schedule;

   if( std::holds_alternative<completed_block>(my->pending->_block_stage) )
      return std::get<completed_block>(my->pending->_block_stage)._block_state->pending_schedule.schedule;

   if( std::holds_alternative<assembled_block>(my->pending->_block_stage) ) {
      const auto& new_prods_cache = std::get<assembled_block>(my->pending->_block_stage)._new_producer_authority_cache;
      if( new_prods_cache ) {
         return *new_prods_cache;
      }
   }

   const auto& bb = std::get<building_block>(my->pending->_block_stage);

   if( bb._new_pending_producer_schedule )
      return *bb._new_pending_producer_schedule;

   return bb._pending_block_header_state.prev_pending_schedule.schedule;
}

std::optional<producer_authority_schedule> controller::proposed_producers()const {
   const auto& gpo = get_global_properties();
   if( !gpo.proposed_schedule_block_num )
      return std::optional<producer_authority_schedule>();

   return producer_authority_schedule::from_shared(gpo.proposed_schedule);
}

bool controller::light_validation_allowed() const {
   if (!my->pending || my->in_trx_requiring_checks) {
      return false;
   }

   const auto pb_status = my->pending->_block_status;

   // in a pending irreversible or previously validated block and we have forcing all checks
   const bool consider_skipping_on_replay =
         (pb_status == block_status::irreversible || pb_status == block_status::validated) && !my->conf.force_all_checks;

   // OR in a signed block and in light validation mode
   const bool consider_skipping_on_validate = (pb_status == block_status::complete &&
         (my->conf.block_validation_mode == validation_mode::LIGHT || my->trusted_producer_light_validation));

   return consider_skipping_on_replay || consider_skipping_on_validate;
}


bool controller::skip_auth_check() const {
   return light_validation_allowed();
}

bool controller::skip_trx_checks() const {
   return light_validation_allowed();
}

bool controller::skip_db_sessions( block_status bs ) const {
   bool consider_skipping = bs == block_status::irreversible;
   return consider_skipping
      && !my->conf.disable_replay_opts
      && !my->in_trx_requiring_checks;
}

bool controller::skip_db_sessions() const {
   if (my->pending) {
      return skip_db_sessions(my->pending->_block_status);
   } else {
      return false;
   }
}

bool controller::is_trusted_producer( const account_name& producer) const {
   return get_validation_mode() == chain::validation_mode::LIGHT || my->conf.trusted_producers.count(producer);
}

bool controller::contracts_console()const {
   return my->conf.contracts_console;
}

bool controller::is_profiling(account_name account) const {
   return my->conf.profile_accounts.find(account) != my->conf.profile_accounts.end();
}

chain_id_type controller::get_chain_id()const {
   return my->chain_id;
}

db_read_mode controller::get_read_mode()const {
   return my->read_mode;
}

validation_mode controller::get_validation_mode()const {
   return my->conf.block_validation_mode;
}

uint32_t controller::get_terminate_at_block()const {
   return my->conf.terminate_at_block;
}

const apply_handler* controller::find_apply_handler( account_name receiver, account_name scope, action_name act ) const
{
   auto native_handler_scope = my->apply_handlers.find( receiver );
   if( native_handler_scope != my->apply_handlers.end() ) {
      auto handler = native_handler_scope->second.find( make_pair( scope, act ) );
      if( handler != native_handler_scope->second.end() )
         return &handler->second;
   }
   return nullptr;
}
wasm_interface& controller::get_wasm_interface() {
   return my->get_wasm_interface();
}

const account_object& controller::get_account( account_name name )const
{ try {
   return my->db.get<account_object, by_name>(name);
} FC_CAPTURE_AND_RETHROW( (name) ) }

bool controller::sender_avoids_whitelist_blacklist_enforcement( account_name sender )const {
   return my->sender_avoids_whitelist_blacklist_enforcement( sender );
}

void controller::check_actor_list( const flat_set<account_name>& actors )const {
   my->check_actor_list( actors );
}

void controller::check_contract_list( account_name code )const {
   my->check_contract_list( code );
}

void controller::check_action_list( account_name code, action_name action )const {
   my->check_action_list( code, action );
}

void controller::check_key_list( const public_key_type& key )const {
   my->check_key_list( key );
}

bool controller::is_building_block()const {
   return my->pending.has_value();
}

bool controller::is_speculative_block()const {
   if( !my->pending ) return false;

   return (my->pending->_block_status == block_status::incomplete || my->pending->_block_status == block_status::ephemeral );
}

bool controller::is_ram_billing_in_notify_allowed()const {
   return my->conf.disable_all_subjective_mitigations || !is_speculative_block() || my->conf.allow_ram_billing_in_notify;
}

uint32_t controller::configured_subjective_signature_length_limit()const {
   return my->conf.maximum_variable_signature_length;
}

void controller::validate_expiration( const transaction& trx )const { try {
   const auto& chain_configuration = get_global_properties().configuration;

   SYS_ASSERT( trx.expiration.to_time_point() >= pending_block_time(),
               expired_tx_exception,
               "transaction has expired, "
               "expiration is ${trx.expiration} and pending block time is ${pending_block_time}",
               ("trx.expiration",trx.expiration)("pending_block_time",pending_block_time()));
   SYS_ASSERT( trx.expiration.to_time_point() <= pending_block_time() + fc::seconds(chain_configuration.max_transaction_lifetime),
               tx_exp_too_far_exception,
               "Transaction expiration is too far in the future relative to the reference time of ${reference_time}, "
               "expiration is ${trx.expiration} and the maximum transaction lifetime is ${max_til_exp} seconds",
               ("trx.expiration",trx.expiration)("reference_time",pending_block_time())
               ("max_til_exp",chain_configuration.max_transaction_lifetime) );
} FC_CAPTURE_AND_RETHROW((trx)) }

void controller::validate_tapos( const transaction& trx )const { try {
   const auto& tapos_block_summary = db().get<block_summary_object>((uint16_t)trx.ref_block_num);

   //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
   SYS_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), invalid_ref_block_exception,
              "Transaction's reference block did not match. Is this transaction from a different fork?",
              ("tapos_summary", tapos_block_summary));
} FC_CAPTURE_AND_RETHROW() }

void controller::validate_db_available_size() const {
   const auto free = db().get_free_memory();
   const auto guard = my->conf.state_guard_size;
   SYS_ASSERT(free >= guard, database_guard_exception, "database free: ${f}, guard size: ${g}", ("f", free)("g",guard));

   // give a change to chainbase to write some pages to disk if memory becomes scarce.
   if (is_write_window()) {
      if (auto flushed_pages = mutable_db().check_memory_and_flush_if_needed()) {
         ilog("CHAINBASE: flushed ${p} pages to disk to decrease memory pressure", ("p", flushed_pages));
      }
   }
}

bool controller::is_protocol_feature_activated( const digest_type& feature_digest )const {
   if( my->pending )
      return my->pending->is_protocol_feature_activated( feature_digest );

   const auto& activated_features = my->head->activated_protocol_features->protocol_features;
   return (activated_features.find( feature_digest ) != activated_features.end());
}

bool controller::is_builtin_activated( builtin_protocol_feature_t f )const {
   uint32_t current_block_num = head_block_num();

   if( my->pending ) {
      ++current_block_num;
   }

   return my->protocol_features.is_builtin_activated( f, current_block_num );
}

bool controller::is_known_unexpired_transaction( const transaction_id_type& id) const {
   return db().find<transaction_object, by_trx_id>(id);
}

void controller::set_subjective_cpu_leeway(fc::microseconds leeway) {
   my->subjective_cpu_leeway = leeway;
}

std::optional<fc::microseconds> controller::get_subjective_cpu_leeway() const {
    return my->subjective_cpu_leeway;
}

void controller::set_greylist_limit( uint32_t limit ) {
   SYS_ASSERT( 0 < limit && limit <= chain::config::maximum_elastic_resource_multiplier,
               misc_exception,
               "Invalid limit (${limit}) passed into set_greylist_limit. "
               "Must be between 1 and ${max}.",
               ("limit", limit)("max", chain::config::maximum_elastic_resource_multiplier)
   );
   my->conf.greylist_limit = limit;
}

uint32_t controller::get_greylist_limit()const {
   return my->conf.greylist_limit;
}

void controller::add_resource_greylist(const account_name &name) {
   my->conf.resource_greylist.insert(name);
}

void controller::remove_resource_greylist(const account_name &name) {
   my->conf.resource_greylist.erase(name);
}

bool controller::is_resource_greylisted(const account_name &name) const {
   return my->conf.resource_greylist.find(name) !=  my->conf.resource_greylist.end();
}

const flat_set<account_name> &controller::get_resource_greylist() const {
   return  my->conf.resource_greylist;
}


void controller::add_to_ram_correction( account_name account, uint64_t ram_bytes ) {
   auto ptr = my->db.find<account_ram_correction_object, by_name>( account );
   if( ptr ) {
      my->db.modify<account_ram_correction_object>( *ptr, [&]( auto& rco ) {
         rco.ram_correction += ram_bytes;
      } );
   } else {
      ptr = &my->db.create<account_ram_correction_object>( [&]( auto& rco ) {
         rco.name = account;
         rco.ram_correction = ram_bytes;
      } );
   }

   // on_add_ram_correction is only called for deferred transaction
   // (in apply_context::schedule_deferred_transaction)
   if (auto dm_logger = get_deep_mind_logger(false)) {
      dm_logger->on_add_ram_correction(*ptr, ram_bytes);
   }
}

bool controller::all_subjective_mitigations_disabled()const {
   return my->conf.disable_all_subjective_mitigations;
}

deep_mind_handler* controller::get_deep_mind_logger(bool is_trx_transient)const {
   return my->get_deep_mind_logger(is_trx_transient);
}

void controller::enable_deep_mind(deep_mind_handler* logger) {
   SYS_ASSERT( logger != nullptr, misc_exception, "Invalid logger passed into enable_deep_mind, must be set" );
   my->deep_mind_logger = logger;
}

uint32_t controller::earliest_available_block_num() const{
   return my->earliest_available_block_num();
}
#if defined(SYSIO_SYS_VM_RUNTIME_ENABLED) || defined(SYSIO_SYS_VM_JIT_RUNTIME_ENABLED)
vm::wasm_allocator& controller::get_wasm_allocator() {
   return my->wasm_alloc;
}
#endif
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
bool controller::is_sys_vm_oc_enabled() const {
   return my->is_sys_vm_oc_enabled();
}
#endif

std::optional<uint64_t> controller::convert_exception_to_error_code( const fc::exception& e ) {
   const chain_exception* e_ptr = dynamic_cast<const chain_exception*>( &e );

   if( e_ptr == nullptr ) return {};

   if( !e_ptr->error_code ) return static_cast<uint64_t>(system_error_code::generic_system_error);

   return e_ptr->error_code;
}

chain_id_type controller::extract_chain_id(snapshot_reader& snapshot) {
   chain_snapshot_header header;
   snapshot.read_section<chain_snapshot_header>([&header]( auto &section ){
      section.read_row(header);
      header.validate();
   });

   chain_id_type chain_id;

   snapshot.read_section<global_property_object>([&chain_id]( auto &section ){
      snapshot_global_property_object global_properties;
      section.read_row(global_properties);
      chain_id = global_properties.chain_id;
   });

   return chain_id;
}

std::optional<chain_id_type> controller::extract_chain_id_from_db( const path& state_dir ) {
   try {
      chainbase::database db( state_dir, chainbase::database::read_only );

      db.add_index<database_header_multi_index>();
      db.add_index<global_property_multi_index>();

      controller_impl::validate_db_version( db );

      if( db.revision() < 1 ) return {};

      auto * gpo = db.find<global_property_object>();
      if (gpo==nullptr) return {};

      return gpo->chain_id;
   } catch( const std::system_error& e ) {
      // do not propagate db_error_code::not_found for absent db, so it will be created
      if( e.code().value() != chainbase::db_error_code::not_found )
         throw;
   }

   return {};
}

void controller::replace_producer_keys( const public_key_type& key ) {
   ilog("Replace producer keys with ${k}", ("k", key));
   mutable_db().modify( db().get<global_property_object>(), [&]( auto& gp ) {
      gp.proposed_schedule_block_num = {};
      gp.proposed_schedule.version = 0;
      gp.proposed_schedule.producers.clear();
   });
   auto version = my->head->pending_schedule.schedule.version;
   my->head->pending_schedule = {};
   my->head->pending_schedule.schedule.version = version;
   for (auto& prod: my->head->active_schedule.producers ) {
      ilog("${n}", ("n", prod.producer_name));
      std::visit([&](auto &auth) {
         auth.threshold = 1;
         auth.keys = {key_weight{key, 1}};
      }, prod.authority);
   }
}

void controller::replace_account_keys( name account, name permission, const public_key_type& key ) {
   auto& rlm = get_mutable_resource_limits_manager();
   auto* perm = db().find<permission_object, by_owner>(boost::make_tuple(account, permission));
   if (!perm)
      return;
   int64_t old_size = (int64_t)(chain::config::billable_size_v<permission_object> + perm->auth.get_billable_size());
   mutable_db().modify(*perm, [&](auto& p) {
      p.auth = authority(key);
   });
   int64_t new_size = (int64_t)(chain::config::billable_size_v<permission_object> + perm->auth.get_billable_size());
   rlm.add_pending_ram_usage(account, new_size - old_size, false); // false for doing dm logging
   rlm.verify_account_ram_usage(account);
}

void controller::set_producer_node(bool is_producer_node) {
   my->is_producer_node = is_producer_node;
}

bool controller::is_producer_node()const {
   return my->is_producer_node;
}

void controller::set_db_read_only_mode() {
   mutable_db().set_read_only_mode();
}

void controller::unset_db_read_only_mode() {
   mutable_db().unset_read_only_mode();
}

void controller::init_thread_local_data() {
   my->init_thread_local_data();
}

void controller::set_to_write_window() {
   my->set_to_write_window();
}
void controller::set_to_read_window() {
   my->set_to_read_window();
}
bool controller::is_write_window() const {
   return my->is_write_window();
}

void controller::code_block_num_last_used(const digest_type& code_hash, uint8_t vm_type, uint8_t vm_version, uint32_t block_num) {
   return my->code_block_num_last_used(code_hash, vm_type, vm_version, block_num);
}

bool controller::is_irreversible_state_available() const {
   return !!my->slog;
}

void controller::initialize_root_extensions(contract_action_matches&& matches) {
      auto root_txn_ident = std::make_shared<root_txn_identification>(
         std::move(matches)
      );
      applied_transaction.connect(
         [self=this,root_txn_ident]( std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t ) {
            if( self->is_builtin_activated( chain::builtin_protocol_feature_t::multiple_state_roots_supported ) ) {
               root_txn_ident->signal_applied_transaction(std::get<0>(t), std::get<1>(t));
            }
         } );
      accepted_block.connect(
         [self=this,root_txn_ident]( const block_state_ptr& blk ) {
            if( self->is_builtin_activated( chain::builtin_protocol_feature_t::multiple_state_roots_supported ) ) {
               root_txn_ident->signal_accepted_block(blk);
            }
         } ) ;
      block_start.connect(
         [self=this,root_txn_ident]( uint32_t block_num ) {
            if( self->is_builtin_activated( chain::builtin_protocol_feature_t::multiple_state_roots_supported ) ) {
               root_txn_ident->signal_block_start(block_num);
            }
         } ) ;
      my->merkle_processor = std::make_shared<block_root_processor>(my->db, std::move(root_txn_ident));
}

/// Protocol feature activation handlers:

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::preactivate_feature>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "preactivate_feature" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "is_feature_activated" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::get_sender>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "get_sender" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::replace_deferred>() {
   const auto& indx = db.get_index<account_ram_correction_index, by_id>();
   for( auto itr = indx.begin(); itr != indx.end(); itr = indx.begin() ) {
      int64_t current_ram_usage = resource_limits.get_account_ram_usage( itr->name );
      int64_t ram_delta = -static_cast<int64_t>(itr->ram_correction);
      if( itr->ram_correction > static_cast<uint64_t>(current_ram_usage) ) {
         ram_delta = -current_ram_usage;
         elog( "account ${name} was to be reduced by ${adjust} bytes of RAM despite only using ${current} bytes of RAM",
               ("name", itr->name)("adjust", itr->ram_correction)("current", current_ram_usage) );
      }

      // This method is only called for deferred transaction
      if (auto dm_logger = get_deep_mind_logger(false)) {
         dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", itr->id._id)), "deferred_trx", "correction", "deferred_trx_ram_correction");
      }

      resource_limits.add_pending_ram_usage( itr->name, ram_delta, false ); // false for doing dm logging
      db.remove( *itr );
   }
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::webauthn_key>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      ps.num_supported_key_types = 3;
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::em_key>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      ps.num_supported_key_types = 4;
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::ed_key>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      ps.num_supported_key_types = 5;
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::wtmsig_block_signatures>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "set_proposed_producers_ex" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::action_return_value>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "set_action_return_value" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::configurable_wasm_limits>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "set_wasm_parameters_packed" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "get_wasm_parameters_packed" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::blockchain_parameters>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "get_parameters_packed" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "set_parameters_packed" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::get_code_hash>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "get_code_hash" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::get_block_num>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "get_block_num" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::crypto_primitives>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "alt_bn128_add" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "alt_bn128_mul" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "alt_bn128_pair" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "mod_exp" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "blake2_f" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "sha3" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "k1_recover" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "blake2b_256" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::bls_primitives>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_g1_add" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_g2_add" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_g1_weighted_sum" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_g2_weighted_sum" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_pairing" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_g1_map" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_g2_map" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_fp_mod" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_fp_mul" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "bls_fp_exp" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::disable_deferred_trxs_stage_2>() {
   const auto& idx = db.get_index<generated_transaction_multi_index, by_trx_id>();
   // remove all deferred trxs and refund their payers
   for( auto itr = idx.begin(); itr != idx.end(); itr = idx.begin() ) {
      remove_scheduled_transaction(*itr);
   }
}

/// End of protocol feature activation handlers

} } /// sysio::chain
