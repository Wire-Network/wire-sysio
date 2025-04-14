#pragma once

#include <sysio/action.hpp>
#include <sysio/crypto.hpp>
#include <sysio/print.hpp>
#include <sysio/privileged.hpp>
#include <sysio/producer_schedule.hpp>
#include <sysio/contract.hpp>
#include <sysio/ignore.hpp>
#include <sysio/system.hpp>

namespace sysiosystem {
using sysio::name;
using sysio::permission_level;
using sysio::public_key;
using sysio::ignore;
using sysio::checksum256;

struct permission_level_weight {
   permission_level  permission;
   uint16_t          weight;

   // explicit serialization macro is not necessary, used here only to improve compilation time
   SYSLIB_SERIALIZE( permission_level_weight, (permission)(weight) )
};

struct key_weight {
   sysio::public_key  key;
   uint16_t           weight;

   // explicit serialization macro is not necessary, used here only to improve compilation time
   SYSLIB_SERIALIZE( key_weight, (key)(weight) )
};

struct wait_weight {
   uint32_t           wait_sec;
   uint16_t           weight;

   // explicit serialization macro is not necessary, used here only to improve compilation time
   SYSLIB_SERIALIZE( wait_weight, (wait_sec)(weight) )
};

struct authority {
   uint32_t                              threshold = 0;
   std::vector<key_weight>               keys;
   std::vector<permission_level_weight>  accounts;
   std::vector<wait_weight>              waits;

   // explicit serialization macro is not necessary, used here only to improve compilation time
   SYSLIB_SERIALIZE( authority, (threshold)(keys)(accounts)(waits) )
};

struct block_header {
   uint32_t                                  timestamp;
   name                                      producer;
   uint16_t                                  confirmed = 0;
   checksum256                               previous;
   checksum256                               transaction_mroot;
   checksum256                               action_mroot;
   uint32_t                                  schedule_version = 0;
   std::optional<sysio::producer_schedule>   new_producers;

   // explicit serialization macro is not necessary, used here only to improve compilation time
   SYSLIB_SERIALIZE(block_header, (timestamp)(producer)(confirmed)(previous)(transaction_mroot)(action_mroot)
         (schedule_version)(new_producers))
};


struct [[sysio::table("abihash"), sysio::contract("sysio.system")]] abi_hash {
   name              owner;
   checksum256       hash;
   uint64_t primary_key()const { return owner.value; }

   SYSLIB_SERIALIZE( abi_hash, (owner)(hash) )
};

/*
 * Method parameters commented out to prevent generation of code that parses input data.
 */
class [[sysio::contract("sysio.system")]] native : public sysio::contract {
public:

   using sysio::contract::contract;

   /**
    *  Called after a new account is created. This code enforces resource-limits rules
    *  for new accounts as well as new account naming conventions.
    *
    *  1. accounts cannot contain '.' symbols which forces all acccounts to be 12
    *  characters long without '.' until a future account auction process is implemented
    *  which prevents name squatting.
    *
    *  2. new accounts must stake a minimal number of tokens (as set in system parameters)
    *     therefore, this method will execute an inline buyram from receiver for newacnt in
    *     an amount equal to the current new account creation fee.
    */
   [[sysio::action]]
   void newaccount( name             creator,
                    name             name,
                    ignore<authority> owner,
                    ignore<authority> active);


   [[sysio::action]]
   void updateauth(  ignore<name>  account,
                     ignore<name>  permission,
                     ignore<name>  parent,
                     ignore<authority> auth ) {}

   [[sysio::action]]
   void deleteauth( ignore<name>  account,
                    ignore<name>  permission ) {}

   [[sysio::action]]
   void linkauth(  ignore<name>    account,
                   ignore<name>    code,
                   ignore<name>    type,
                   ignore<name>    requirement  ) {}

   [[sysio::action]]
   void unlinkauth( ignore<name>  account,
                    ignore<name>  code,
                    ignore<name>  type ) {}

   [[sysio::action]]
   void canceldelay( ignore<permission_level> canceling_auth, ignore<checksum256> trx_id ) {}

   [[sysio::action]]
   void onerror( ignore<uint128_t> sender_id, ignore<std::vector<char>> sent_trx ) {}

   [[sysio::action]]
   void setabi( name account, const std::vector<char>& abi );

   [[sysio::action]]
   void setcode( name account, uint8_t vmtype, uint8_t vmversion, const std::vector<char>& code ) {}
};
}
