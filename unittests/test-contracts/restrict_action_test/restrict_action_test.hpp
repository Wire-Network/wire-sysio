#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] restrict_action_test : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void noop( );

   [[sysio::action]]
   void sendinline( sysio::name authorizer );


   [[sysio::action]]
   void notifyinline( sysio::name acctonotify, sysio::name authorizer );

   [[sysio::on_notify("testacc::notifyinline")]]
   void on_notify_inline( sysio::name acctonotify, sysio::name authorizer );
};
