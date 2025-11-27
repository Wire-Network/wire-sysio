#pragma once

#include <fc/reflect/reflect.hpp>

namespace sysio::testing {

struct assertdef {
   int8_t      condition;
   string      message;

   static account_name get_account() {
      return "asserter"_n;
   }

   static action_name get_name() {
      return "procassert"_n;
   }
};

struct provereset {
   static account_name get_account() {
      return "asserter"_n;
   }

   static action_name get_name() {
      return "provereset"_n;
   }
};

} // namespace sysio::testing

FC_REFLECT( sysio::testing::assertdef, (condition)(message) );
FC_REFLECT_EMPTY( sysio::testing::provereset );
