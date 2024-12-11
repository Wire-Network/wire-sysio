#pragma once
#include <sysio/chain/types.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/chain/asset.hpp>
#include <chainbase/chainbase.hpp>
#include <fc/reflect/reflect.hpp>

/**
 * @brief This file represents the structure of the system contract sysio.roa which replaces the old resource management system.
 * The tables defined here MUST match the structure in the system contract ( sysio.roa.hpp ).
 */
namespace sysio { namespace chain {

   // Note: The object_type IDs below start at 200 to avoid conflicts with core SYSIO object types.
   // Check sysio/chain/types.hpp for core definitions.
   // Do not modify sysio/chain/types.hpp. Keep custom object types in separate files.
   enum sysio_roa_object_type {
      nodeowners_object_type = 200,
      policies_object_type,
      reslimit_object_type
   };

   class nodeowners_object : public chainbase::object<nodeowners_object_type, nodeowners_object> {
    CHAINBASE_DEFAULT_CONSTRUCTOR(nodeowners_object)

    public:
    id_type  id;
    name     owner;          // Node Owner's account name.
    uint8_t  tier;
    asset    total_sys;
    asset    allocated_sys;
    asset    allocated_bw;
    asset    allocated_ram;
    uint8_t  network_gen;    // Added field for scoping

    // Primary key for chainbase
    id_type primary_key() const { return id; }

    // Accessors for indexing by (network_gen, owner)
    name    get_owner() const { return owner; }
    uint8_t get_network_gen() const { return network_gen; }

    // No need for get_tier() since we are not indexing or filtering by tier in native code
    };

   struct by_network_gen_owner;

    using nodeowners_index = chainbase::shared_multi_index_container<
        nodeowners_object,
        indexed_by<
            ordered_unique<tag<by_id>, member<nodeowners_object, nodeowners_object::id_type, &nodeowners_object::id>>,
            // Composite key using network_gen and owner for scoping
            ordered_unique<tag<by_network_gen_owner>,
                composite_key<nodeowners_object,
                    const_mem_fun<nodeowners_object, uint8_t, &nodeowners_object::get_network_gen>,
                    const_mem_fun<nodeowners_object, name, &nodeowners_object::get_owner>
                >
            >
        >
    >;

   class policies_object : public chainbase::object<policies_object_type, policies_object> {
      CHAINBASE_DEFAULT_CONSTRUCTOR(policies_object)

   public:
      id_type  id;
      name     owner;          // The account this policy applies to.
      name     issuer;         // The Node Owner who issued this policy.
      asset    net_weight;
      asset    cpu_weight;
      asset    ram_weight;
      uint64_t bytes_per_unit;
      uint32_t time_block;

      id_type primary_key() const { return id; }
      name    get_owner() const { return owner; }
      name    get_issuer() const { return issuer; }
   };

   struct by_issuer_owner; 
   // struct by_owner; // Remove if not used

   using policies_index = chainbase::shared_multi_index_container<
      policies_object,
      indexed_by<
         ordered_unique<tag<by_id>, member<policies_object, policies_object::id_type, &policies_object::id>>,
         ordered_unique<tag<by_issuer_owner>,
            composite_key<policies_object,
               const_mem_fun<policies_object, name, &policies_object::get_issuer>,
               const_mem_fun<policies_object, name, &policies_object::get_owner>
            >
         >
      >
   >;
    
   class reslimit_object : public chainbase::object<reslimit_object_type, reslimit_object> {
      CHAINBASE_DEFAULT_CONSTRUCTOR(reslimit_object)
   
   public:
      id_type  id;
      name     owner;
      asset    net_weight;
      asset    cpu_weight;
      uint64_t ram_bytes;

      id_type primary_key() const { return id; }
      name    get_owner() const { return owner; }
   };

   struct by_reslimit_owner;

   using reslimit_index = chainbase::shared_multi_index_container<
      reslimit_object,
      indexed_by<
         ordered_unique<tag<by_id>, member<reslimit_object, reslimit_object::id_type, &reslimit_object::id>>,
         ordered_unique<tag<by_reslimit_owner>, const_mem_fun<reslimit_object, name, &reslimit_object::get_owner>>
      >
   >;

}} // namespace sysio::chain

CHAINBASE_SET_INDEX_TYPE(sysio::chain::nodeowners_object, sysio::chain::nodeowners_index)
CHAINBASE_SET_INDEX_TYPE(sysio::chain::policies_object, sysio::chain::policies_index)
CHAINBASE_SET_INDEX_TYPE(sysio::chain::reslimit_object, sysio::chain::reslimit_index)

FC_REFLECT(sysio::chain::nodeowners_object, (id)(owner)(tier)(total_sys)(allocated_sys)(allocated_bw)(allocated_ram)(network_gen))
FC_REFLECT(sysio::chain::policies_object, (id)(owner)(issuer)(net_weight)(cpu_weight)(ram_weight)(bytes_per_unit)(time_block))
FC_REFLECT(sysio::chain::reslimit_object, (id)(owner)(net_weight)(cpu_weight)(ram_bytes))

