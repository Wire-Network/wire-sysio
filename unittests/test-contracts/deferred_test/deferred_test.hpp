#pragma once

#include <sysio/sysio.hpp>
#include <vector>

class [[sysio::contract]] deferred_test : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void defercall( sysio::name payer, uint64_t sender_id, sysio::name contract, uint64_t payload );

   [[sysio::action]]
   void delayedcall( sysio::name payer, uint64_t sender_id, sysio::name contract,
                     uint64_t payload, uint32_t delay_sec, bool replace_existing );

   [[sysio::action]]
   void cancelcall( uint64_t sender_id );

   [[sysio::action]]
   void deferfunc( uint64_t payload );
   using deferfunc_action = sysio::action_wrapper<"deferfunc"_n, &deferred_test::deferfunc>;

   [[sysio::action]]
   void inlinecall( sysio::name contract, sysio::name authorizer, uint64_t payload );

   [[sysio::action]]
   void fail();

   [[sysio::on_notify("sysio::onerror")]]
   void on_error( uint128_t sender_id, sysio::ignore<std::vector<char>> sent_trx );
};
