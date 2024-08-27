#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] integration_test : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void store( sysio::name from, sysio::name to, uint64_t num );

   struct [[sysio::table("payloads")]] payload {
      uint64_t              key;
      std::vector<uint64_t> data;

      uint64_t primary_key()const { return key; }

      SYSLIB_SERIALIZE( payload, (key)(data) )
   };

   using payloads_table = sysio::multi_index< "payloads"_n,  payload >;

};
