#pragma once

#include <sysio/sysio.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>

// Extacted from sysio.token contract:
namespace sysio {
   class [[sysio::contract("sysio.token")]] token : public sysio::contract {
   public:
      using sysio::contract::contract;

      [[sysio::action]]
      void transfer( sysio::name        from,
                     sysio::name        to,
                     sysio::asset       quantity,
                     const std::string& memo );
      using transfer_action = sysio::action_wrapper<"transfer"_n, &token::transfer>;
   };
}

// This contract:
class [[sysio::contract]] proxy : public sysio::contract {
public:
   proxy( sysio::name self, sysio::name first_receiver, sysio::datastream<const char*> ds );

   [[sysio::action]]
   void setowner( sysio::name owner, uint32_t delay );

   [[sysio::on_notify("sysio.token::transfer")]]
   void on_transfer( sysio::name        from,
                     sysio::name        to,
                     sysio::asset       quantity,
                     const std::string& memo );

   [[sysio::on_notify("sysio::onerror")]]
   void on_error( uint128_t sender_id, sysio::ignore<std::vector<char>> sent_trx );

   struct [[sysio::table]] config {
      sysio::name owner;
      uint32_t    delay   = 0;
      uint32_t    next_id = 0;

      SYSLIB_SERIALIZE( config, (owner)(delay)(next_id) )
   };

   using config_singleton = sysio::singleton< "config"_n,  config >;

protected:
   config_singleton _config;
};
