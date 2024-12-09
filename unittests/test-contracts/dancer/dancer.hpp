#pragma once

#include <sysio/sysio.hpp>

class [[sysio::contract]] dancer : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void dance();

   [[sysio::action]]
   void stop();

   struct [[sysio::table("steps")]] step {
      uint64_t id;
      uint64_t count;
      bool    is_dancing;
      uint64_t primary_key() const { return id; }
   };

   typedef sysio::multi_index< "steps"_n, step > steps;

};
