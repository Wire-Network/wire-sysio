#pragma once

#include <sysio/chain/authority.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain/asset.hpp> // Included for reducepolicy
#include <sysio/chain/binary_extension.hpp>

namespace sysio { namespace chain {

using action_name    = sysio::chain::action_name;

struct newaccount {
   account_name                     creator;
   account_name                     name;
   authority                        owner;
   authority                        active;

   static account_name get_account() {
      return sysio::chain::config::system_account_name;
   }

   static action_name get_name() {
      return "newaccount"_n;
   }
};

struct setcode {
   account_name                     account;
   uint8_t                          vmtype = 0;
   uint8_t                          vmversion = 0;
   bytes                            code;
   binary_extension<std::string>    memo; ///< Read by the system contract, not by the native handler.

   static account_name get_account() {
      return sysio::chain::config::system_account_name;
   }

   static action_name get_name() {
      return "setcode"_n;
   }
};

struct setabi {
   account_name                     account;
   bytes                            abi;
   binary_extension<std::string>    memo; ///< Read by the system contract, not by the native handler.

   static account_name get_account() {
      return sysio::chain::config::system_account_name;
   }

   static action_name get_name() {
      return "setabi"_n;
   }
};


struct updateauth {
   account_name                      account;
   permission_name                   permission;
   permission_name                   parent;
   authority                         auth;
   binary_extension<account_name>    authorized_by; ///< Read by the system contract, not by the native handler.

   static account_name get_account() {
      return sysio::chain::config::system_account_name;
   }

   static action_name get_name() {
      return "updateauth"_n;
   }
};

struct deleteauth {
   deleteauth() = default;
   deleteauth(const account_name& account, const permission_name& permission)
   :account(account), permission(permission)
   {}

   account_name                      account;
   permission_name                   permission;
   binary_extension<account_name>    authorized_by; ///< Read by the system contract, not by the native handler.

   static account_name get_account() {
      return sysio::chain::config::system_account_name;
   }

   static action_name get_name() {
      return "deleteauth"_n;
   }
};

struct linkauth {
   linkauth() = default;
   linkauth(const account_name& account, const account_name& code, const action_name& type, const permission_name& requirement)
   :account(account), code(code), type(type), requirement(requirement)
   {}

   account_name                      account;
   account_name                      code;
   action_name                       type;
   permission_name                   requirement;
   binary_extension<account_name>    authorized_by; ///< Read by the system contract, not by the native handler.

   static account_name get_account() {
      return sysio::chain::config::system_account_name;
   }

   static action_name get_name() {
      return "linkauth"_n;
   }
};

struct unlinkauth {
   unlinkauth() = default;
   unlinkauth(const account_name& account, const account_name& code, const action_name& type)
   :account(account), code(code), type(type)
   {}

   account_name                      account;
   account_name                      code;
   action_name                       type;
   binary_extension<account_name>    authorized_by; ///< Read by the system contract, not by the native handler.

   static account_name get_account() {
      return sysio::chain::config::system_account_name;
   }

   static action_name get_name() {
      return "unlinkauth"_n;
   }
};

} } /// namespace sysio::chain

FC_REFLECT( sysio::chain::newaccount                       , (creator)(name)(owner)(active) )
FC_REFLECT( sysio::chain::setcode                          , (account)(vmtype)(vmversion)(code)(memo) )
FC_REFLECT( sysio::chain::setabi                           , (account)(abi)(memo) )
FC_REFLECT( sysio::chain::updateauth                       , (account)(permission)(parent)(auth)(authorized_by) )
FC_REFLECT( sysio::chain::deleteauth                       , (account)(permission)(authorized_by) )
FC_REFLECT( sysio::chain::linkauth                         , (account)(code)(type)(requirement)(authorized_by) )
FC_REFLECT( sysio::chain::unlinkauth                       , (account)(code)(type)(authorized_by) )
