#pragma once

#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/testing/tester.hpp>

#include <string>
#include <string_view>
#include <vector>

/// Helpers for tests that push incoming transactions to a producing node started from the default genesis.
namespace sysio::test {

/// Prefix of the accounts the test transactions create. Each transaction is a native newaccount by sysio, which runs
/// on a chain with no contracts; a unique account name makes every transaction unique.
inline constexpr std::string_view new_account_prefix = "inctrx";
/// Letters after new_account_prefix, which fill an account name's 12 characters.
inline constexpr size_t new_account_suffix_length = 6;
/// Letters a name suffix is spelled with.
inline constexpr std::string_view new_account_letters = "abcdefghijklmnopqrstuvwxyz";
/// How long an incoming transaction stays valid.
inline constexpr fc::microseconds trx_lifetime = fc::seconds(60);

/// Key of sysio@active: a producing test node signs with it, and its default genesis assigns it to sysio.
inline chain::private_key_type sysio_key() {
   return testing::base_tester::get_private_key(chain::config::system_account_name, "active");
}

/// @return a name for an account no earlier call returned
inline chain::account_name next_new_account() {
   static uint64_t next_id = 0;
   std::string name(new_account_prefix);
   std::string suffix(new_account_suffix_length, new_account_letters.front());
   uint64_t id = next_id++;
   for (size_t i = suffix.size(); i-- > 0; id /= new_account_letters.size())
      suffix[i] = new_account_letters[id % new_account_letters.size()];
   return chain::account_name(name + suffix);
}

/// @param lifetime how long the transaction stays valid
/// @return a transaction, authorized by sysio@active, of a native newaccount by sysio for a new account
inline chain::signed_transaction make_newaccount_trx(fc::microseconds lifetime = trx_lifetime) {
   const chain::permission_level sysio_active{chain::config::system_account_name, chain::config::active_name};
   const chain::authority new_account_auth(sysio_key().get_public_key());
   chain::signed_transaction trx;
   trx.expiration = fc::time_point_sec{fc::time_point::now() + lifetime};
   trx.actions.emplace_back(std::vector<chain::permission_level>{sysio_active},
                            chain::newaccount{.creator = chain::config::system_account_name,
                                              .name = next_new_account(),
                                              .owner = new_account_auth,
                                              .active = new_account_auth});
   return trx;
}

/// Arguments of a node producing on the default genesis, signing with sysio_key().
inline std::vector<std::string> producer_args(std::vector<std::string> extra = {}) {
   const chain::private_key_type key = sysio_key();
   // <chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>
   const std::string provider = "wire,wire," + key.get_public_key().to_string({}) + ",KEY:" + key.to_string({});
   std::vector<std::string> args = {
      "-p", "sysio", "-e", "--disable-subjective-p2p-billing=true", "--signature-provider", provider};
   args.insert(args.end(), extra.begin(), extra.end());
   return args;
}

} // namespace sysio::test
