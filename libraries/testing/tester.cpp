#include <boost/test/unit_test.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/block_log.hpp>
#include <sysio/chain/wast_to_wasm.hpp>
#include <sysio/chain/sysio_contract.hpp>
#include <sysio/chain/generated_transaction_object.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>

#include <fstream>

#include <contracts.hpp>

namespace bio = boost::iostreams;

sysio::chain::asset core_from_string(const std::string& s) {
  return sysio::chain::asset::from_string(s + " " CORE_SYMBOL_NAME);
}

namespace sysio { namespace testing {

   // required by boost::unit_test::data
   std::ostream& operator<<(std::ostream& os, setup_policy p) {
      switch(p) {
         case setup_policy::none:
            os << "none";
            break;
         case setup_policy::old_bios_only:
            os << "old_bios_only";
            break;
         case setup_policy::preactivate_feature_only:
            os << "preactivate_feature_only";
            break;
         case setup_policy::preactivate_feature_and_new_bios:
            os << "preactivate_feature_and_new_bios";
            break;
         case setup_policy::old_wasm_parser:
            os << "old_wasm_parser";
            break;
         case setup_policy::full:
            os << "full";
            break;
         default:
            FC_ASSERT(false, "Unknown setup_policy");
      }
      return os;
   }

   std::string read_wast( const char* fn ) {
      std::ifstream wast_file(fn);
      FC_ASSERT( wast_file.is_open(), "wast file cannot be found" );
      wast_file.seekg(0, std::ios::end);
      std::vector<char> wast;
      int len = wast_file.tellg();
      FC_ASSERT( len >= 0, "wast file length is -1" );
      wast.resize(len+1);
      wast_file.seekg(0, std::ios::beg);
      wast_file.read(wast.data(), wast.size());
      wast[wast.size()-1] = '\0';
      wast_file.close();
      return {wast.data()};
   }

   std::vector<uint8_t> read_wasm( const char* fn ) {
      std::ifstream wasm_file(fn, std::ios::binary);
      FC_ASSERT( wasm_file.is_open(), "wasm file cannot be found" );
      wasm_file.seekg(0, std::ios::end);
      std::vector<uint8_t> wasm;
      int len = wasm_file.tellg();
      FC_ASSERT( len >= 0, "wasm file length is -1" );
      wasm.resize(len);
      wasm_file.seekg(0, std::ios::beg);
      wasm_file.read((char*)wasm.data(), wasm.size());
      wasm_file.close();
      return wasm;
   }

   std::vector<char> read_abi( const char* fn ) {
      std::ifstream abi_file(fn);
      FC_ASSERT( abi_file.is_open(), "abi file cannot be found" );
      abi_file.seekg(0, std::ios::end);
      std::vector<char> abi;
      int len = abi_file.tellg();
      FC_ASSERT( len >= 0, "abi file length is -1" );
      abi.resize(len+1);
      abi_file.seekg(0, std::ios::beg);
      abi_file.read(abi.data(), abi.size());
      abi[abi.size()-1] = '\0';
      abi_file.close();
      return abi;
   }

   namespace {
      std::string read_gzipped_snapshot( const char* fn ) {
         std::ifstream file(fn, std::ios_base::in | std::ios_base::binary);
         bio::filtering_streambuf<bio::input> in;

         in.push(bio::gzip_decompressor());
         in.push(file);

         std::stringstream decompressed;
         bio::copy(in, decompressed);
         return decompressed.str();
      }
   }

   std::string read_binary_snapshot( const char* fn ) {
      return read_gzipped_snapshot(fn);
   }

   fc::variant read_json_snapshot( const char* fn ) {
      return fc::json::from_string( read_gzipped_snapshot(fn) );
   }

   const fc::microseconds base_tester::abi_serializer_max_time{1000*1000}; // 1s for slow test machines

   bool expect_assert_message(const fc::exception& ex, string expected) {
      BOOST_TEST_MESSAGE("LOG : " << "expected: " << expected << ", actual: " << ex.get_log().at(0).get_message());
      return (ex.get_log().at(0).get_message().find(expected) != std::string::npos);
   }

   fc::variant_object filter_fields(const fc::variant_object& filter, const fc::variant_object& value) {
      fc::mutable_variant_object res;
      for( auto& entry : filter ) {
         auto it = value.find(entry.key());
         res( it->key(), it->value() );
      }
      return res;
   }

   void copy_row(const chain::key_value_object& obj, vector<char>& data) {
      data.resize( obj.value.size() );
      memcpy( data.data(), obj.value.data(), obj.value.size() );
   }

   protocol_feature_set make_protocol_feature_set(const subjective_restriction_map& custom_subjective_restrictions) {
      protocol_feature_set pfs;

      map< builtin_protocol_feature_t, std::optional<digest_type> > visited_builtins;

      std::function<digest_type(builtin_protocol_feature_t)> add_builtins =
      [&pfs, &visited_builtins, &add_builtins, &custom_subjective_restrictions]
      ( builtin_protocol_feature_t codename ) -> digest_type {
         auto res = visited_builtins.emplace( codename, std::optional<digest_type>() );
         if( !res.second ) {
            SYS_ASSERT( res.first->second, protocol_feature_exception,
                        "invariant failure: cycle found in builtin protocol feature dependencies"
            );
            return *res.first->second;
         }

         auto f = protocol_feature_set::make_default_builtin_protocol_feature( codename,
         [&add_builtins]( builtin_protocol_feature_t d ) {
            return add_builtins( d );
         } );

         const auto itr = custom_subjective_restrictions.find(codename);
         if( itr != custom_subjective_restrictions.end() ) {
            f.subjective_restrictions = itr->second;
         }

         const auto& pf = pfs.add_feature( f );
         res.first->second = pf.feature_digest;

         return pf.feature_digest;
      };

      for( const auto& p : builtin_protocol_feature_codenames ) {
         add_builtins( p.first );
      }

      return pfs;
   }

   bool base_tester::is_same_chain( base_tester& other ) {
     return control->head_block_id() == other.control->head_block_id();
   }

   void base_tester::init(const setup_policy policy, db_read_mode read_mode, std::optional<uint32_t> genesis_max_inline_action_size) {
      auto def_conf = default_config(tempdir, genesis_max_inline_action_size);
      def_conf.first.read_mode = read_mode;
      cfg = def_conf.first;

      open(def_conf.second);
      execute_setup_policy(policy);
   }

   void base_tester::init(controller::config config, const snapshot_reader_ptr& snapshot) {
      cfg = config;
      open(snapshot);
   }

   void base_tester::init(controller::config config, const genesis_state& genesis) {
      cfg = config;
      open(genesis);
   }

   void base_tester::init(controller::config config) {
      cfg = config;
      open(default_genesis().compute_chain_id());
   }

   void base_tester::init(controller::config config, protocol_feature_set&& pfs, const snapshot_reader_ptr& snapshot) {
      cfg = config;
      open(std::move(pfs), snapshot);
   }

   void base_tester::init(controller::config config, protocol_feature_set&& pfs, const genesis_state& genesis) {
      cfg = config;
      open(std::move(pfs), genesis);
   }

   void base_tester::init(controller::config config, protocol_feature_set&& pfs) {
      cfg = config;
      open(std::move(pfs), default_genesis().compute_chain_id());
   }

