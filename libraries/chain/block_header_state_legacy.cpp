#include <sysio/chain/block_header_state_legacy.hpp>
#include <sysio/chain/block_header_state_utils.hpp>
#include <sysio/chain/snapshot_detail.hpp>
#include <sysio/chain/exceptions.hpp>
#include <limits>

namespace sysio::chain {

   uint32_t block_header_state_legacy::calc_dpos_last_irreversible( account_name producer_of_next_block )const {
      vector<uint32_t> blocknums; blocknums.reserve( producer_to_last_implied_irb.size() );
      for( auto& i : producer_to_last_implied_irb ) {
         blocknums.push_back( (i.first == producer_of_next_block) ? dpos_proposed_irreversible_blocknum : i.second);
      }
      /// 2/3 must be greater, so if I go 1/3 into the list sorted from low to high, then 2/3 are greater

      if( blocknums.size() == 0 ) return 0;

      std::size_t index = (blocknums.size()-1) / 3;
      std::nth_element( blocknums.begin(),  blocknums.begin() + index, blocknums.end() );
      return blocknums[ index ];
   }

   const producer_authority& block_header_state_legacy::get_scheduled_producer( block_timestamp_type t ) const {
      return detail::get_scheduled_producer(active_schedule.producers, t);
   }

   pending_block_header_state_legacy  block_header_state_legacy::next( block_timestamp_type when,
                                                                       uint16_t num_prev_blocks_to_confirm )const
   {
      pending_block_header_state_legacy result;

      if( when != block_timestamp_type() ) {
        SYS_ASSERT( when > header.timestamp, block_validate_exception, "next block must be in the future" );
      } else {
        (when = header.timestamp).slot++;
      }

      const auto& proauth = get_scheduled_producer(when);

      auto itr = producer_to_last_produced.find( proauth.producer_name );
      if( itr != producer_to_last_produced.end() ) {
        SYS_ASSERT( itr->second < (block_num+1) - num_prev_blocks_to_confirm, producer_double_confirm,
                    "producer ${prod} double-confirming known range",
                    ("prod", proauth.producer_name)("num", block_num+1)
                    ("confirmed", num_prev_blocks_to_confirm)("last_produced", itr->second) );
      }

      result.block_num                                       = block_num + 1;
      result.previous                                        = id;
      result.timestamp                                       = when;
      result.confirmed                                       = num_prev_blocks_to_confirm;
      result.active_schedule_version                         = active_schedule.version;
      result.prev_activated_protocol_features                = activated_protocol_features;

      result.valid_block_signing_authority                   = proauth.authority;
      result.producer                                        = proauth.producer_name;

      result.blockroot_merkle = blockroot_merkle;
      result.blockroot_merkle.append( id );

      /// grow the confirmed count
      static_assert(std::numeric_limits<uint8_t>::max() >= (config::max_producers * 2 / 3) + 1, "8bit confirmations may not be able to hold all of the needed confirmations");

      // This uses the previous block active_schedule because thats the "schedule" that signs and therefore confirms _this_ block
      auto num_active_producers = active_schedule.producers.size();
      uint32_t required_confs = (uint32_t)(num_active_producers * 2 / 3) + 1;

      if( confirm_count.size() < config::maximum_tracked_dpos_confirmations ) {
         result.confirm_count.reserve( confirm_count.size() + 1 );
         result.confirm_count  = confirm_count;
         result.confirm_count.resize( confirm_count.size() + 1 );
         result.confirm_count.back() = (uint8_t)required_confs;
      } else {
         result.confirm_count.resize( confirm_count.size() );
         memcpy( &result.confirm_count[0], &confirm_count[1], confirm_count.size() - 1 );
         result.confirm_count.back() = (uint8_t)required_confs;
      }

      auto new_dpos_proposed_irreversible_blocknum = dpos_proposed_irreversible_blocknum;

      int32_t i = (int32_t)(result.confirm_count.size() - 1);
      uint32_t blocks_to_confirm = num_prev_blocks_to_confirm + 1; /// confirm the head block too
      while( i >= 0 && blocks_to_confirm ) {
        --result.confirm_count[i];
        //idump((confirm_count[i]));
        if( result.confirm_count[i] == 0 )
        {
            uint32_t block_num_for_i = result.block_num - (uint32_t)(result.confirm_count.size() - 1 - i);
            new_dpos_proposed_irreversible_blocknum = block_num_for_i;
            //idump((dpos2_lib)(block_num)(dpos_irreversible_blocknum));

            if (i == static_cast<int32_t>(result.confirm_count.size() - 1)) {
               result.confirm_count.resize(0);
            } else {
               memmove( &result.confirm_count[0], &result.confirm_count[i + 1], result.confirm_count.size() - i  - 1);
               result.confirm_count.resize( result.confirm_count.size() - i - 1 );
            }

            break;
        }
        --i;
        --blocks_to_confirm;
      }

      result.dpos_proposed_irreversible_blocknum   = new_dpos_proposed_irreversible_blocknum;
      result.dpos_irreversible_blocknum            = calc_dpos_last_irreversible( proauth.producer_name );

      result.prev_pending_schedule                 = pending_schedule;

      if( pending_schedule.schedule.producers.size() &&
          result.dpos_irreversible_blocknum >= pending_schedule.schedule_lib_num )
      {
         result.active_schedule = pending_schedule.schedule;

         flat_map<account_name,uint32_t> new_producer_to_last_produced;

         for( const auto& pro : result.active_schedule.producers ) {
            if( pro.producer_name == proauth.producer_name ) {
               new_producer_to_last_produced[pro.producer_name] = result.block_num;
            } else {
               auto existing = producer_to_last_produced.find( pro.producer_name );
               if( existing != producer_to_last_produced.end() ) {
                  new_producer_to_last_produced[pro.producer_name] = existing->second;
               } else {
                  new_producer_to_last_produced[pro.producer_name] = result.dpos_irreversible_blocknum;
               }
            }
         }
         new_producer_to_last_produced[proauth.producer_name] = result.block_num;

         result.producer_to_last_produced = std::move( new_producer_to_last_produced );

         flat_map<account_name,uint32_t> new_producer_to_last_implied_irb;

         for( const auto& pro : result.active_schedule.producers ) {
            if( pro.producer_name == proauth.producer_name ) {
               new_producer_to_last_implied_irb[pro.producer_name] = dpos_proposed_irreversible_blocknum;
            } else {
               auto existing = producer_to_last_implied_irb.find( pro.producer_name );
               if( existing != producer_to_last_implied_irb.end() ) {
                  new_producer_to_last_implied_irb[pro.producer_name] = existing->second;
               } else {
                  new_producer_to_last_implied_irb[pro.producer_name] = result.dpos_irreversible_blocknum;
               }
            }
         }

         result.producer_to_last_implied_irb = std::move( new_producer_to_last_implied_irb );

         result.was_pending_promoted = true;
      } else {
         result.active_schedule                  = active_schedule;
         result.producer_to_last_produced        = producer_to_last_produced;
         result.producer_to_last_produced[proauth.producer_name] = result.block_num;
         result.producer_to_last_implied_irb     = producer_to_last_implied_irb;
         result.producer_to_last_implied_irb[proauth.producer_name] = dpos_proposed_irreversible_blocknum;
      }

      if (auto it = header_exts.find(finality_extension::extension_id()); it != header_exts.end()) { // transition to savanna has started
         const auto& f_ext = std::get<finality_extension>(it->second);
         // copy over qc_claim from IF Genesis Block
         result.qc_claim = f_ext.qc_claim;
      }

      return result;
   }

