#include "ram_restrictions_test.hpp"
#include <sysio/transaction.hpp>

using namespace sysio;

template <typename Table>
void _setdata(name self, int len, name payer) {
   Table ta(self, 0);
   std::vector<char> data;
   data.resize(len, 0);
   auto it = ta.find(0);
   if (it == ta.end()) {
     ta.emplace(payer, [&](auto &v) {
         v.key = 0;
         v.value = data;
     });
   } else {
     ta.modify(it, payer, [&](auto &v) {
         v.key = 0;
         v.value = data;
     });
   }
}

void ram_restrictions_test::noop( ) {
}

void ram_restrictions_test::setdata( uint32_t len1, uint32_t len2, name payer ) {
   _setdata<tablea>(get_self(), len1, payer);
   _setdata<tableb>(get_self(), len2, payer);
}

void ram_restrictions_test::notifysetdat( name acctonotify, uint32_t len1, uint32_t len2, name payer ) {
   require_recipient(acctonotify);
}

void ram_restrictions_test::on_notify_setdata( name acctonotify, uint32_t len1, uint32_t len2, name payer) {
   setdata(len1, len2, payer);
}