   void base_tester::execute_setup_policy(const setup_policy policy) {
      const auto& pfm = control->get_protocol_feature_manager();

      auto schedule_preactivate_protocol_feature = [&]() {
         auto preactivate_feature_digest = pfm.get_builtin_digest(builtin_protocol_feature_t::preactivate_feature);
         FC_ASSERT( preactivate_feature_digest, "PREACTIVATE_FEATURE not found" );
         schedule_protocol_features_wo_preactivation( { *preactivate_feature_digest } );
      };

      switch (policy) {
         case setup_policy::old_bios_only: {
            set_before_preactivate_bios_contract();
            break;
         }
         case setup_policy::preactivate_feature_only: {
            schedule_preactivate_protocol_feature();
            produce_block(); // block production is required to activate protocol feature
            break;
         }
         case setup_policy::preactivate_feature_and_new_bios: {
            schedule_preactivate_protocol_feature();
            produce_block();
            set_before_producer_authority_bios_contract();
            preactivate_builtin_protocol_features({
               chain::builtin_protocol_feature_t::get_block_num,
            });
            produce_block();
            init_roa();
            break;
         }
         case setup_policy::old_wasm_parser: {
            schedule_preactivate_protocol_feature();
            produce_block();
            set_before_producer_authority_bios_contract();
            preactivate_builtin_protocol_features({
               builtin_protocol_feature_t::only_link_to_existing_permission,
               builtin_protocol_feature_t::replace_deferred,
               builtin_protocol_feature_t::no_duplicate_deferred_id,
               builtin_protocol_feature_t::fix_linkauth_restriction,
               builtin_protocol_feature_t::disallow_empty_producer_schedule,
               builtin_protocol_feature_t::restrict_action_to_self,
               builtin_protocol_feature_t::only_bill_first_authorizer,
               builtin_protocol_feature_t::forward_setcode,
               builtin_protocol_feature_t::get_sender,
               builtin_protocol_feature_t::ram_restrictions,
               builtin_protocol_feature_t::webauthn_key,
               builtin_protocol_feature_t::wtmsig_block_signatures,
               builtin_protocol_feature_t::em_key,
               builtin_protocol_feature_t::get_block_num,
            });
            produce_block();
            set_bios_contract();
            init_roa();
            break;
         }
         case setup_policy::full:
         case setup_policy::full_except_do_not_disable_deferred_trx: {
            schedule_preactivate_protocol_feature();
            produce_block();
            set_before_producer_authority_bios_contract();
            if( policy == setup_policy::full ) {
               preactivate_all_builtin_protocol_features();
            } else {
               preactivate_all_but_disable_deferred_trx();
            }
            produce_block();
            set_bios_contract();
            init_roa();
            break;
         }
         case setup_policy::none:
         default:
            break;
      };
   }

   void base_tester::close() {
      control.reset();
      chain_transactions.clear();
   }

   void base_tester::open( const snapshot_reader_ptr& snapshot ) {
      open( make_protocol_feature_set(), snapshot );
   }

   void base_tester::open( const genesis_state& genesis ) {
      open( make_protocol_feature_set(), genesis );
   }

   void base_tester::open( std::optional<chain_id_type> expected_chain_id ) {
      open( make_protocol_feature_set(), expected_chain_id );
   }

   void base_tester::open( protocol_feature_set&& pfs, std::optional<chain_id_type> expected_chain_id, const std::function<void()>& lambda ) {
      if( !expected_chain_id ) {
         expected_chain_id = controller::extract_chain_id_from_db( cfg.state_dir );
         if( !expected_chain_id ) {
            std::filesystem::path retained_dir;
            auto partitioned_config = std::get_if<partitioned_blocklog_config>(&cfg.blog);
            if (partitioned_config) {
               retained_dir = partitioned_config->retained_dir;
               if (retained_dir.is_relative())
                  retained_dir = cfg.blocks_dir/retained_dir;
            }
            expected_chain_id = block_log<signed_block>::extract_chain_id( cfg.blocks_dir, retained_dir );
         }
      }

      control.reset( new controller(cfg, std::move(pfs), *expected_chain_id) );
      if (root_matches) {
         control->initialize_root_extensions(std::move(*root_matches));
      }
      control->add_indices();
      if (lambda) lambda();
      chain_transactions.clear();
      control->accepted_block.connect([this]( const block_state_ptr& block_state ){
        FC_ASSERT( block_state->block );
          for( auto receipt : block_state->block->transactions ) {
              if( std::holds_alternative<packed_transaction>(receipt.trx) ) {
                  auto &pt = std::get<packed_transaction>(receipt.trx);
                  chain_transactions[pt.get_transaction().id()] = std::move(receipt);
              } else {
                  auto& id = std::get<transaction_id_type>(receipt.trx);
                  chain_transactions[id] = std::move(receipt);
              }
          }
      });
   }

   void base_tester::open( protocol_feature_set&& pfs, const snapshot_reader_ptr& snapshot ) {
      const auto& snapshot_chain_id = controller::extract_chain_id( *snapshot );
      snapshot->return_to_header();
      open(std::move(pfs), snapshot_chain_id, [&snapshot,&control=this->control]() {
         control->startup( [](){}, []() { return false; }, snapshot );
      });
   }

   void base_tester::open( protocol_feature_set&& pfs, const genesis_state& genesis ) {
      open(std::move(pfs), genesis.compute_chain_id(), [&genesis,&control=this->control]() {
         control->startup( [](){}, []() { return false; }, genesis );
      });
   }

   void base_tester::open( protocol_feature_set&& pfs, std::optional<chain_id_type> expected_chain_id ) {
      open(std::move(pfs), expected_chain_id, [&control=this->control]() {
         control->startup( [](){}, []() { return false; } );
      });
   }

   void base_tester::push_block(signed_block_ptr b) {
      auto bsf = control->create_block_state_future(b->calculate_id(), b);
      unapplied_transactions.add_aborted( control->abort_block() );
      controller::block_report br;
      control->push_block( br, bsf.get(), [this]( const branch_type& forked_branch ) {
         unapplied_transactions.add_forked( forked_branch );
      }, [this]( const transaction_id_type& id ) {
         return unapplied_transactions.get_trx( id );
      } );

      auto itr = last_produced_block.find(b->producer);
      if (itr == last_produced_block.end() || b->block_num() > block_header::num_from_id(itr->second)) {
         last_produced_block[b->producer] = b->calculate_id();
      }
   }

   signed_block_ptr base_tester::_produce_block( fc::microseconds skip_time, bool skip_pending_trxs ) {
      std::vector<transaction_trace_ptr> traces;
      return _produce_block( skip_time, skip_pending_trxs, false, traces );
   }