   std::optional<block_num_type> pending_block_header_state_legacy::savanna_genesis_block_num() const {
      if (qc_claim) {
         return std::optional<block_num_type>{qc_claim->block_num};
      }
      return {};
   }

   signed_block_header pending_block_header_state_legacy::make_block_header(
                                                      const checksum256_type& transaction_mroot,
                                                      const checksum256_type& action_mroot,
                                                      const std::optional<producer_authority_schedule>& new_producers,
                                                      std::optional<finalizer_policy>&& new_finalizer_policy,
                                                      vector<digest_type>&& new_protocol_feature_activations,
                                                      const protocol_feature_set& pfs,
                                                      const chain::deque<s_header>& s_header
   )const
   {
      signed_block_header h;

      h.timestamp         = timestamp;
      h.producer          = producer;
      h.confirmed         = confirmed;
      h.previous          = previous;
      h.transaction_mroot = transaction_mroot;
      h.action_mroot      = action_mroot;
      h.schedule_version  = active_schedule_version;

      if( new_protocol_feature_activations.size() > 0 ) {
         emplace_extension(
               h.header_extensions,
               protocol_feature_activation::extension_id(),
               fc::raw::pack( protocol_feature_activation{ .protocol_features=std::move(new_protocol_feature_activations) } )
         );
      }

      if (new_producers) {
         // add the header extension to update the block schedule
         emplace_extension(
               h.header_extensions,
               producer_schedule_change_extension::extension_id(),
               fc::raw::pack( producer_schedule_change_extension( *new_producers ) )
         );
      }

      // Add s_root_extensions to header extensions if present & relevant
      for (const auto& header : s_header) {
         emplace_extension(
            h.header_extensions,
            s_root_extension::extension_id(),
            fc::raw::pack( s_root_extension ( header ))
         );
      }

      if (new_finalizer_policy) {
         assert(new_finalizer_policy->generation == 1); // only allowed to be set once
         // set current block_num as qc_claim.last_qc_block_num in the IF extension
         qc_claim_t initial_if_claim { .block_num = block_num,
                                       .is_strong_qc = false };
         finalizer_policy no_policy;
         auto new_fin_policy_diff = no_policy.create_diff(*new_finalizer_policy);
         emplace_extension(h.header_extensions, finality_extension::extension_id(),
                           fc::raw::pack(finality_extension{ initial_if_claim, std::move(new_fin_policy_diff), {} }));
      } else if (qc_claim) {
         emplace_extension(h.header_extensions, finality_extension::extension_id(),
                           fc::raw::pack(finality_extension{ *qc_claim, {}, {} }));
      }

      return h;
   }

