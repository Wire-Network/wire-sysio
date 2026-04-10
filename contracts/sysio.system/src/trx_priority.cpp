#include <sysio.system/trx_priority.hpp>


namespace sysiosystem {

void trx_priority::addtrxp(name receiver, name action_name, trx_match_type match_type, short priority) {
   require_auth( get_self() );
   sysio::check(receiver.value != 0, "receiver cannot be empty");
   sysio::check(match_type <= trx_match_type::any, "Invalid match type");

   trx_priority_table tbl(get_self());

   auto key = trxprio_key{static_cast<uint64_t>(static_cast<uint16_t>(priority))};
   sysio::check(!tbl.contains(key), "Priority " + std::to_string(priority) + " already exists");
   tbl.emplace(get_self(), key, trx_prio{
      .priority = priority,
      .receiver = receiver,
      .action_name = action_name,
      .match_type = match_type,
   });
   _gstate.last_trx_priority_update = sysio::current_time_point();
}

void trx_priority::deltrxp(short priority) {
   require_auth( get_self() );

   trx_priority_table tbl(get_self());
   auto key = trxprio_key{static_cast<uint64_t>(static_cast<uint16_t>(priority))};
   sysio::check(tbl.contains(key), "Unable to find priority: " + std::to_string(priority));
   tbl.erase(key);
   _gstate.last_trx_priority_update = sysio::current_time_point();
}

} // namespace sysiosystem
