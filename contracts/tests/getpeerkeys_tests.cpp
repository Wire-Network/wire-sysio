#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>

#include "sysio.system_tester.hpp"

using namespace sysio_system;

class getpeerkeys_tester : public sysio_system_tester {
public:
   action_result regpeerkey( const name& proposer, const fc::crypto::public_key& key  ) {
      return push_action(proposer, "regpeerkey"_n, mvo()("proposer_finalizer_name", proposer)("key", key));
   }
};

BOOST_AUTO_TEST_SUITE(getpeerkeys_tests)

BOOST_FIXTURE_TEST_CASE( getpeerkeys_test, getpeerkeys_tester ) { try {
   std::vector<name> prod_names = activate_producers();

   for (size_t i=0; i<prod_names.size(); ++i) {
      auto n = prod_names[i];
      if (i % 2 == 0)
         BOOST_REQUIRE_EQUAL(success(), regpeerkey(n, get_public_key(n)));
   }
/* TODO: there is no ranking of producers currently in WIRE
   auto peerkeys = control->get_top_producer_keys(); // call readonly action from controller
   BOOST_REQUIRE_EQUAL(peerkeys.size(), 11);

   size_t num_found = 0;
   for (size_t i=0; i<prod_names.size(); ++i) {
      auto n = prod_names[i];
      for (auto& p : peerkeys) {
         if (p.producer_name == n) {
            ++num_found;
            BOOST_REQUIRE(!!p.peer_key);
            BOOST_REQUIRE_EQUAL(get_public_key(n), *p.peer_key);
         }
      }
   }
   BOOST_REQUIRE_EQUAL(num_found, 11);
*/
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