   block_header_state_legacy pending_block_header_state_legacy::_finish_next(
                                 const signed_block_header& h,
                                 const protocol_feature_set& pfs,
                                 validator_t& validator

   )&&
   {
      SYS_ASSERT( h.timestamp == timestamp, block_validate_exception, "timestamp mismatch" );
      SYS_ASSERT( h.previous == previous, unlinkable_block_exception, "previous mismatch ${p} != ${id}", ("p", h.previous)("id", previous) );
      SYS_ASSERT( h.confirmed == confirmed, block_validate_exception, "confirmed mismatch" );
      SYS_ASSERT( h.producer == producer, wrong_producer, "wrong producer specified" );
      SYS_ASSERT( h.schedule_version == active_schedule_version, producer_schedule_exception, "schedule_version in signed block is corrupted" );

      auto exts = h.validate_and_extract_header_extensions();

      std::optional<producer_authority_schedule> maybe_new_producer_schedule;
      std::optional<digest_type> maybe_new_producer_schedule_hash;

      if( h.not_used ) {
         SYS_ASSERT(false, producer_schedule_exception, "Block header contains legacy producer schedule, required to be empty on wire.network" );
      }

      if (auto it = exts.find(producer_schedule_change_extension::extension_id()); it != exts.end()) {
         SYS_ASSERT( !was_pending_promoted, producer_schedule_exception, "cannot set pending producer schedule in the same block in which pending was promoted to active" );

         const auto& new_producer_schedule = std::get<producer_schedule_change_extension>(it->second);

         SYS_ASSERT( new_producer_schedule.version == active_schedule.version + 1, producer_schedule_exception, "wrong producer schedule version specified" );
         SYS_ASSERT( prev_pending_schedule.schedule.producers.empty(), producer_schedule_exception,
                     "cannot set new pending producers until last pending is confirmed" );

         maybe_new_producer_schedule_hash.emplace(digest_type::hash(new_producer_schedule));
         maybe_new_producer_schedule.emplace(new_producer_schedule);
      }

      protocol_feature_activation_set_ptr new_activated_protocol_features;
      {  // handle protocol_feature_activation
         if (auto it = exts.find(protocol_feature_activation::extension_id()); it != exts.end()) {
            const auto& new_protocol_features = std::get<protocol_feature_activation>(it->second).protocol_features;
            validator( timestamp, prev_activated_protocol_features->protocol_features, new_protocol_features );

            new_activated_protocol_features =   std::make_shared<protocol_feature_activation_set>(
                                                   *prev_activated_protocol_features,
                                                   new_protocol_features
                                                );
         } else {
            new_activated_protocol_features = std::move( prev_activated_protocol_features );
         }
      }

      auto block_number = block_num;

      block_header_state_legacy result( std::move( *static_cast<detail::block_header_state_legacy_common*>(this) ) );

      result.id       = h.calculate_id();
      result.header   = h;

      result.header_exts = std::move(exts);

      if( maybe_new_producer_schedule ) {
         result.pending_schedule.schedule = std::move(*maybe_new_producer_schedule);
         result.pending_schedule.schedule_hash = *maybe_new_producer_schedule_hash;
         result.pending_schedule.schedule_lib_num    = block_number;
      } else {
         if( was_pending_promoted ) {
            result.pending_schedule.schedule.version = prev_pending_schedule.schedule.version;
         } else {
            result.pending_schedule.schedule         = std::move( prev_pending_schedule.schedule );
         }
         result.pending_schedule.schedule_hash       = prev_pending_schedule.schedule_hash ;
         result.pending_schedule.schedule_lib_num    = prev_pending_schedule.schedule_lib_num;
      }

      result.activated_protocol_features = std::move( new_activated_protocol_features );

      return result;
   }

