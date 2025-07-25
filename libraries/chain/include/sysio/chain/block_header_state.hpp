#pragma once
#include <sysio/chain/block_header.hpp>
#include <sysio/chain/incremental_merkle.hpp>
#include <sysio/chain/protocol_feature_manager.hpp>
#include <sysio/chain/chain_snapshot.hpp>
#include <sysio/chain/s_root_extension.hpp>
#include <future>

namespace sysio { namespace chain {

using signer_callback_type = std::function<std::vector<signature_type>(const digest_type&)>;

struct block_header_state;

namespace detail {
   struct block_header_state_common {
      uint32_t                          block_num = 0;
      uint32_t                          dpos_proposed_irreversible_blocknum = 0;
      uint32_t                          dpos_irreversible_blocknum = 0;
      producer_authority_schedule       active_schedule;
      incremental_merkle                blockroot_merkle;
      flat_map<account_name,uint32_t>   producer_to_last_produced;
      flat_map<account_name,uint32_t>   producer_to_last_implied_irb;
      block_signing_authority           valid_block_signing_authority;
      vector<uint8_t>                   confirm_count;
   };

   struct schedule_info {
      uint32_t                          schedule_lib_num = 0; /// last irr block num
      digest_type                       schedule_hash;
      producer_authority_schedule       schedule;
   };

   bool is_builtin_activated( const protocol_feature_activation_set_ptr& pfa,
                              const protocol_feature_set& pfs,
                              builtin_protocol_feature_t feature_codename );
}

struct pending_block_header_state : public detail::block_header_state_common {
   protocol_feature_activation_set_ptr  prev_activated_protocol_features;
   detail::schedule_info                prev_pending_schedule;
   bool                                 was_pending_promoted = false;
   block_id_type                        previous;
   account_name                         producer;
   block_timestamp_type                 timestamp;
   uint32_t                             active_schedule_version = 0;
   uint16_t                             confirmed = 1;

   signed_block_header make_block_header( const checksum256_type& transaction_mroot,
                                          const checksum256_type& action_mroot,
                                          const std::optional<producer_authority_schedule>& new_producers,
                                          vector<digest_type>&& new_protocol_feature_activations,
                                          const protocol_feature_set& pfs,
                                          const chain::deque<s_header>& s_header)const;

   block_header_state  finish_next( const signed_block_header& h,
                                    vector<signature_type>&& additional_signatures,
                                    const protocol_feature_set& pfs,
                                    const std::function<void( block_timestamp_type,
                                                              const flat_set<digest_type>&,
                                                              const vector<digest_type>& )>& validator,
                                    bool skip_validate_signee = false )&&;

   block_header_state  finish_next( signed_block_header& h,
                                    const protocol_feature_set& pfs,
                                    const std::function<void( block_timestamp_type,
                                                              const flat_set<digest_type>&,
                                                              const vector<digest_type>& )>& validator,
                                    const signer_callback_type& signer )&&;

protected:
   block_header_state  _finish_next( const signed_block_header& h,
                                     const protocol_feature_set& pfs,
                                     const std::function<void( block_timestamp_type,
                                                               const flat_set<digest_type>&,
                                                               const vector<digest_type>& )>& validator )&&;
};

/**
 *  @struct block_header_state
 *  @brief defines the minimum state necessary to validate transaction headers
 */
struct block_header_state : public detail::block_header_state_common {
   block_id_type                        id;
   signed_block_header                  header;
   detail::schedule_info                pending_schedule;
   protocol_feature_activation_set_ptr  activated_protocol_features;
   vector<signature_type>               additional_signatures;

   /// this data is redundant with the data stored in header, but it acts as a cache that avoids
   /// duplication of work
   flat_multimap<uint16_t, block_header_extension> header_exts;

   block_header_state() = default;

   explicit block_header_state( detail::block_header_state_common&& base )
   :detail::block_header_state_common( std::move(base) )
   {}

   pending_block_header_state  next( block_timestamp_type when, uint16_t num_prev_blocks_to_confirm )const;

   block_header_state   next( const signed_block_header& h,
                              vector<signature_type>&& additional_signatures,
                              const protocol_feature_set& pfs,
                              const std::function<void( block_timestamp_type,
                                                        const flat_set<digest_type>&,
                                                        const vector<digest_type>& )>& validator,
                              bool skip_validate_signee = false )const;

   bool                 has_pending_producers()const { return pending_schedule.schedule.producers.size(); }
   uint32_t             calc_dpos_last_irreversible( account_name producer_of_next_block )const;

   producer_authority     get_scheduled_producer( block_timestamp_type t )const;
   const block_id_type&   prev()const { return header.previous; }
   digest_type            sig_digest()const;
   void                   sign( const signer_callback_type& signer );
   void                   verify_signee()const;

   const vector<digest_type>& get_new_protocol_feature_activations()const;
};

using block_header_state_ptr = std::shared_ptr<block_header_state>;

} } /// namespace sysio::chain

FC_REFLECT( sysio::chain::detail::block_header_state_common,
            (block_num)
            (dpos_proposed_irreversible_blocknum)
            (dpos_irreversible_blocknum)
            (active_schedule)
            (blockroot_merkle)
            (producer_to_last_produced)
            (producer_to_last_implied_irb)
            (valid_block_signing_authority)
            (confirm_count)
)

FC_REFLECT( sysio::chain::detail::schedule_info,
            (schedule_lib_num)
            (schedule_hash)
            (schedule)
)

FC_REFLECT_DERIVED(  sysio::chain::block_header_state, (sysio::chain::detail::block_header_state_common),
                     (id)
                     (header)
                     (pending_schedule)
                     (activated_protocol_features)
                     (additional_signatures)
)
