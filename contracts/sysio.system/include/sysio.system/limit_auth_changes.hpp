#pragma once

#include <sysio/multi_index.hpp>

namespace sysiosystem {
   using sysio::name;

   struct [[sysio::table("limitauthchg"),sysio::contract("sysio.system")]] limit_auth_change {
      uint8_t              version = 0;
      name                 account;
      std::vector<name>    allow_perms;
      std::vector<name>    disallow_perms;

      uint64_t primary_key() const { return account.value; }

      SYSLIB_SERIALIZE(limit_auth_change, (version)(account)(allow_perms)(disallow_perms))
   };

   typedef sysio::multi_index<"limitauthchg"_n, limit_auth_change> limit_auth_change_table;
} // namespace sysiosystem
