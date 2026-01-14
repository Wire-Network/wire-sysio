#pragma once

#include <sysio/contract.hpp>
#include <sysio/multi_index.hpp>
#include <sysio/name.hpp>
#include <sysio/singleton.hpp>
#include <sysio/time.hpp>

namespace sysiosystem {

using sysio::name;

// -------------------------------------------------------------------------------------------------
enum trx_match_type : uint8_t {
   only = 0,    // trx has only one action and it matches
   first = 1,   // trx first action matches
   any = 2      // trx has any action that matches
};
struct [[sysio::table("trxpriority"), sysio::contract("sysio.system")]] trx_prio {
   short priority = 0;
   name receiver{};
   name action_name{};
   trx_match_type match_type = only;

   uint64_t primary_key() const { return priority; }

   SYSLIB_SERIALIZE(trx_prio, (priority)(receiver)(action_name)(match_type))
};

// -------------------------------------------------------------------------------------------------
struct [[sysio::table("trxpglobal"), sysio::contract("sysio.system")]] trx_prio_global {
   sysio::block_timestamp last_trx_priority_update{};

   SYSLIB_SERIALIZE(trx_prio_global, (last_trx_priority_update))
};

// -------------------------------------------------------------------------------------------------
using trx_priority_table = sysio::multi_index<"trxpriority"_n, trx_prio>;
using global_trx_prio_singleton = sysio::singleton< "trxpglobal"_n, trx_prio_global >;

// -------------------------------------------------------------------------------------------------
struct [[sysio::contract("sysio.system")]] trx_priority : public sysio::contract {
private:
   global_trx_prio_singleton   _global;
   trx_prio_global             _gstate;

public:

   trx_priority(name s, name code, sysio::datastream<const char*> ds);
   ~trx_priority();

   /**
    * Action to register a transaction priority.
    * priority required to be unique.
    *
    * Entries are assumed to be mutually exclusive, if not then results are undefined. For example, given
    * [1, "sysio"_n, ""_n, only] and [2, "sysio"_n, ""_n, any], then the priority might be 1 or 2. Exact match
    * of action_name does take precedence over any (empty action_name) match. For example, given
    * [1, "sysio"_n, ""_n, only] and [2, "sysio"_n, "doit"_n, any], then the priority will be 2 for a doit action.
    *
    * @param receiver The action receiver contract account (action.account). Can't be empty.
    * @param action_name The action name (action.name). Empty matches any action of receiver.
    * @param match_type The transaction match type. See trx_match_type enum.
    * @param priority The transaction priority. -32,768 to 32,767. A negative value will prioritize less than default.
    */
   [[sysio::action]]
   void addtrxp(name receiver, name action_name, trx_match_type match_type, short priority);

   /**
    * Finds unique priority and removes entry. Associated trxs will no longer be prioritized.
    * @param priority The transaction priority to delete.
    */
   [[sysio::action]]
   void deltrxp(short priority);
};

} // namespace sysiosystem