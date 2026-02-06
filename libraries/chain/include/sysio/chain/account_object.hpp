#pragma once
#include <sysio/chain/database_utils.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/code_object.hpp>
#include <sysio/chain/block_timestamp.hpp>
#include <sysio/chain/abi_def.hpp>

#include "multi_index_includes.hpp"

namespace sysio { namespace chain {

   class account_object : public chainbase::object<account_object_type, account_object> {
      OBJECT_CTOR(account_object)

      id_type              id;
      account_name         name; //< name should not be changed within a chainbase modifier lambda
      block_timestamp_type creation_date;
      uint64_t             recv_sequence = 0;
      uint64_t             auth_sequence = 0;
   };
   using account_id_type = account_object::id_type;

   struct by_name;
   using account_index = chainbase::shared_multi_index_container<
      account_object,
      indexed_by<
         ordered_unique<tag<by_id>, member<account_object, account_object::id_type, &account_object::id>>,
         ordered_unique<tag<by_name>, member<account_object, account_name, &account_object::name>>
      >
   >;

   class account_metadata_object : public chainbase::object<account_metadata_object_type, account_metadata_object>
   {
      OBJECT_CTOR(account_metadata_object,(abi));

      enum class flags_fields : uint32_t {
         privileged = 1
      };

      id_type               id;
      account_name          name; //< name should not be changed within a chainbase modifier lambda
      uint64_t              code_sequence = 0;
      uint64_t              abi_sequence  = 0;
      digest_type           code_hash;
      shared_blob           abi;
      time_point            last_code_update;
      uint32_t              flags = 0;
      uint8_t               vm_type = 0;
      uint8_t               vm_version = 0;

      void set_abi( const sysio::chain::abi_def& a ) {
         abi.resize_and_fill( fc::raw::pack_size( a ), [&a](char* data, std::size_t size) {
            fc::datastream<char*> ds( data, size );
            fc::raw::pack( ds, a );
         });
      }

      sysio::chain::abi_def get_abi()const {
         sysio::chain::abi_def a;
         SYS_ASSERT( abi.size() != 0, abi_not_found_exception, "No ABI set on account {}", name );

         fc::datastream<const char*> ds( abi.data(), abi.size() );
         fc::raw::unpack( ds, a );
         return a;
      }

      bool is_privileged()const { return has_field( flags, flags_fields::privileged ); }

      void set_privileged( bool privileged )  {
         flags = set_field( flags, flags_fields::privileged, privileged );
      }
   };

   struct by_name;
   using account_metadata_index = chainbase::shared_multi_index_container<
      account_metadata_object,
      indexed_by<
         ordered_unique<tag<by_id>, member<account_metadata_object, account_metadata_object::id_type, &account_metadata_object::id>>,
         ordered_unique<tag<by_name>, member<account_metadata_object, account_name, &account_metadata_object::name>>
      >
   >;

   namespace config {
      template<>
      struct billable_size<account_metadata_object> {
         // protocol feature will be needed if this increases
         static_assert(sizeof(account_metadata_object) == 88, "account_metadata_object size changed");
         static constexpr uint64_t overhead = overhead_per_row_per_index_ram_bytes * 2; ///< 2x indices id, name
         static constexpr uint64_t value = 88 + overhead; ///< fixed field + overhead
      };
      template<>
      struct billable_size<account_object> {
         // protocol feature will be needed if this increases
         static_assert(sizeof(account_object) == 40, "account_object size changed");
         static constexpr uint64_t overhead = overhead_per_row_per_index_ram_bytes * 2; ///< 2x indices id, name
         static constexpr uint64_t value = 40 + overhead; ///< fixed field + overhead
      };
   }

} } // sysio::chain

CHAINBASE_SET_INDEX_TYPE(sysio::chain::account_object, sysio::chain::account_index)
CHAINBASE_SET_INDEX_TYPE(sysio::chain::account_metadata_object, sysio::chain::account_metadata_index)

FC_REFLECT(sysio::chain::account_object, (name)(creation_date)(recv_sequence)(auth_sequence))
FC_REFLECT(sysio::chain::account_metadata_object, (name)(code_sequence)(abi_sequence)
                                                  (code_hash)(abi)(last_code_update)(flags)(vm_type)(vm_version))
