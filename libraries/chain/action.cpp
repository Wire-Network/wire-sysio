#include <sysio/chain/action.hpp>
#include <sysio/chain/config.hpp>

namespace sysio::chain {

account_name action::payer() const {
   // sysio_payer_name must be position 0; enforced by validate_referenced_accounts
   // and authorization_manager::check_authorization.
   if (!authorization.empty() && authorization[0].permission == config::sysio_payer_name)
      return authorization[0].actor;
   return account;
}

account_name action::first_authorizer() const {
   if (!authorization.empty())
      return authorization[0].actor;
   return {};
}

}