   block_header_state_legacy pending_block_header_state_legacy::finish_next(
                                 const signed_block_header& h,
                                 vector<signature_type>&& additional_signatures,
                                 const protocol_feature_set& pfs,
                                 validator_t& validator,
                                 bool skip_validate_signee
   )&&
   {
      auto result = std::move(*this)._finish_next( h, pfs, validator );

      if( !additional_signatures.empty() ) {
         result.additional_signatures = std::move(additional_signatures);
      }

      // ASSUMPTION FROM controller_impl::apply_block = all untrusted blocks will have their signatures pre-validated here
      if( !skip_validate_signee ) {
        result.verify_signee( );
      }

      return result;
   }

   block_header_state_legacy pending_block_header_state_legacy::finish_next(
                                 signed_block_header& h,
                                 const protocol_feature_set& pfs,
                                 validator_t& validator,
                                 const signer_callback_type& signer
   )&&
   {
      auto pfa = prev_activated_protocol_features;

      auto result = std::move(*this)._finish_next( h, pfs, validator );
      result.sign( signer );
      h.producer_signature = result.header.producer_signature;

      return result;
   }

   /**
    *  Transitions the current header state into the next header state given the supplied signed block header.
    *
    *  Given a signed block header, generate the expected template based upon the header time,
    *  then validate that the provided header matches the template.
    *
    *  If the header specifies new_producers then apply them accordingly.
    */
   block_header_state_legacy block_header_state_legacy::next(
                        const signed_block_header& h,
                        vector<signature_type>&& additional_signatures,
                        const protocol_feature_set& pfs,
                        validator_t& validator,
                        bool skip_validate_signee )const
   {
      return next( h.timestamp, h.confirmed ).finish_next( h, std::move(additional_signatures), pfs, validator, skip_validate_signee );
   }

   digest_type   block_header_state_legacy::sig_digest()const {
      auto header_bmroot = digest_type::hash( std::make_pair( header.digest(), blockroot_merkle.get_root() ) );
      return digest_type::hash( std::make_pair(header_bmroot, pending_schedule.schedule_hash) );
   }

   void block_header_state_legacy::sign( const signer_callback_type& signer ) {
      auto d = sig_digest();
      auto sigs = signer( d );

      SYS_ASSERT(!sigs.empty(), no_block_signatures, "Signer returned no signatures");
      header.producer_signature = sigs.back();
      sigs.pop_back();

      additional_signatures = std::move(sigs);

      verify_signee();
   }

   void block_header_state_legacy::verify_signee( )const {

      auto num_keys_in_authority = std::visit([](const auto &a){ return a.keys.size(); }, valid_block_signing_authority);
      SYS_ASSERT(1 + additional_signatures.size() <= num_keys_in_authority, wrong_signing_key,
                 "number of block signatures (${num_block_signatures}) exceeds number of keys in block signing authority (${num_keys})",
                 ("num_block_signatures", 1 + additional_signatures.size())
                 ("num_keys", num_keys_in_authority)
                 ("authority", valid_block_signing_authority)
      );

      std::set<public_key_type> keys;
      auto digest = sig_digest();
      keys.emplace(fc::crypto::public_key( header.producer_signature, digest, true ));

      for (const auto& s: additional_signatures) {
         auto res = keys.emplace(s, digest, true);
         SYS_ASSERT(res.second, wrong_signing_key, "block signed by same key twice", ("key", *res.first));
      }

      bool is_satisfied = false;
      size_t relevant_sig_count = 0;

      std::tie(is_satisfied, relevant_sig_count) = producer_authority::keys_satisfy_and_relevant(keys, valid_block_signing_authority);

      SYS_ASSERT(relevant_sig_count == keys.size(), wrong_signing_key,
                 "block signed by unexpected key: ${signing_keys}, expected: ${authority}. ${c} != ${s}",
                 ("signing_keys", keys)("authority", valid_block_signing_authority)("c", relevant_sig_count)("s", keys.size()));

      SYS_ASSERT(is_satisfied, wrong_signing_key,
                 "block signatures do not satisfy the block signing authority",
                 ("signing_keys", keys)("authority", valid_block_signing_authority));
   }

   /**
    *  Reference cannot outlive *this. Assumes header_exts is not mutated after instantiation.
    */
   const vector<digest_type>& block_header_state_legacy::get_new_protocol_feature_activations()const {
      return detail::get_new_protocol_feature_activations(header_exts);
   }


} /// namespace sysio::chain
