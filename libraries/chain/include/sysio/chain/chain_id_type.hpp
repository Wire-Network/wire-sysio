#pragma once

#include <fc/crypto/sha256.hpp>

namespace sysio {

   class net_plugin_impl;
   struct handshake_message;

   namespace chain_apis {
      class get_info_db;
   }

   class chain_plugin;

namespace chain {

   struct chain_id_type : public fc::sha256 {
      using fc::sha256::sha256;

      template<typename T>
      inline friend T& operator<<( T& ds, const chain_id_type& cid ) {
        ds.write( cid.data(), cid.data_size() );
        return ds;
      }

      template<typename T>
      inline friend T& operator>>( T& ds, chain_id_type& cid ) {
        ds.read( cid.data(), cid.data_size() );
        return ds;
      }

      void reflector_init()const;

      static chain_id_type empty_chain_id() {
         return {};
      }

      private:
         chain_id_type() = default;

         // Some exceptions are unfortunately necessary:
         template<typename T>
         friend T fc::variant::as()const;

         friend class sysio::chain_apis::get_info_db;

         friend class sysio::net_plugin_impl;
         friend struct sysio::handshake_message;
         friend class block_log;
         friend struct trim_data;
         friend class controller;
         friend struct controller_impl;
         friend class global_property_object;
         friend struct snapshot_global_property_object;
   };

} }  // namespace sysio::chain

namespace fc {
  class variant;
  void to_variant(const sysio::chain::chain_id_type& cid, fc::variant& v);
  void from_variant(const fc::variant& v, sysio::chain::chain_id_type& cid);
} // fc
