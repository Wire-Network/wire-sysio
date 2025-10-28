#include <sysio/chain/action.hpp>
#include <sysio/chain/config.hpp>

namespace sysio {
    namespace chain {

        account_name action::payer() const {
        for ( auto &auth : authorization ) {
            if ( auth.permission == sysio::chain::config::sysio_payer_name ) {
                return auth.actor;
            }
        }
        return account;
    }
}}
