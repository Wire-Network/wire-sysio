#pragma once

#include <sysio/asset.hpp>
#include <sysio/sysio.hpp>

#include <string>

/// Does CPU work in a contract reached through sysio.token or sysio.msig, for the CPU billing tests.
class [[sysio::contract]] cpu_receiver_test : public sysio::contract {
public:
   using sysio::contract::contract;

   /**
    * Handles a sysio.token transfer this contract is notified of. A transfer to it does what the memo says:
    *   spin:<n>        loops n times
    *   spinsend:<n>    loops n times and sends itself an inline spin of n loops
    *   forward:<acct>  forwards the notification to acct
    *   pay:<acct>      pays the quantity to acct with sysio.token, memo "spin:1000"
    *   forever         loops until the transaction runs out of CPU
    * A forwarded notification, or one for a transfer it sends, loops the default number of times.
    */
   [[sysio::on_notify("sysio.token::transfer")]]
   void on_transfer( sysio::name from, sysio::name to, sysio::asset quantity, std::string memo );

   /// Pays `quantity` to `to` with sysio.token.
   [[sysio::action]]
   void payout( sysio::name to, sysio::asset quantity, std::string memo );

   /// Loops n times and prints that it ran. It needs no authorization.
   [[sysio::action]]
   void spin( uint32_t n );

   /// Notifies `to` of this action, without authorization or writes, so a read-only transaction can run it.
   [[sysio::action]]
   void notify( sysio::name to );
};
