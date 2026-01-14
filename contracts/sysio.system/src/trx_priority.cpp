#include <sysio.system/trx_priority.hpp>


namespace sysiosystem {

trx_priority::trx_priority(name s, name code, sysio::datastream<const char*> ds)
   : sysio::contract(s, code, ds),
     _global(get_self(), get_self().value)
{
   _gstate = _global.get_or_create(get_self());
}

trx_priority::~trx_priority() {
   _global.set(_gstate, get_self());
}

void trx_priority::addtrxp(name receiver, name action_name, trx_match_type match_type, short priority) {
   require_auth( get_self() );

   trx_priority_table trx_priority_table(get_self(), get_self().value);

   auto itr = trx_priority_table.find(priority);
   sysio::check(itr == trx_priority_table.end(), "Priority " + std::to_string(priority) + " already exists");
   trx_priority_table.emplace(get_self(), [&](auto& row) {
      row.priority = priority;
      row.receiver = receiver;
      row.action_name = action_name;
      row.match_type = match_type;
   });
   _gstate.last_trx_priority_update = sysio::current_time_point();
}

void trx_priority::deltrxp(short priority) {
   require_auth( get_self() );

   trx_priority_table trx_priority_table(get_self(), get_self().value);
   auto itr = trx_priority_table.find(priority);
   sysio::check(itr != trx_priority_table.end(), "Unable to find priority: " + std::to_string(priority));
   trx_priority_table.erase(itr);
   _gstate.last_trx_priority_update = sysio::current_time_point();
}

} // namespace sysiosystem
