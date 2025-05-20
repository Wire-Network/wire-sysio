#pragma once
#include <sysio/chain/types.hpp>
#include <fc/uint128.hpp>
#include <sysio/chain/block_header.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>

#include <sysio/chain/multi_index_includes.hpp>

namespace sysio {
   using boost::multi_index_container;
   using namespace boost::multi_index;
   /**
    * The purpose of this object is to store the most recent sroot
    */
   /**
    * @class contract_root_object
    * @brief Represents the root object for a contract's state root within the blockchain system.
    *
    * This object stores information related to a contract's state root at a specific block,
    * including the contract name, root name, block identifier, and associated checksums.
    * It inherits from chainbase::object and is identified by contract_root_object_type.
    *
    * Members:
    * - id: Unique identifier for this object instance.
    * - contract: The account name of the contract.
    * - root_name: The name associated with the root.
    * - block_id: The identifier of the block this state root is associated with.
    * - prev_root_bn: The block number of the previous root block produced for this contract and root name.
    * - s_id: Checksum representing the state identifier.
    * - s_root: Checksum representing the state root.
    *
    * Methods:
    * - block_num(): Returns the block number associated with block_id, incremented by one.
    */
   class contract_root_object : public chainbase::object<chain::contract_root_object_type, contract_root_object>
   {
         OBJECT_CTOR(contract_root_object)

         id_type                       id;
         chain::account_name           contract;
         chain::account_name           root_name;
         chain::block_id_type          block_id;
         uint32_t                      prev_root_bn = 0;
         chain::checksum256_type       s_id;
         chain::checksum256_type       s_root;

         uint32_t block_num() const {
            return chain::block_header::num_from_id(block_id) + 1;;
         }
   };

   struct by_id;
   struct by_contract;
   struct by_block_num;

   using contract_s_root_multi_index = chainbase::shared_multi_index_container<
      contract_root_object,
      indexed_by<
         ordered_unique< tag<by_id>, BOOST_MULTI_INDEX_MEMBER(contract_root_object, contract_root_object::id_type, id)>,
         ordered_unique< tag<by_contract>,
            composite_key<
               contract_root_object,
               BOOST_MULTI_INDEX_MEMBER(contract_root_object, chain::account_name, contract),
               BOOST_MULTI_INDEX_MEMBER(contract_root_object, chain::account_name, root_name)
            >
         >,
         ordered_non_unique< tag<by_block_num>, const_mem_fun<contract_root_object, uint32_t, &contract_root_object::block_num> >
      >
   >;
} // sysio

CHAINBASE_SET_INDEX_TYPE(sysio::contract_root_object, sysio::contract_s_root_multi_index)

FC_REFLECT(sysio::contract_root_object, (contract)(root_name)(block_id)(prev_root_bn)(s_id)(s_root))
