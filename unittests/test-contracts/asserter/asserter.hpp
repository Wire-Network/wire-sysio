#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] asserter : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void procassert( int8_t condition, std::string message );

   [[sysio::action]]
   void provereset();
};
