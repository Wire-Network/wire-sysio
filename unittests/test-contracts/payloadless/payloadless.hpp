#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] payloadless : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void doit();

   [[sysio::action]]
   void doitslow();
};
