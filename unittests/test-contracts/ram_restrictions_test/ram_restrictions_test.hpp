#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] ram_restrictions_test : public sysio::contract {
public:
   struct [[sysio::table]] data {
      uint64_t           key;
      std::vector<char>  value;

      uint64_t primary_key() const { return key; }
   };

   typedef sysio::multi_index<"tablea"_n, data> tablea;
   typedef sysio::multi_index<"tableb"_n, data> tableb;

public:
   using sysio::contract::contract;

   [[sysio::action]]
   void noop();

   [[sysio::action]]
   void setdata( uint32_t len1, uint32_t len2, sysio::name payer );

   [[sysio::action]]
   void notifysetdat( sysio::name acctonotify, uint32_t len1, uint32_t len2, sysio::name payer );

   [[sysio::on_notify("tester2::notifysetdat")]]
   void on_notify_setdata( sysio::name acctonotify, uint32_t len1, uint32_t len2, sysio::name payer );

};
