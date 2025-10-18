#pragma once

#include <sysio/sysio.hpp>
#include <sysio/ignore.hpp>
#include <sysio/transaction.hpp>

namespace sysio {

class [[sysio::contract("sysio.wrap")]] wrap : public contract {
public:
   using contract::contract;

   [[sysio::action]]
   void exec( ignore<name> executer, ignore<transaction> trx );

};

} /// namespace sysio
