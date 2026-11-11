#include <sysio.system/limit_auth_changes.hpp>
#include <sysio.system/sysio.system.hpp>

#include <string>

namespace sysiosystem {

   namespace {
      /**
       * Requires `account` to be within its RAM quota now. Called after an action has grown the account's
       * RAM usage, so the account must have room for the new data when it is written, not only once the
       * transaction ends. An unlimited quota (negative) always passes.
       */
      void check_ram_within_quota(const name& account) {
         int64_t ram_bytes  = 0;
         int64_t net_weight = 0;
         int64_t cpu_weight = 0;
         sysio::get_resource_limits(account, ram_bytes, net_weight, cpu_weight);
         if (ram_bytes < 0)
            return;
         const int64_t ram_usage = sysio::get_ram_usage(account);
         if (ram_usage > ram_bytes)
            sysio::check(false, "account " + account.to_string() + " has insufficient ram; needs " +
                                std::to_string(ram_usage) + " bytes has " + std::to_string(ram_bytes) + " bytes");
      }
   } // namespace

   void system_contract::limitauthchg(const name& account, const std::vector<name>& allow_perms,
                                      const std::vector<name>& disallow_perms) {
      limit_auth_change_table table(get_self());
      require_auth(account);
      sysio::check(allow_perms.empty() || disallow_perms.empty(), "either allow_perms or disallow_perms must be empty");
      sysio::check(allow_perms.empty() ||
                   std::find(allow_perms.begin(), allow_perms.end(), "owner"_n) != allow_perms.end(),
                   "allow_perms does not contain owner");
      sysio::check(disallow_perms.empty() ||
                   std::find(disallow_perms.begin(), disallow_perms.end(), "owner"_n) == disallow_perms.end(),
                   "disallow_perms contains owner");
      auto key = limitauthchg_key{account.value};
      if(!allow_perms.empty() || !disallow_perms.empty()) {
         // The row is billed to `account`. A row that grows must fit the account's quota as soon as it is written.
         const int64_t ram_usage_before = sysio::get_ram_usage(account);
         table.upsert(account, key,
            limit_auth_change{
               .version = 0,
               .account = account,
               .allow_perms = allow_perms,
               .disallow_perms = disallow_perms,
            },
            [&](auto& row){
               row.allow_perms = allow_perms;
               row.disallow_perms = disallow_perms;
            });
         if (sysio::get_ram_usage(account) > ram_usage_before)
            check_ram_within_quota(account);
      } else {
         if(table.contains(key))
            table.erase(key);
      }
   }

   void check_auth_change(name contract, name account, const binary_extension<name>& authorized_by) {
      name by(authorized_by.has_value() ? authorized_by.value().value : 0);
      if(by.value)
         sysio::require_auth({account, by});
      limit_auth_change_table table(contract);
      auto key = limitauthchg_key{account.value};
      if(!table.contains(key))
         return;
      auto row = table.get(key);
      sysio::check(by.value, "authorized_by is required for this account");
      if(!row.allow_perms.empty())
         sysio::check(
            std::find(row.allow_perms.begin(), row.allow_perms.end(), by) != row.allow_perms.end(),
            "authorized_by does not appear in allow_perms");
      else
         sysio::check(
            std::find(row.disallow_perms.begin(), row.disallow_perms.end(), by) == row.disallow_perms.end(),
            "authorized_by appears in disallow_perms");
   }

} // namespace sysiosystem
