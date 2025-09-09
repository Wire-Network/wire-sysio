#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/contract_types.hpp>

namespace sysio { namespace chain {

   class apply_context;

   /**
    * @defgroup native_action_handlers Native Action Handlers
    */
   ///@{
   void apply_sysio_newaccount(apply_context&);
   void apply_sysio_updateauth(apply_context&);
   void apply_sysio_deleteauth(apply_context&);
   void apply_sysio_linkauth(apply_context&);
   void apply_sysio_unlinkauth(apply_context&);
   // **Roa changes**
   void apply_roa_reducepolicy(apply_context&);

   /*
   void apply_sysio_postrecovery(apply_context&);
   void apply_sysio_passrecovery(apply_context&);
   void apply_sysio_vetorecovery(apply_context&);
   */

   void apply_sysio_setcode(apply_context&);
   void apply_sysio_setabi(apply_context&);

   ///@}  end action handlers

} } /// namespace sysio::chain
