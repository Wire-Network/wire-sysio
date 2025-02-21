#pragma once
#include <sysio/chain/types.hpp>
#include <fc/uint128.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>

#include "multi_index_includes.hpp"

namespace sysio {
   using boost::multi_index_container;
   using namespace boost::multi_index;
   /**
    * The purpose of this object is to store the most recent sroot
    */
   class contract_s_root_object : public chainbase::object<contract_s_root_object_type, contract_s_root_object>
   {
         OBJECT_CTOR(contract_s_root_object, (packed_trx) )

         id_type                       id;
         account_name                  contract;
         block_id_type                 block_id;
         checksum256_type              s_id;
         checksum256_type              s_root;

         uint32_t block_num() {
            return block_header::num_from_id(block_id) + 1;;
         }
   };

   struct by_id;
   struct by_contract;
   struct by_block_num;

   using contract_s_root_multi_index = chainbase::shared_multi_index_container<
      contract_s_root_object,
      indexed_by<
         ordered_unique< tag<by_id>, BOOST_MULTI_INDEX_MEMBER(contract_s_root_object, contract_s_root_object::id_type, id)>,
         ordered_unique< tag<by_contract>, BOOST_MULTI_INDEX_MEMBER( contract_s_root_object, account_name, contract)>,
         ordered_unique< tag<by_block_num>, const_mem_fun< contract_s_root_object, uint32_t, &contract_s_root_object::block_num> >
   >;
} // sysio

CHAINBASE_SET_INDEX_TYPE(sysio::contract_s_root_object, sysio::contract_s_root_multi_index)

FC_REFLECT(sysio::contract_s_root_object, (contract)(block_id)(s_id)(s_root))
