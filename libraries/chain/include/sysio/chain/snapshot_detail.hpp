#pragma once
#include <sysio/chain/block_header.hpp>
#include <sysio/chain/incremental_merkle_legacy.hpp>
#include <sysio/chain/protocol_feature_manager.hpp>
#include <sysio/chain/chain_snapshot.hpp>
#include <sysio/chain/block_state.hpp>

namespace sysio::chain::snapshot_detail {

   /**
    *  Snapshot V1 Data structure
    */
   struct snapshot_block_state_v1 {
      // from block_header_state
      block_id_type                                       block_id;
      block_header                                        header;
      protocol_feature_activation_set_ptr                 activated_protocol_features;
      finality_core                                       core;
      finalizer_policy_ptr                                active_finalizer_policy;
      proposer_policy_ptr                                 active_proposer_policy;
      proposer_policy_ptr                                 latest_proposed_proposer_policy;
      proposer_policy_ptr                                 latest_pending_proposer_policy;
      std::vector<std::pair<block_num_type, finalizer_policy_ptr>>   proposed_finalizer_policies;
      std::optional<std::pair<block_num_type, finalizer_policy_ptr>> pending_finalizer_policy;
      finalizer_policy_ptr                                latest_qc_claim_block_active_finalizer_policy;
      uint32_t                                            finalizer_policy_generation;
      digest_type                                         last_pending_finalizer_policy_digest;
      block_timestamp_type                                last_pending_finalizer_policy_start_timestamp;

      // from block_state
      std::optional<valid_t>                              valid;

      snapshot_block_state_v1() = default;

      // When adding a member initialization here also update block_state(snapshot_block_state_v1) constructor
      explicit snapshot_block_state_v1(const block_state& bs)
         : block_id(bs.block_id)
         , header(bs.header)
         , activated_protocol_features(bs.activated_protocol_features)
         , core(bs.core)
         , active_finalizer_policy(bs.active_finalizer_policy)
         , active_proposer_policy(bs.active_proposer_policy)
         , latest_proposed_proposer_policy(bs.latest_proposed_proposer_policy)
         , latest_pending_proposer_policy(bs.latest_pending_proposer_policy)
         , proposed_finalizer_policies(bs.proposed_finalizer_policies)
         , pending_finalizer_policy(bs.pending_finalizer_policy)
         , latest_qc_claim_block_active_finalizer_policy(bs.latest_qc_claim_block_active_finalizer_policy)
         , finalizer_policy_generation(bs.finalizer_policy_generation)
         , last_pending_finalizer_policy_digest(bs.last_pending_finalizer_policy_digest)
         , last_pending_finalizer_policy_start_timestamp(bs.last_pending_finalizer_policy_start_timestamp)
         , valid(bs.valid)
      {}
   };

   struct snapshot_block_state_data_v1 {
      static constexpr uint32_t minimum_version = 1;
      static constexpr uint32_t maximum_version = 1;

      snapshot_block_state_v1               bs;

      snapshot_block_state_data_v1() = default;

      explicit snapshot_block_state_data_v1(const block_state& b)
      {
         bs = snapshot_block_state_v1(b);
      }
   };

}

FC_REFLECT( sysio::chain::snapshot_detail::snapshot_block_state_v1,
            (block_id)
            (header)
            (activated_protocol_features)
            (core)
            (active_finalizer_policy)
            (active_proposer_policy)
            (latest_proposed_proposer_policy)
            (latest_pending_proposer_policy)
            (proposed_finalizer_policies)
            (pending_finalizer_policy)
            (latest_qc_claim_block_active_finalizer_policy)
            (finalizer_policy_generation)
            (last_pending_finalizer_policy_digest)
            (last_pending_finalizer_policy_start_timestamp)
            (valid)
   )

FC_REFLECT( sysio::chain::snapshot_detail::snapshot_block_state_data_v1,
            (bs)
   )
