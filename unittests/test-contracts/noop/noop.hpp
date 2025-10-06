#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] noop : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void anyaction( sysio::name                       from,
                   const sysio::ignore<std::string>& type,
                   const sysio::ignore<std::string>& data );

   [[sysio::action]]
   void nonce( const sysio::ignore<std::string>& data ) {}
};
