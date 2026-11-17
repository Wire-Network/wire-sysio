#include "sysio.wrap.hpp"

#include <sysio.system/canonical_transaction.hpp>

namespace sysio {

void wrap::exec( ignore<name>, ignore<transaction> ) {
   require_auth( get_self() );

   name executer;
   _ds >> executer;

   require_auth( executer );

   const transaction trx = sysiosystem::unpack_canonical_transaction( _ds );
   check( trx.context_free_actions.empty(), "not allowed to `exec` a transaction with context-free actions" );
   check( trx.transaction_extensions.empty(), "not allowed to `exec` a transaction with transaction extensions" );
   check_action_data_consumed( _ds );

   for (const auto& act : trx.actions) {
      act.send();
   }
}

} /// namespace sysio
