#pragma once

#include <sysio/kv_table.hpp>

namespace sysiosystem {
   using sysio::name;

   struct limitauthchg_key {
      uint64_t account;
      SYSLIB_SERIALIZE(limitauthchg_key, (account))
   };

   struct [[sysio::table("limitauthchg"),sysio::contract("sysio.system")]] limit_auth_change {
      uint8_t              version = 0;
      name                 account;
      std::vector<name>    allow_perms;
      std::vector<name>    disallow_perms;

      SYSLIB_SERIALIZE(limit_auth_change, (version)(account)(allow_perms)(disallow_perms))
   };

   using limit_auth_change_table = sysio::kv::table<"limitauthchg"_n, limitauthchg_key, limit_auth_change>;
} // namespace sysiosystem
