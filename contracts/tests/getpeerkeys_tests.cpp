#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>
#include <fc/io/raw.hpp>

#include <map>
#include <optional>

#include "sysio.system_tester.hpp"

using namespace sysio_system;

// Decoded mirror of sysio.system peer_keys::peerkeys_t -- the element type returned by the
// getpeerkeys action. Field order must match its SYSLIB_SERIALIZE (producer_name)(peer_key).
struct gpk_peerkeys_t {
   name                                   producer_name;
   std::optional<fc::crypto::public_key>  peer_key;
};
FC_REFLECT(gpk_peerkeys_t, (producer_name)(peer_key))

class getpeerkeys_tester : public sysio_system_tester {
public:
   action_result regpeerkey( const name& proposer, const fc::crypto::public_key& key  ) {
      return push_action(proposer, "regpeerkey"_n, mvo()("proposer_finalizer_name", proposer)("key", key));
   }

   // Push the getpeerkeys action and decode its action return value -- the value the
   // net_plugin auto-bp-peering path consumes. Exercising the decoded return here guards
   // against a dropped action return value (a CDT codegen hazard that otherwise surfaces only
   // in the auto_bp_gossip_peering integration test).
   std::vector<gpk_peerkeys_t> get_peer_keys() {
      auto trace = TESTER::push_action( config::system_account_name, "getpeerkeys"_n,
                                        config::system_account_name, mvo() );
      BOOST_REQUIRE( trace && !trace->action_traces.empty() );
      return fc::raw::unpack<std::vector<gpk_peerkeys_t>>( trace->action_traces[0].return_value );
   }
};

BOOST_AUTO_TEST_SUITE(getpeerkeys_tests)

BOOST_FIXTURE_TEST_CASE( getpeerkeys_test, getpeerkeys_tester ) { try {
   std::vector<name> prod_names = activate_producers();

   // Register peer keys for the even-indexed producers; the odd ones stay keyless.
   std::map<name, fc::crypto::public_key> registered;
   for (size_t i=0; i<prod_names.size(); ++i) {
      if (i % 2 == 0) {
         auto key = get_public_key(prod_names[i]);
         BOOST_REQUIRE_EQUAL(success(), regpeerkey(prod_names[i], key));
         registered.emplace(prod_names[i], key);
      }
   }

   // getpeerkeys returns every ranked producer (rank <= 30); a registered producer carries its
   // peer key, an unregistered one an empty optional. A dropped return value decodes to an empty
   // vector and fails the size check below.
   auto peerkeys = get_peer_keys();
   BOOST_REQUIRE_EQUAL( peerkeys.size(), prod_names.size() );

   size_t with_key = 0;
   for (const auto& pk : peerkeys) {
      auto it = registered.find(pk.producer_name);
      if (it != registered.end()) {
         BOOST_REQUIRE( !!pk.peer_key );
         BOOST_REQUIRE_EQUAL( it->second, *pk.peer_key );
         ++with_key;
      } else {
         BOOST_REQUIRE( !pk.peer_key );
      }
   }
   BOOST_REQUIRE_EQUAL( with_key, registered.size() );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
