#pragma once

#include <sysio/asset.hpp>
#include <sysio/sysio.hpp>

#include <string>

class [[sysio::contract]] noop : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void anyaction( sysio::name                       from,
                   const sysio::ignore<std::string>& type,
                   const sysio::ignore<std::string>& data );

   [[sysio::action]]
   void nonce( const sysio::ignore<std::string>& data ) {}

   /// Takes a token transfer's arguments, checks `from`'s authority and changes nothing. trx_generator's transfer
   /// load is sent here, so it needs no token contract or balances. The other arguments are not read, which keeps
   /// this contract small for the tests that deploy it as a minimal contract.
   [[sysio::action]]
   void transfer( sysio::name                        from,
                  const sysio::ignore<sysio::name>&  to,
                  const sysio::ignore<sysio::asset>& quantity,
                  const sysio::ignore<std::string>&  memo );
};
