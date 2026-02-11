#pragma once

#include <sysio/sysio.hpp>

/// Very similar to payloadless contract.
/// Used for testing infinite actions like payloadless contract.
/// Used by read_only_trx_test.py so there are two different wasm modules with
/// actions that can run forever.
class [[sysio::contract]] infinite : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void runslow();

   [[sysio::action]]
   void runforever();

   [[sysio::action]]
   void segv();
};