   signed_block_ptr base_tester::_produce_block( fc::microseconds skip_time, bool skip_pending_trxs,
                                                 bool no_throw, std::vector<transaction_trace_ptr>& traces ) {
      auto head = control->head_block_state();
      auto head_time = control->head_block_time();
      auto next_time = head_time + skip_time;

      if( !control->is_building_block() || control->pending_block_time() != next_time ) {
         _start_block( next_time );
      }

      if( !skip_pending_trxs ) {
         for( auto itr = unapplied_transactions.begin(); itr != unapplied_transactions.end();  ) {
            auto trace = control->push_transaction( itr->trx_meta, fc::time_point::maximum(), fc::microseconds::maximum(), DEFAULT_BILLED_CPU_TIME_US, true, 0 );
            traces.emplace_back( trace );
            if(!no_throw && trace->except) {
               // this always throws an fc::exception, since the original exception is copied into an fc::exception
               trace->except->dynamic_rethrow_exception();
            }
            itr = unapplied_transactions.erase( itr );
         }

         vector<transaction_id_type> scheduled_trxs;
         while ((scheduled_trxs = get_scheduled_transactions()).size() > 0 ) {
            for( const auto& trx : scheduled_trxs ) {
               auto trace = control->push_scheduled_transaction( trx, fc::time_point::maximum(), fc::microseconds::maximum(), DEFAULT_BILLED_CPU_TIME_US, true );
               traces.emplace_back( trace );
               if( !no_throw && trace->except ) {
                  // this always throws an fc::exception, since the original exception is copied into an fc::exception
                  trace->except->dynamic_rethrow_exception();
               }
            }
         }
      }

      auto head_block = _finish_block();

      _start_block( next_time + fc::microseconds(config::block_interval_us));
      return head_block;
   }

   void base_tester::_start_block(fc::time_point block_time) {
      auto head_block_number = control->head_block_num();
      auto producer = control->head_block_state()->get_scheduled_producer(block_time);

      auto last_produced_block_num = control->last_irreversible_block_num();
      auto itr = last_produced_block.find(producer.producer_name);
      if (itr != last_produced_block.end()) {
         last_produced_block_num = std::max(control->last_irreversible_block_num(), block_header::num_from_id(itr->second));
      }

      unapplied_transactions.add_aborted( control->abort_block() );

      vector<digest_type> feature_to_be_activated;
      // First add protocol features to be activated WITHOUT preactivation
      feature_to_be_activated.insert(
         feature_to_be_activated.end(),
         protocol_features_to_be_activated_wo_preactivation.begin(),
         protocol_features_to_be_activated_wo_preactivation.end()
      );
      // Then add protocol features to be activated WITH preactivation
      const auto preactivated_protocol_features = control->get_preactivated_protocol_features();
      feature_to_be_activated.insert(
         feature_to_be_activated.end(),
         preactivated_protocol_features.begin(),
         preactivated_protocol_features.end()
      );

      control->start_block( block_time, head_block_number - last_produced_block_num, feature_to_be_activated,
                            controller::block_status::incomplete );

      // Clear the list, if start block finishes successfuly, the protocol features should be assumed to be activated
      protocol_features_to_be_activated_wo_preactivation.clear();
   }

   signed_block_ptr base_tester::_finish_block() {
      FC_ASSERT( control->is_building_block(), "must first start a block before it can be finished" );

      auto producer = control->head_block_state()->get_scheduled_producer( control->pending_block_time() );
      vector<private_key_type> signing_keys;

      auto default_active_key = get_public_key( producer.producer_name, "active");
      producer.for_each_key([&](const public_key_type& key){
         const auto& iter = block_signing_private_keys.find(key);
         if(iter != block_signing_private_keys.end()) {
            signing_keys.push_back(iter->second);
         } else if (key == default_active_key) {
            signing_keys.emplace_back( get_private_key( producer.producer_name, "active") );
         }
      });

      controller::block_report br;
      control->finalize_block( br, [&]( digest_type d ) {
         std::vector<signature_type> result;
         result.reserve(signing_keys.size());
         for (const auto& k: signing_keys)
             result.emplace_back(k.sign(d));

         return result;
      } );

      control->commit_block();

      last_produced_block[control->head_block_state()->header.producer] = control->head_block_state()->id;

      return control->head_block_state()->block;
   }

   signed_block_ptr base_tester::produce_block( std::vector<transaction_trace_ptr>& traces ) {
      return _produce_block( fc::milliseconds(config::block_interval_ms), false, true, traces );
   }

   void base_tester::produce_blocks( uint32_t n, bool empty ) {
      if( empty ) {
         for( uint32_t i = 0; i < n; ++i )
            produce_empty_block();
      } else {
         for( uint32_t i = 0; i < n; ++i )
            produce_block();
      }
   }

   vector<transaction_id_type> base_tester::get_scheduled_transactions() const {
      const auto& idx = control->db().get_index<generated_transaction_multi_index,by_delay>();

      vector<transaction_id_type> result;

      auto itr = idx.begin();
      while( itr != idx.end() && itr->delay_until <= control->pending_block_time() ) {
         result.emplace_back(itr->trx_id);
         ++itr;
      }
      return result;
   }

   void base_tester::produce_blocks_until_end_of_round() {
      uint64_t blocks_per_round;
      while(true) {
         blocks_per_round = control->active_producers().producers.size() * config::producer_repetitions;
         produce_block();
         if (control->head_block_num() % blocks_per_round == (blocks_per_round - 1)) break;
      }
   }

   void base_tester::produce_blocks_for_n_rounds(const uint32_t num_of_rounds) {
      for(uint32_t i = 0; i < num_of_rounds; i++) {
         produce_blocks_until_end_of_round();
      }
   }

   void base_tester::produce_min_num_of_blocks_to_spend_time_wo_inactive_prod(const fc::microseconds target_elapsed_time) {
      fc::microseconds elapsed_time;
      while (elapsed_time < target_elapsed_time) {
         for(uint32_t i = 0; i < control->head_block_state()->active_schedule.producers.size(); i++) {
            const auto time_to_skip = fc::milliseconds(config::producer_repetitions * config::block_interval_ms);
            produce_block(time_to_skip);
            elapsed_time += time_to_skip;
         }
         // if it is more than 24 hours, producer will be marked as inactive
         const auto time_to_skip = fc::seconds(23 * 60 * 60);
         produce_block(time_to_skip);
         elapsed_time += time_to_skip;
      }

   }


  void base_tester::set_transaction_headers( transaction& trx, uint32_t expiration, uint32_t delay_sec ) const {
     trx.expiration = fc::time_point_sec{control->head_block_time() + fc::seconds(expiration)};
     trx.set_reference_block( control->head_block_id() );

     trx.max_net_usage_words = 0; // No limit
     trx.max_cpu_usage_ms = 0; // No limit
     trx.delay_sec = delay_sec;
  }


