#include <sysio/chain/action.hpp>
#include <sysio/chain/config.hpp>
#include <ranges>

namespace sysio::chain {

account_name action::payer() const {
   for (const auto& auth : authorization | std::views::take(1)) { // sysio_payer_name is always first
      if (auth.permission == sysio::chain::config::sysio_payer_name) {
         return auth.actor;
      }
   }
   return account;
}

account_name action::first_authorizer() const {
   for( const auto& u : authorization )
      return u.actor;
   return {};
}

}
