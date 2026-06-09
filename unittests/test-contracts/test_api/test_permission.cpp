#include <limits>

#include <sysio/action.hpp>
#include <sysio/sysio.hpp>
#include <sysio/permission.hpp>
#include <sysio/print.hpp>
#include <sysio/serialize.hpp>

#include "test_api.hpp"

struct check_auth_msg {
   sysio::name                    account;
   sysio::name                    permission;
   std::vector<sysio::public_key> pubkeys;

   SYSLIB_SERIALIZE( check_auth_msg, (account)(permission)(pubkeys)  )
};

void test_permission::check_authorization( uint64_t receiver, uint64_t code, uint64_t action ) {
   (void)code;
   (void)action;
   using namespace sysio;

   auto self = receiver;
   auto params = unpack_action_data<check_auth_msg>();
   auto packed_pubkeys = pack(params.pubkeys);
   int64_t res64 = sysio::check_permission_authorization( params.account,
                                                     params.permission,
                                                     packed_pubkeys.data(), packed_pubkeys.size(),
                                                     (const char*)0,        0,
                                                     microseconds{ 0 }
                                                   );

   char key[24];
   make_kv_key(self, self, 1, key);
   // kv_set does upsert — stores if new, updates if exists
   kv_set( 0, self, key, 24, &res64, sizeof(int64_t) );
}

