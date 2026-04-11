#include <sysio.system/limit_auth_changes.hpp>
#include <sysio.system/sysio.system.hpp>

namespace sysiosystem {

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
