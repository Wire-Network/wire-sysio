#include "sysio.wrap.hpp"

namespace sysio {

void wrap::exec( ignore<name>, ignore<transaction> ) {
   require_auth( get_self() );

   name executer;
   _ds >> executer;

   require_auth( executer );

   transaction_header trx_header;
   std::vector<action> context_free_actions;
   std::vector<action> actions;
   _ds >> trx_header;
   _ds >> context_free_actions;
   check( context_free_actions.empty(), "not allowed to `exec` a transaction with context-free actions" );
   _ds >> actions;
   extensions_type transaction_extensions;
   _ds >> transaction_extensions;
   check( transaction_extensions.empty(), "not allowed to `exec` a transaction with transaction extensions" );
   check_action_data_consumed( _ds );

   for (const auto& act : actions) {
      act.send();
   }
}

} /// namespace sysio