   transaction_trace_ptr base_tester::create_account( account_name a, account_name creator, bool multisig, bool include_code, bool include_roa_policy) {
      signed_transaction trx;
      set_transaction_headers(trx);

      authority owner_auth;
      if( multisig ) {
         // multisig between account's owner key and creators active permission
         owner_auth = authority(2, {key_weight{get_public_key( a, "owner" ), 1}}, {permission_level_weight{{creator, config::active_name}, 1}});
      } else {
         owner_auth =  authority( get_public_key( a, "owner" ) );
      }

      authority active_auth( get_public_key( a, "active" ) );

      auto sort_permissions = []( authority& auth ) {
         std::sort( auth.accounts.begin(), auth.accounts.end(),
                    []( const permission_level_weight& lhs, const permission_level_weight& rhs ) {
                        return lhs.permission < rhs.permission;
                    }
                  );
      };

      if( include_code ) {
         FC_ASSERT( owner_auth.threshold <= std::numeric_limits<weight_type>::max(), "threshold is too high" );
         FC_ASSERT( active_auth.threshold <= std::numeric_limits<weight_type>::max(), "threshold is too high" );
         owner_auth.accounts.push_back( permission_level_weight{ {a, config::sysio_code_name},
                                                                 static_cast<weight_type>(owner_auth.threshold) } );
         sort_permissions(owner_auth);
         active_auth.accounts.push_back( permission_level_weight{ {a, config::sysio_code_name},
                                                                  static_cast<weight_type>(active_auth.threshold) } );
         sort_permissions(active_auth);
      }

      trx.actions.emplace_back( vector<permission_level>{{creator,config::active_name}},
                                newaccount{
                                   .creator  = creator,
                                   .name     = a,
                                   .owner    = owner_auth,
                                   .active   = active_auth,
                                });

      include_roa_policy &= has_roa;
      include_roa_policy &= a.prefix() != config::system_account_name;
      if( include_roa_policy ) {
         trx.actions.emplace_back(get_action(config::roa_account_name, "addpolicy"_n,
                                             vector<permission_level>{
                                                {NODE_DADDY, config::active_name},
                                             },
                                             mutable_variant_object()
                                             ("owner", a)
                                             ("issuer", NODE_DADDY)
                                             ("net_weight", "0.0010 SYS")
                                             ("cpu_weight", "0.0010 SYS")
                                             ("ram_weight", "2.0000 SYS")
                                             ("network_gen", 0)
                                             ("time_block", 0)
         ));
      }

      set_transaction_headers(trx);
      trx.sign( get_private_key( creator, "active" ), control->get_chain_id()  );
      if (include_roa_policy) {
         trx.sign( get_private_key( NODE_DADDY, "active" ), control->get_chain_id() );
      }
      return push_transaction( trx );
   }

   transaction_trace_ptr base_tester::register_node_owner(account_name owner, uint32_t tier) {
      signed_transaction trx;
      set_transaction_headers(trx);

      wlog("Registering node owner: ${owner} with tier: ${tier}", ("owner", owner)("tier", tier));

      trx.actions.emplace_back(get_action(config::roa_account_name, "regnodeowner"_n,
                                          vector<permission_level>{{config::roa_account_name, config::active_name}},
                                          mutable_variant_object()
                                          ("owner", owner)
                                          ("tier", tier)
      ));
      set_transaction_headers(trx);
      trx.sign(get_private_key(config::roa_account_name, "active"), control->get_chain_id());
      return push_transaction(trx);
   }

   transaction_trace_ptr base_tester::add_roa_policy(account_name issuer, account_name owner, string net_weight,
                                                     string cpu_weight, string ram_weight, int64_t network_gen,
                                                     uint32_t time_block) {
      signed_transaction trx;
      set_transaction_headers(trx);

      trx.actions.emplace_back(get_action(config::roa_account_name, "addpolicy"_n,
                                          vector<permission_level>{{issuer, config::active_name}},
                                          fc::mutable_variant_object
                                          ("owner", owner)
                                          ("issuer", issuer)
                                          ("net_weight", net_weight)
                                          ("cpu_weight", cpu_weight)
                                          ("ram_weight", ram_weight)
                                          ("network_gen", network_gen)
                                          ("time_block", time_block)
      ));

      set_transaction_headers(trx);
      trx.sign(get_private_key(issuer, "active"), control->get_chain_id());
      return push_transaction(trx);
   }

   transaction_trace_ptr base_tester::expand_roa_policy(account_name issuer, account_name owner, string net_weight,
                                                     string cpu_weight, string ram_weight, int64_t network_gen) {
      signed_transaction trx;
      set_transaction_headers(trx);

      trx.actions.emplace_back(get_action("sysio.roa"_n, "expandpolicy"_n,
                                          vector<permission_level>{{issuer, config::active_name}},
                                          fc::mutable_variant_object
                                          ("owner", owner)
                                          ("issuer", issuer)
                                          ("net_weight", net_weight)
                                          ("cpu_weight", cpu_weight)
                                          ("ram_weight", ram_weight)
                                          ("network_gen", network_gen)
      ));

      set_transaction_headers(trx);
      trx.sign(get_private_key(issuer, "active"), control->get_chain_id());
      return push_transaction(trx);
   }

   transaction_trace_ptr base_tester::reduce_roa_policy(account_name issuer, account_name owner, string net_weight,
                                                     string cpu_weight, string ram_weight, int64_t network_gen) {
      signed_transaction trx;
      set_transaction_headers(trx);

      trx.actions.emplace_back(get_action("sysio.roa"_n, "reducepolicy"_n,
                                          vector<permission_level>{{issuer, config::active_name}},
                                          fc::mutable_variant_object
                                          ("owner", owner)
                                          ("issuer", issuer)
                                          ("net_weight", net_weight)
                                          ("cpu_weight", cpu_weight)
                                          ("ram_weight", ram_weight)
                                          ("network_gen", network_gen)
      ));

      set_transaction_headers(trx);
      trx.sign(get_private_key(issuer, "active"), control->get_chain_id());
      return push_transaction(trx);
   }

   transaction_trace_ptr base_tester::push_transaction( packed_transaction& trx,
                                                        fc::time_point deadline,
                                                        uint32_t billed_cpu_time_us
                                                      )
   { try {
      if( !control->is_building_block() )
         _start_block(control->head_block_time() + fc::microseconds(config::block_interval_us));

      auto ptrx = std::make_shared<packed_transaction>(trx);
      auto time_limit = deadline == fc::time_point::maximum() ?
            fc::microseconds::maximum() :
            fc::microseconds( deadline - fc::time_point::now() );
      auto fut = transaction_metadata::start_recover_keys( ptrx, control->get_thread_pool(), control->get_chain_id(), time_limit, transaction_metadata::trx_type::input );
      auto r = control->push_transaction( fut.get(), deadline, fc::microseconds::maximum(), billed_cpu_time_us, billed_cpu_time_us > 0, 0 );
      if( r->except_ptr ) std::rethrow_exception( r->except_ptr );
      if( r->except ) throw *r->except;
      return r;
   } FC_RETHROW_EXCEPTIONS( warn, "transaction_header: ${header}", ("header", transaction_header(trx.get_transaction()) )) }

   transaction_trace_ptr base_tester::push_transaction( signed_transaction& trx,
                                                        fc::time_point deadline,
                                                        uint32_t billed_cpu_time_us,
                                                        bool no_throw,
                                                        transaction_metadata::trx_type trx_type
                                                      )
   { try {
      if( !control->is_building_block() )
         _start_block(control->head_block_time() + fc::microseconds(config::block_interval_us));
      auto c = packed_transaction::compression_type::none;

      if( fc::raw::pack_size(trx) > 1000 ) {
         c = packed_transaction::compression_type::zlib;
      }

      auto time_limit = deadline == fc::time_point::maximum() ?
            fc::microseconds::maximum() :
            fc::microseconds( deadline - fc::time_point::now() );
      auto ptrx = std::make_shared<packed_transaction>( trx, c );
      auto fut = transaction_metadata::start_recover_keys( ptrx, control->get_thread_pool(), control->get_chain_id(), time_limit, trx_type );
      auto r = control->push_transaction( fut.get(), deadline, fc::microseconds::maximum(), billed_cpu_time_us, billed_cpu_time_us > 0, 0 );
      if (no_throw) return r;
      if( r->except_ptr ) std::rethrow_exception( r->except_ptr );
      if( r->except)  throw *r->except;
      return r;
   } FC_RETHROW_EXCEPTIONS( warn, "transaction_header: ${header}, billed_cpu_time_us: ${billed}",
                            ("header", transaction_header(trx) ) ("billed", billed_cpu_time_us))
   }

