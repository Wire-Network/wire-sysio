#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>

using namespace sysio;

class [[sysio::contract("test_kv_sec_query")]] test_kv_sec_query : public contract {
public:
   using contract::contract;

   struct user_key {
      uint64_t id;
      uint64_t primary_key() const { return id; }
      SYSLIB_SERIALIZE(user_key, (id))
   };

   struct [[sysio::table("users")]] user_val {
      name     owner;
      uint64_t balance;
      SYSLIB_SERIALIZE(user_val, (owner)(balance))
   };

   using users_table = kv::table<"users"_n, user_key, user_val,
      kv::index<"byowner"_n, kv::member_data<user_val, name, &user_val::owner>>
   >;

   users_table users{get_self()};

   [[sysio::action]]
   void adduser(uint64_t id, name owner, uint64_t balance) {
      users.emplace(get_self(), {id}, {owner, balance});
   }
};
