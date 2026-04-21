/**
 * Token benchmark using sysio::kv::table with auto zero-copy.
 * Same clean API as multi_index, raw KV performance for trivially_copyable types.
 */
#include <sysio/sysio.hpp>
#include <sysio/multi_index.hpp>

using namespace sysio;

// Trivially copyable → sysio::kv::table auto-selects memcpy (no pack/unpack)
struct account {
   uint64_t sym_code;
   int64_t  balance;
   uint64_t primary_key() const { return sym_code; }
};
static_assert(std::is_trivially_copyable<account>::value, "must be trivially_copyable for zero-copy");

class [[sysio::contract("bench_kv_fast_token")]] bench_kv_fast_token : public contract {
public:
   using contract::contract;

   [[sysio::action]]
   void setup(uint32_t num_accounts) {
      for (uint32_t i = 0; i < num_accounts; ++i) {
         sysio::multi_index<"accounts"_n, account> acnts(get_self(), i);
         acnts.emplace(get_self(), [&](auto& a) {
            a.sym_code = 1;
            a.balance = 1000000;
         });
      }
   }

   [[sysio::action]]
   void dotransfers(uint32_t count) {
      for (uint32_t i = 0; i < count; ++i) {
         uint64_t from_scope = i % 100;
         uint64_t to_scope   = (i + 1) % 100;

         // sub_balance
         sysio::multi_index<"accounts"_n, account> from_acnts(get_self(), from_scope);
         auto from_itr = from_acnts.find(1);
         check(from_itr != from_acnts.end(), "no balance");
         check(from_itr->balance >= 1, "overdrawn");
         from_acnts.modify(from_itr, same_payer, [&](auto& a) {
            a.balance -= 1;
         });

         // add_balance
         sysio::multi_index<"accounts"_n, account> to_acnts(get_self(), to_scope);
         auto to_itr = to_acnts.find(1);
         check(to_itr != to_acnts.end(), "no balance");
         to_acnts.modify(to_itr, same_payer, [&](auto& a) {
            a.balance += 1;
         });
      }
   }
};