   typename base_tester::action_result base_tester::push_action(action&& act, uint64_t authorizer) {
      signed_transaction trx;
      if (authorizer) {
         act.authorization = vector<permission_level>{{account_name(authorizer), config::active_name}, {account_name(authorizer), config::sysio_payer_name}};
      }
      trx.actions.emplace_back(std::move(act));
      set_transaction_headers(trx);
      if (authorizer) {
         trx.sign(get_private_key(account_name(authorizer), "active"), control->get_chain_id());
      }
      try {
         push_transaction(trx);
      } catch (const fc::exception& ex) {
         edump((ex.to_detail_string()));
         return error(ex.top_message()); // top_message() is assumed by many tests; otherwise they fail
         //return error(ex.to_detail_string());
      }
      produce_block();
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      return success();
   }

   typename base_tester::action_result base_tester::push_paid_action(action&& act, uint64_t authorizer) {
      signed_transaction trx;
      if (authorizer) {
         act.authorization = vector<permission_level>{{account_name(authorizer), config::active_name},
            {account_name(authorizer), config::sysio_payer_name}};
      }
      trx.actions.emplace_back(std::move(act));
      set_transaction_headers(trx);
      if (authorizer) {
         trx.sign(get_private_key(account_name(authorizer), "active"), control->get_chain_id());
      }
      try {
         push_transaction(trx);
      } catch (const fc::exception& ex) {
         edump((ex.to_detail_string()));
         return error(ex.top_message()); // top_message() is assumed by many tests; otherwise they fail
         //return error(ex.to_detail_string());
      }
      produce_block();
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      return success();
   }

   transaction_trace_ptr base_tester::push_action( const account_name& code,
                                                   const action_name& acttype,
                                                   const account_name& actor,
                                                   const variant_object& data,
                                                   uint32_t expiration,
                                                   uint32_t delay_sec
                                                 )

   {
      vector<permission_level> auths;
      auths.push_back( permission_level{actor, config::active_name} );
      return push_action( code, acttype, auths, data, expiration, delay_sec );
   }

   transaction_trace_ptr base_tester::push_action( const account_name& code,
                                                   const action_name& acttype,
                                                   const vector<account_name>& actors,
                                                   const variant_object& data,
                                                   uint32_t expiration,
                                                   uint32_t delay_sec
                                                 )

   {
      vector<permission_level> auths;
      for (const auto& actor : actors) {
         auths.push_back( permission_level{actor, config::active_name} );
      }
      return push_action( code, acttype, auths, data, expiration, delay_sec );
   }

   transaction_trace_ptr base_tester::push_action( const account_name& code,
                                                   const action_name& acttype,
                                                   const vector<permission_level>& auths,
                                                   const variant_object& data,
                                                   uint32_t expiration,
                                                   uint32_t delay_sec
                                                 )

   { try {
      signed_transaction trx;
      trx.actions.emplace_back( get_action( code, acttype, auths, data ) );
      set_transaction_headers( trx, expiration, delay_sec );
      for (const auto& auth : auths) {
         if (auth.permission == config::sysio_payer_name) {
            continue;
         }
         trx.sign( get_private_key( auth.actor, auth.permission.to_string() ), control->get_chain_id() );
      }

      return push_transaction( trx );
   } FC_CAPTURE_AND_RETHROW( (code)(acttype)(auths)(data)(expiration)(delay_sec) ) }

   action base_tester::get_action( account_name code, action_name acttype, vector<permission_level> auths,
                                   const variant_object& data )const { try {
      const auto& acnt = control->get_account(code);
      auto abi = acnt.get_abi();
      chain::abi_serializer abis(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));

      string action_type_name = abis.get_action_type(acttype);
      FC_ASSERT( action_type_name != string(), "unknown action type ${a}", ("a",acttype) );


      action act;
      act.account = code;
      act.name = acttype;
      act.authorization = auths;
      act.data = abis.variant_to_binary(action_type_name, data, abi_serializer::create_yield_function( abi_serializer_max_time ));
      return act;
   } FC_CAPTURE_AND_RETHROW() }

