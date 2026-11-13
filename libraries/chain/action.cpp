#include <sysio/chain/action.hpp>
#include <sysio/chain/config.hpp>

namespace sysio::chain {

bool action::has_explicit_payer() const {
   // sysio_payer_name must be position 0; enforced by validate_referenced_accounts
   // and authorization_manager::check_authorization.
   return !authorization.empty() && authorization[0].permission == config::sysio_payer_name;
}

account_name action::payer() const {
   return has_explicit_payer() ? authorization[0].actor : account;
}

account_name action::first_authorizer() const {
   if (!authorization.empty())
      return authorization[0].actor;
   return {};
}

}
