#pragma once

#include <sysio/sysio.hpp>
#include <sysio/asset.hpp>
#include <sysio/system.hpp>
#include <sysio/print.hpp>

using namespace sysio;
using namespace std;

class [[sysio::contract("badtoken")]] badtoken : public contract {
   public:
      using contract::contract;

      [[sysio::action]] void transfer( const name& from, const name& to, 
         const asset& quantity, const string& memo );
      [[sysio::on_notify("sysio.swap::transfer")]] void ontransfer( const name& from, const name& to, 
         const asset& quantity, const string& memo );
      [[sysio::on_notify("sysio.swap::addliquidity")]] void addliquidity(name user, asset to_buy, 
      asset max_asset1, asset max_asset2);
      [[sysio::on_notify("sysio.swap::remliquidity")]] void remliquidity(name user, asset to_sell,
      asset min_asset1, asset min_asset2);
};