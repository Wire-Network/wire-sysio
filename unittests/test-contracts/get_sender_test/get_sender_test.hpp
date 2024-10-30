#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] get_sender_test : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void assertsender( sysio::name expected_sender );
   using assertsender_action = sysio::action_wrapper<"assertsender"_n, &get_sender_test::assertsender>;

   [[sysio::action]]
   void sendinline( sysio::name to, sysio::name expected_sender );

   [[sysio::action]]
   void notify( sysio::name to, sysio::name expected_sender, bool send_inline );

   [[sysio::on_notify("*::notify")]]
   void on_notify( sysio::name to, sysio::name expected_sender, bool send_inline );

};
