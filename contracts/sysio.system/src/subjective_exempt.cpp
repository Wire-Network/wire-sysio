#include <sysio.system/subjective_exempt.hpp>

#include <string_view>

namespace sysiosystem {

namespace {

constexpr std::string_view account_missing_msg = "account does not exist";
constexpr std::string_view not_listed_msg      = "account is not on the subjective billing exempt list";

} // anonymous namespace

void subjective_exempt::addsubjex(name account) {
   if (!sysio::has_auth(bond_refs::account))
      require_auth(get_self());
   sysio::check(sysio::is_account(account), account_missing_msg);

   subj_exempt_table    list(get_self());
   const subjexempt_key key{account};
   if (!list.contains(key))
      list.emplace(get_self(), key, subj_exempt{.account = account});
}

void subjective_exempt::delsubjex(name account) {
   require_auth(get_self());

   subj_exempt_table    list(get_self());
   const subjexempt_key key{account};
   sysio::check(list.contains(key), not_listed_msg);
   list.erase(key);
}

} // namespace sysiosystem