   transaction_trace_ptr base_tester::push_reqauth( account_name from, const vector<permission_level>& auths, const vector<private_key_type>& keys ) {
      fc::variant pretty_trx = fc::mutable_variant_object()
         ("actions", fc::variants({
            fc::mutable_variant_object()
               ("account", name(config::system_account_name))
               ("name", "reqauth")
               ("authorization", auths)
               ("data", fc::mutable_variant_object()
                  ("from", from)
               )
            })
        );

      signed_transaction trx;
      abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer::create_yield_function( abi_serializer_max_time ));
      set_transaction_headers(trx);
      for(auto iter = keys.begin(); iter != keys.end(); iter++)
         trx.sign( *iter, control->get_chain_id() );
      return push_transaction( trx );
   }


    transaction_trace_ptr base_tester::push_reqauth(account_name from, string role, bool multi_sig) {
        if (!multi_sig) {
            return push_reqauth(from, vector<permission_level>{{from, config::owner_name}},
                                        {get_private_key(from, role)});
        } else {
            return push_reqauth(from, vector<permission_level>{{from, config::owner_name}},
                                        {get_private_key(from, role), get_private_key( config::system_account_name, "active" )} );
        }
    }


   transaction_trace_ptr base_tester::push_dummy(account_name from, const string& v, uint32_t billed_cpu_time_us) {
      // use reqauth for a normal action, this could be anything
      fc::variant pretty_trx = fc::mutable_variant_object()
         ("actions", fc::variants({
            fc::mutable_variant_object()
               ("account", name(config::system_account_name))
               ("name", "reqauth")
               ("authorization", fc::variants({
                  fc::mutable_variant_object()
                     ("actor", from)
                     ("permission", name(config::active_name))
               }))
               ("data", fc::mutable_variant_object()
                  ("from", from)
               )
            })
        )
        // lets also push a context free action, the multi chain test will then also include a context free action
        ("context_free_actions", fc::variants({
            fc::mutable_variant_object()
               ("account", name(config::null_account_name))
               ("name", "nonce")
               ("data", fc::raw::pack(v))
            })
         );

      signed_transaction trx;
      abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer::create_yield_function( abi_serializer_max_time ));
      set_transaction_headers(trx);

      trx.sign( get_private_key( from, "active" ), control->get_chain_id() );
      return push_transaction( trx, fc::time_point::maximum(), billed_cpu_time_us );
   }


   transaction_trace_ptr base_tester::transfer( account_name from, account_name to, string amount, string memo, account_name currency ) {
      return transfer( from, to, asset::from_string(amount), memo, currency );
   }


   transaction_trace_ptr base_tester::transfer( account_name from, account_name to, asset amount, string memo, account_name currency ) {
      fc::variant pretty_trx = fc::mutable_variant_object()
         ("actions", fc::variants({
            fc::mutable_variant_object()
               ("account", currency)
               ("name", "transfer")
               ("authorization", fc::variants({
                  fc::mutable_variant_object()
                     ("actor", from)
                     ("permission", name(config::active_name))
               }))
               ("data", fc::mutable_variant_object()
                  ("from", from)
                  ("to", to)
                  ("quantity", amount)
                  ("memo", memo)
               )
            })
         );

      signed_transaction trx;
      abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer::create_yield_function( abi_serializer_max_time ));
      set_transaction_headers(trx);

      trx.sign( get_private_key( from, name(config::active_name).to_string() ), control->get_chain_id()  );
      return push_transaction( trx );
   }


   transaction_trace_ptr base_tester::issue( account_name to, string amount, account_name currency, string memo ) {
      fc::variant pretty_trx = fc::mutable_variant_object()
         ("actions", fc::variants({
            fc::mutable_variant_object()
               ("account", currency)
               ("name", "issue")
               ("authorization", fc::variants({
                  fc::mutable_variant_object()
                     ("actor", currency )
                     ("permission", name(config::active_name))
               }))
               ("data", fc::mutable_variant_object()
                  ("to", to)
                  ("quantity", amount)
                  ("memo", memo)
               )
            })
         );

      signed_transaction trx;
      abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer::create_yield_function( abi_serializer_max_time ));
      set_transaction_headers(trx);

      trx.sign( get_private_key( currency, name(config::active_name).to_string() ), control->get_chain_id()  );
      return push_transaction( trx );
   }


   void base_tester::link_authority( account_name account, account_name code, permission_name req, action_name type ) {
      signed_transaction trx;

      trx.actions.emplace_back( vector<permission_level>{{account, config::active_name}},
                                linkauth(account, code, type, req));
      set_transaction_headers(trx);
      trx.sign( get_private_key( account, "active" ), control->get_chain_id()  );

      push_transaction( trx );
   }


   void base_tester::unlink_authority( account_name account, account_name code, action_name type ) {
      signed_transaction trx;

      trx.actions.emplace_back( vector<permission_level>{{account, config::active_name}},
                                unlinkauth(account, code, type ));
      set_transaction_headers(trx);
      trx.sign( get_private_key( account, "active" ), control->get_chain_id()  );

      push_transaction( trx );
   }


   void base_tester::set_authority( account_name account,
                               permission_name perm,
                               authority auth,
                               permission_name parent,
                               const vector<permission_level>& auths,
                               const vector<private_key_type>& keys) { try {
      signed_transaction trx;

      trx.actions.emplace_back( auths,
                                updateauth{
                                   .account    = account,
                                   .permission = perm,
                                   .parent     = parent,
                                   .auth       = std::move(auth),
                                });

         set_transaction_headers(trx);
      for (const auto& key: keys) {
         trx.sign( key, control->get_chain_id()  );
      }

      push_transaction( trx );
   } FC_CAPTURE_AND_RETHROW( (account)(perm)(auth)(parent) ) }


   void base_tester::set_authority( account_name account,
                                    permission_name perm,
                                    authority auth,
                                    permission_name parent) {
      set_authority(account, perm, auth, parent, { { account, config::owner_name } }, { get_private_key( account, "owner" ) });
   }



   void base_tester::delete_authority( account_name account,
                                    permission_name perm,
                                    const vector<permission_level>& auths,
                                    const vector<private_key_type>& keys ) { try {
         signed_transaction trx;
         trx.actions.emplace_back( auths,
                                   deleteauth(account, perm) );

         set_transaction_headers(trx);
         for (const auto& key: keys) {
            trx.sign( key, control->get_chain_id()  );
         }

         push_transaction( trx );
      } FC_CAPTURE_AND_RETHROW( (account)(perm) ) }


   void base_tester::delete_authority( account_name account,
                                       permission_name perm ) {
      delete_authority(account, perm, { permission_level{ account, config::owner_name } }, { get_private_key( account, "owner" ) });
   }

   void base_tester::set_contract( account_name account, const vector<uint8_t>& wasm, const std::string& abi_json ) {
      auto abi = fc::json::from_string(abi_json).template as<abi_def>();

      signed_transaction trx;
      trx.actions.emplace_back( vector<permission_level>{{account, config::active_name}},
         setcode{
            .account    = account,
            .vmtype     = 0,
            .vmversion  = 0,
            .code       = bytes(wasm.begin(), wasm.end())
         });
      trx.actions.emplace_back( vector<permission_level>{{account,config::active_name}},
         setabi{
            .account    = account,
            .abi        = fc::raw::pack(abi)
         });
      set_transaction_headers(trx);
      trx.sign( get_private_key(account, "active"), control->get_chain_id());
      push_transaction(trx);
   }


   void base_tester::set_code( account_name account, const char* wast, const private_key_type* signer  ) try {
      set_code(account, wast_to_wasm(wast), signer);
   } FC_CAPTURE_AND_RETHROW( (account) )


   void base_tester::set_code( account_name account, const vector<uint8_t> wasm, const private_key_type* signer ) try {
      signed_transaction trx;
      trx.actions.emplace_back( vector<permission_level>{{account,config::active_name}},
                                setcode{
                                   .account    = account,
                                   .vmtype     = 0,
                                   .vmversion  = 0,
                                   .code       = bytes(wasm.begin(), wasm.end())
                                });

      set_transaction_headers(trx);
      if( signer ) {
         trx.sign( *signer, control->get_chain_id()  );
      } else {
         trx.sign( get_private_key( account, "active" ), control->get_chain_id()  );
      }
      push_transaction( trx );
   } FC_CAPTURE_AND_RETHROW( (account) )


   void base_tester::set_abi( account_name account, const std::string& abi_json, const private_key_type* signer ) {
      auto abi = fc::json::from_string(abi_json).template as<abi_def>();
      signed_transaction trx;
      trx.actions.emplace_back( vector<permission_level>{{account,config::active_name}},
                                setabi{
                                   .account    = account,
                                   .abi        = fc::raw::pack(abi)
                                });

      set_transaction_headers(trx);
      if( signer ) {
         trx.sign( *signer, control->get_chain_id()  );
      } else {
         trx.sign( get_private_key( account, "active" ), control->get_chain_id()  );
      }
      push_transaction( trx );
   }

   bool base_tester::is_code_cached( sysio::chain::account_name name ) const {
      const auto& db  = control->db();
      const account_metadata_object* receiver_account = &db.template get<account_metadata_object,by_name>( name );
      if ( receiver_account->code_hash == digest_type() ) return false;
      return control->get_wasm_interface().is_code_cached( receiver_account->code_hash, receiver_account->vm_type, receiver_account->vm_version );
   }


   bool base_tester::chain_has_transaction( const transaction_id_type& txid ) const {
      return chain_transactions.count(txid) != 0;
   }


   const transaction_receipt& base_tester::get_transaction_receipt( const transaction_id_type& txid ) const {
      return chain_transactions.at(txid);
   }

   /**
    *  Reads balance as stored by generic_currency contract
    */

   asset base_tester::get_currency_balance( const account_name& code,
                                       const symbol&       asset_symbol,
                                       const account_name& account ) const {
      const auto& db  = control->db();
      const auto* tbl = db.template find<table_id_object, by_code_scope_table>(boost::make_tuple(code, account, "accounts"_n));
      share_type result = 0;

      // the balance is implied to be 0 if either the table or row does not exist
      if (tbl) {
         const auto *obj = db.template find<key_value_object, by_scope_primary>(boost::make_tuple(tbl->id, asset_symbol.to_symbol_code().value));
         if (obj) {
            //balance is the first field in the serialization
            fc::datastream<const char *> ds(obj->value.data(), obj->value.size());
            fc::raw::unpack(ds, result);
         }
      }
      return asset(result, asset_symbol);
   }


   vector<char> base_tester::get_row_by_account( name code, name scope, name table, const account_name& act ) const {
      vector<char> data;
      const auto& db = control->db();
      const auto* t_id = db.find<chain::table_id_object, chain::by_code_scope_table>( boost::make_tuple( code, scope, table ) );
      if ( !t_id ) {
         return data;
      }
      //FC_ASSERT( t_id != 0, "object not found" );

      const auto& idx = db.get_index<chain::key_value_index, chain::by_scope_primary>();

      auto itr = idx.lower_bound( boost::make_tuple( t_id->id, act.to_uint64_t() ) );
      if ( itr == idx.end() || itr->t_id != t_id->id || act.to_uint64_t() != itr->primary_key ) {
         return data;
      }

      data.resize( itr->value.size() );
      memcpy( data.data(), itr->value.data(), data.size() );
      return data;
   }


   vector<uint8_t> base_tester::to_uint8_vector(const string& s) {
      vector<uint8_t> v(s.size());
      copy(s.begin(), s.end(), v.begin());
      return v;
   };


   vector<uint8_t> base_tester::to_uint8_vector(uint64_t x) {
      vector<uint8_t> v(sizeof(x));
      *reinterpret_cast<uint64_t*>(v.data()) = x;
      return v;
   };


   uint64_t base_tester::to_uint64(fc::variant x) {
      vector<uint8_t> blob;
      fc::from_variant<uint8_t>(x, blob);
      FC_ASSERT(8 == blob.size());
      return *reinterpret_cast<uint64_t*>(blob.data());
   }


   string base_tester::to_string(fc::variant x) {
      vector<uint8_t> v;
      fc::from_variant<uint8_t>(x, v);
      string s(v.size(), 0);
      copy(v.begin(), v.end(), s.begin());
      return s;
   }


   void base_tester::sync_with(base_tester& other) {
      // Already in sync?
      if (control->head_block_id() == other.control->head_block_id())
         return;
      // If other has a longer chain than we do, sync it to us first
      if (control->head_block_num() < other.control->head_block_num())
         return other.sync_with(*this);

      auto sync_dbs = [](base_tester& a, base_tester& b) {
         for( uint32_t i = 1; i <= a.control->head_block_num(); ++i ) {

            auto block = a.control->fetch_block_by_number(i);
            if( block ) { //&& !b.control->is_known_block(block->id()) ) {
               auto bsf = b.control->create_block_state_future( block->calculate_id(), block );
               b.control->abort_block();
               controller::block_report br;
               b.control->push_block(br, bsf.get(), forked_branch_callback{}, trx_meta_cache_lookup{}); //, sysio::chain::validation_steps::created_block);
            }
         }
      };

      sync_dbs(*this, other);
      sync_dbs(other, *this);
   }

   void base_tester::set_before_preactivate_bios_contract() {
      set_code(config::system_account_name, contracts::before_preactivate_sysio_bios_wasm());
      set_abi(config::system_account_name, contracts::before_preactivate_sysio_bios_abi());
   }

   void base_tester::set_before_producer_authority_bios_contract() {
      set_code(config::system_account_name, contracts::before_producer_authority_sysio_bios_wasm());
      set_abi(config::system_account_name, contracts::before_producer_authority_sysio_bios_abi());
   }

   void base_tester::set_bios_contract() {
      set_code(config::system_account_name, contracts::sysio_bios_wasm());
      set_abi(config::system_account_name, contracts::sysio_bios_abi());
   }

   void base_tester::init_roa() {
      // Create the ROA account and mark it as privileged
      create_account(config::roa_account_name, config::system_account_name, false, true, false);
      set_contract(config::roa_account_name, contracts::sysio_roa_wasm(), contracts::sysio_roa_abi().data());
      push_action(config::system_account_name, "setpriv"_n,
                  config::system_account_name,
                  fc::mutable_variant_object()("account", config::roa_account_name)("is_priv", 1)
      );

      // Call sysio.roa activateroa with total_sys of 75496.0000 SYS and bytes_per_unit of 104
      push_action(config::roa_account_name, "activateroa"_n, config::roa_account_name,
                  fc::mutable_variant_object()
                  ("total_sys", asset::from_string("75496.0000 SYS"))
                  ("bytes_per_unit", 104)
      );

      produce_block();

      // Setup default node daddy for easier resource allocation during testing
      create_account(NODE_DADDY, config::system_account_name, false, true, false);
      register_node_owner(NODE_DADDY, 1);
      produce_block();
      has_roa = true;
   }

   vector<producer_authority> base_tester::get_producer_authorities( const vector<account_name>& producer_names )const {
       // Create producer schedule
       vector<producer_authority> schedule;
       for (auto& producer_name: producer_names) {
          schedule.emplace_back(producer_authority{ producer_name, block_signing_authority_v0{1, {{ get_public_key( producer_name, "active" ), 1}} } });
       }
       return schedule;
   }

   transaction_trace_ptr base_tester::set_producers(const vector<account_name>& producer_names) {
      auto schedule = get_producer_authorities( producer_names );

      return set_producer_schedule(schedule);
   }

   transaction_trace_ptr base_tester::set_producer_schedule(const vector<producer_authority>& schedule ) {
      // FC reflection does not create variants that are compatible with ABI 1.1 so we manually translate.
      fc::variants schedule_variant;
      schedule_variant.reserve(schedule.size());
      for( const auto& e: schedule ) {
         schedule_variant.emplace_back(e.get_abi_variant());
      }

      return push_action( config::system_account_name, "setprods"_n, config::system_account_name,
                          fc::mutable_variant_object()("schedule", schedule_variant));

   }

   transaction_trace_ptr base_tester::set_producers_legacy(const vector<account_name>& producer_names) {
      auto schedule = get_producer_authorities( producer_names );
      // down-rank to old version

      vector<legacy::producer_key> legacy_keys;
      legacy_keys.reserve(schedule.size());
      for (const auto &p : schedule) {
         std::visit([&legacy_keys, &p](const auto& auth){
            legacy_keys.emplace_back(legacy::producer_key{p.producer_name, auth.keys.front().key});
         }, p.authority);
      }

      return push_action( config::system_account_name, "setprods"_n, config::system_account_name,
                          fc::mutable_variant_object()("schedule", legacy_keys));

   }


   const table_id_object* base_tester::find_table( name code, name scope, name table ) {
      auto tid = control->db().find<table_id_object, by_code_scope_table>(boost::make_tuple(code, scope, table));
      return tid;
   }

   void base_tester::schedule_protocol_features_wo_preactivation(const vector<digest_type> feature_digests) {
      protocol_features_to_be_activated_wo_preactivation.insert(
         protocol_features_to_be_activated_wo_preactivation.end(),
         feature_digests.begin(),
         feature_digests.end()
      );
   }

   void base_tester::preactivate_protocol_features(const vector<digest_type> feature_digests) {
      for( const auto& feature_digest: feature_digests ) {
         push_action( config::system_account_name, "activate"_n, config::system_account_name,
                      fc::mutable_variant_object()("feature_digest", feature_digest) );
      }
   }

   void base_tester::preactivate_builtin_protocol_features(const std::vector<builtin_protocol_feature_t>& builtins) {
      const auto& pfm = control->get_protocol_feature_manager();
      const auto& pfs = pfm.get_protocol_feature_set();
      const auto current_block_num  =  control->head_block_num() + (control->is_building_block() ? 1 : 0);
      const auto current_block_time = ( control->is_building_block() ? control->pending_block_time()
                                        : control->head_block_time() + fc::milliseconds(config::block_interval_ms) );

      set<digest_type>    preactivation_set;
      vector<digest_type> preactivations;

      std::function<void(const digest_type&)> add_digests =
      [&pfm, &pfs, current_block_num, current_block_time, &preactivation_set, &preactivations, &add_digests]
      ( const digest_type& feature_digest ) {
         const auto& pf = pfs.get_protocol_feature( feature_digest );
         FC_ASSERT( pf.builtin_feature, "called add_digests on a non-builtin protocol feature" );
         if( !pf.enabled || pf.earliest_allowed_activation_time > current_block_time
             || pfm.is_builtin_activated( *pf.builtin_feature, current_block_num ) ) return;

         auto res = preactivation_set.emplace( feature_digest );
         if( !res.second ) return;

         for( const auto& dependency : pf.dependencies ) {
            add_digests( dependency );
         }

         preactivations.emplace_back( feature_digest );
      };

      for( const auto& f : builtins ) {
         auto digest = pfs.get_builtin_digest( f);
         if( !digest ) continue;
         add_digests( *digest );
      }

      preactivate_protocol_features( preactivations );
   }

   std::vector<builtin_protocol_feature_t> base_tester::get_all_builtin_protocol_features() {
      std::vector<builtin_protocol_feature_t> builtins;
      for( const auto& f : builtin_protocol_feature_codenames ) {
         if ( f.first ==  builtin_protocol_feature_t::disable_compression_in_transaction_merkle && !shouldAllowBlockProtocolChanges() ) {
            continue;
         }
         builtins.push_back( f.first );
      }

      // Sorting is here to ensure a consistent order across platforms given that it is
      // pulling the items from an std::unordered_map. This order is important because
      // it impacts the block IDs generated and written out to logs for some tests such
      // as the deep-mind tests.
      std::sort( builtins.begin(), builtins.end() );

      return builtins;
   }

   void base_tester::preactivate_all_builtin_protocol_features() {
      preactivate_builtin_protocol_features( get_all_builtin_protocol_features() );
   }

   void base_tester::preactivate_all_but_disable_deferred_trx() {
      std::vector<builtin_protocol_feature_t> builtins;
      for( const auto& f : get_all_builtin_protocol_features() ) {
         // Before deferred trxs feature is fully disabled, existing tests involving
         // deferred trxs need to be exercised to make sure existing behaviors are
         // maintained. Excluding DISABLE_DEFERRED_TRXS_STAGE_1 and DISABLE_DEFERRED_TRXS_STAGE_2
         // from full protocol feature list such that existing tests can run.
         if( f ==  builtin_protocol_feature_t::disable_deferred_trxs_stage_1 || f  == builtin_protocol_feature_t::disable_deferred_trxs_stage_2 ) {
            continue;
         }
         else if ( f ==  builtin_protocol_feature_t::disable_compression_in_transaction_merkle && !shouldAllowBlockProtocolChanges() ) {
            continue;
         }

         builtins.push_back( f );
      }

      preactivate_builtin_protocol_features( builtins );
   }

   tester::tester(const std::function<void(controller&)>& control_setup, setup_policy policy, db_read_mode read_mode) {
      auto def_conf            = default_config(tempdir);
      def_conf.first.read_mode = read_mode;
      cfg                      = def_conf.first;

      base_tester::open(make_protocol_feature_set(), def_conf.second.compute_chain_id(),
                        [&genesis = def_conf.second, &control = this->control, &control_setup]() {
                           control_setup(*control);
                           control->startup([]() {}, []() { return false; }, genesis);
                        });

      execute_setup_policy(policy);
   }

   bool fc_exception_message_is::operator()( const fc::exception& ex ) {
      auto message = ex.get_log().at( 0 ).get_message();
      bool match = (message == expected);
      if( !match ) {
         BOOST_TEST_MESSAGE( "LOG: expected: " << expected << ", actual: " << message );
      }
      return match;
   }

   bool fc_exception_message_starts_with::operator()( const fc::exception& ex ) {
      auto message = ex.get_log().at( 0 ).get_message();
      bool match = boost::algorithm::starts_with( message, expected );
      if( !match ) {
         BOOST_TEST_MESSAGE( "LOG: expected: " << expected << ", actual: " << message );
      }
      return match;
   }

   bool fc_exception_message_contains::operator()( const fc::exception& ex ) {
      auto message = ex.get_log().at( 0 ).get_message();
      bool match = message.find(expected) != std::string::npos;
      if( !match ) {
         BOOST_TEST_MESSAGE( "LOG: expected: " << expected << ", actual: " << message );
      }
      return match;
   }

   bool fc_assert_exception_message_is::operator()( const fc::assert_exception& ex ) {
      auto message = ex.get_log().at( 0 ).get_message();
      bool match = false;
      auto pos = message.find( ": " );
      if( pos != std::string::npos ) {
         message = message.substr( pos + 2 );
         match = (message == expected);
      }
      if( !match ) {
         BOOST_TEST_MESSAGE( "LOG: expected: " << expected << ", actual: " << message );
      }
      return match;
   }

   bool fc_assert_exception_message_starts_with::operator()( const fc::assert_exception& ex ) {
      auto message = ex.get_log().at( 0 ).get_message();
      bool match = false;
      auto pos = message.find( ": " );
      if( pos != std::string::npos ) {
         message = message.substr( pos + 2 );
         match = boost::algorithm::starts_with( message, expected );
      }
      if( !match ) {
         BOOST_TEST_MESSAGE( "LOG: expected: " << expected << ", actual: " << message );
      }
      return match;
   }

   bool sysio_assert_message_is::operator()( const sysio_assert_message_exception& ex ) {
      auto message = ex.get_log().at( 0 ).get_message();
      bool match = (message == expected);
      if( !match ) {
         BOOST_TEST_MESSAGE( "LOG: expected: " << expected << ", actual: " << message );
      }
      return match;
   }

   bool sysio_assert_message_starts_with::operator()( const sysio_assert_message_exception& ex ) {
      auto message = ex.get_log().at( 0 ).get_message();
      bool match = boost::algorithm::starts_with( message, expected );
      if( !match ) {
         BOOST_TEST_MESSAGE( "LOG: expected: " << expected << ", actual: " << message );
      }
      return match;
   }

   bool sysio_assert_code_is::operator()( const sysio_assert_code_exception& ex ) {
      auto message = ex.get_log().at( 0 ).get_message();
      bool match = (message == expected);
      if( !match ) {
         BOOST_TEST_MESSAGE( "LOG: expected: " << expected << ", actual: " << message );
      }
      return match;
   }

   const std::string mock::webauthn_private_key::_origin = "mock.webauthn.invalid";
   const sha256 mock::webauthn_private_key::_origin_hash = fc::sha256::hash(mock::webauthn_private_key::_origin);
} }  /// sysio::testing

std::ostream& operator<<( std::ostream& osm, const fc::variant& v ) {
   //fc::json::to_stream( osm, v );
   osm << fc::json::to_pretty_string( v );
   return osm;
}

std::ostream& operator<<( std::ostream& osm, const fc::variant_object& v ) {
   osm << fc::variant(v);
   return osm;
}

std::ostream& operator<<( std::ostream& osm, const fc::variant_object::entry& e ) {
   osm << "{ " << e.key() << ": " << e.value() << " }";
   return osm;
}
