#pragma once

#include <sysio/asset.hpp>

namespace sysiosystem {
using sysio::asset;
using sysio::symbol;

typedef double real_type;

/**
 *  Uses Bancor math to create a 50/50 relay between two asset types. The state of the
 *  bancor exchange is entirely contained within this struct. There are no external
 *  side effects associated with using this API.
 */
struct [[sysio::table, sysio::contract("sysio.system")]] exchange_state {
   asset    supply;

   struct connector {
      asset balance;
      double weight = .5;

      SYSLIB_SERIALIZE( connector, (balance)(weight) )
   };

   connector base;
   connector quote;

   uint64_t primary_key()const { return supply.symbol.raw(); }

   asset convert_to_exchange( connector& c, asset in );
   asset convert_from_exchange( connector& c, asset in );
   asset convert( asset from, const symbol& to );

   SYSLIB_SERIALIZE( exchange_state, (supply)(base)(quote) )
};

typedef sysio::multi_index< "rammarket"_n, exchange_state > rammarket;

} /// namespace sysiosystem
