/**
 * Minimal token contract simulating transfer pattern using KV shim.
 * Matches sysio.token::transfer flow: 2 finds + 2 modifies per transfer.
 */
#include <sysio/sysio.hpp>
// kv_multi_index provided by CDT via sysio.hpp -> multi_index.hpp

using namespace sysio;

class [[sysio::contract("bench_kv_shim_token")]] bench_kv_shim_token : public contract {
public:
   using contract::contract;

   struct [[sysio::table]] account {
      uint64_t id;      // symbol code
      int64_t  balance;
      uint64_t primary_key() const { return id; }
   };

   typedef kv_multi_index<"accounts"_n, account> accounts_table;

   [[sysio::action]]
   void setup(uint32_t num_accounts) {
      // Create num_accounts accounts each with balance 1000000
      for (uint32_t i = 0; i < num_accounts; ++i) {
         accounts_table acnts(get_self(), i); // scope = account index
         acnts.emplace(get_self(), [&](auto& a) {
            a.id = 1; // token symbol code
            a.balance = 1000000;
         });
      }
   }

   [[sysio::action]]
   void dotransfers(uint32_t count) {
      // Simulate 'count' token transfers between account pairs
      for (uint32_t i = 0; i < count; ++i) {
         uint64_t from_scope = i % 100;
         uint64_t to_scope   = (i + 1) % 100;
         int64_t  amount     = 1;

         // sub_balance
         accounts_table from_acnts(get_self(), from_scope);
         auto from_itr = from_acnts.find(1);
         check(from_itr != from_acnts.end(), "no balance");
         check(from_itr->balance >= amount, "overdrawn");
         from_acnts.modify(from_itr, same_payer, [&](auto& a) {
            a.balance -= amount;
         });

         // add_balance
         accounts_table to_acnts(get_self(), to_scope);
         auto to_itr = to_acnts.find(1);
         check(to_itr != to_acnts.end(), "no balance");
         to_acnts.modify(to_itr, same_payer, [&](auto& a) {
            a.balance += amount;
         });
      }
   }
};
