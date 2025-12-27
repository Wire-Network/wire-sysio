#pragma once
#include <fc/uint128.hpp>

#include <sysio/chain/types.hpp>
#include <sysio/chain/block_timestamp.hpp>
#include <sysio/chain/chain_config.hpp>
#include <sysio/chain/chain_snapshot.hpp>
#include <sysio/chain/wasm_config.hpp>
#include <sysio/chain/producer_schedule.hpp>
#include <sysio/chain/incremental_merkle.hpp>
#include <sysio/chain/snapshot.hpp>
#include <chainbase/chainbase.hpp>
#include "multi_index_includes.hpp"

namespace sysio { namespace chain {

   /**
    * @class global_property_object
    * @brief Maintains global state information about block producer schedules and chain configuration parameters
    * @ingroup object
    * @ingroup implementation
    */
   class global_property_object : public chainbase::object<global_property_object_type, global_property_object>
   {
      OBJECT_CTOR(global_property_object)

   public:
      id_type                             id;
      chain_config                        configuration;
      chain_id_type                       chain_id;
      wasm_config                         wasm_configuration;

   };


   using global_property_multi_index = chainbase::shared_multi_index_container<
      global_property_object,
      indexed_by<
         ordered_unique<tag<by_id>,
            BOOST_MULTI_INDEX_MEMBER(global_property_object, global_property_object::id_type, id)
         >
      >
   >;

   struct snapshot_global_property_object {
      chain_config                        configuration;
      chain_id_type                       chain_id;
      wasm_config                         wasm_configuration;
   };

   namespace detail {
      template<>
      struct snapshot_row_traits<global_property_object> {
         using value_type = global_property_object;
         using snapshot_type = snapshot_global_property_object;

         static snapshot_global_property_object to_snapshot_row( const global_property_object& value, const chainbase::database& ) {
            return {value.configuration, value.chain_id, value.wasm_configuration};
         }

         static void from_snapshot_row( snapshot_global_property_object&& row, global_property_object& value, chainbase::database& ) {
            value.configuration = row.configuration;
            value.chain_id = row.chain_id;
            value.wasm_configuration = row.wasm_configuration;
         }
      };
   }

   /**
    * @class dynamic_global_property_object
    * @brief Maintains global state information that frequently change
    * @ingroup object
    * @ingroup implementation
    */
   class dynamic_global_property_object : public chainbase::object<dynamic_global_property_object_type, dynamic_global_property_object>
   {
        OBJECT_CTOR(dynamic_global_property_object)

        id_type    id;
        uint64_t   global_action_sequence = 0;
   };

   using dynamic_global_property_multi_index = chainbase::shared_multi_index_container<
      dynamic_global_property_object,
      indexed_by<
         ordered_unique<tag<by_id>,
            BOOST_MULTI_INDEX_MEMBER(dynamic_global_property_object, dynamic_global_property_object::id_type, id)
         >
      >
   >;

}}

CHAINBASE_SET_INDEX_TYPE(sysio::chain::global_property_object, sysio::chain::global_property_multi_index)
CHAINBASE_SET_INDEX_TYPE(sysio::chain::dynamic_global_property_object,
                         sysio::chain::dynamic_global_property_multi_index)

FC_REFLECT(sysio::chain::global_property_object,
            (configuration)(chain_id)(wasm_configuration)
          )

FC_REFLECT(sysio::chain::snapshot_global_property_object,
            (configuration)(chain_id)(wasm_configuration)
          )

FC_REFLECT(sysio::chain::dynamic_global_property_object,
            (global_action_sequence)
          )
