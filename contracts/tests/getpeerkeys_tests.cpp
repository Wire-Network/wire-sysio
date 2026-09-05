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

#include <algorithm>
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
   /// Terminate an operator so it stops being an ELIGIBLE operator, without touching the
   /// schedule the chain is currently producing under. Pushed as sysio.opreg, which is what
   /// `opreg::terminate` requires -- the same actor `register_producer_operators` uses.
   void terminate_operator( const name& account ) {
      base_tester::push_action("sysio.opreg"_n, "terminate"_n, "sysio.opreg"_n, mvo()
         ("account", account)
         ("reason", std::string("peer-discovery test")));
      produce_block();
   }

   /// The producers the chain is currently scheduled to produce blocks from.
   std::vector<name> active_schedule_names() {
      std::vector<name> names;
      for (const auto& p : control->active_producers().producers) names.push_back(p.producer_name);
      return names;
   }

   std::vector<gpk_peerkeys_t> get_peer_keys() {
      auto trace = TESTER::push_action( config::system_account_name, "getpeerkeys"_n,
                                        config::system_account_name, mvo() );
      BOOST_REQUIRE( trace && !trace->action_traces.empty() );
      return fc::raw::unpack<std::vector<gpk_peerkeys_t>>( trace->action_traces[0].return_value );
   }
};

BOOST_AUTO_TEST_SUITE(getpeerkeys_tests)

BOOST_FIXTURE_TEST_CASE( getpeerkeys_test, getpeerkeys_tester ) { try {
   // getpeerkeys ranks by POSITION among ELIGIBLE producers -- an active producers row whose
   // owner is an ACTIVE PRODUCER operator in sysio.opreg -- so the roster needs operator rows,
   // not just regproducer. It deliberately needs NO finalizer key: peer discovery has to cover a
   // producer scheduled through setprods before it registers one, or BP gossip cannot reach it.
   std::vector<name> prod_names = activate_producers_with_operators();

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

// Peer discovery answers "who is producing blocks", and the schedule -- not rank -- is the
// authority on that. `peer_keys_db_t::update_peer_keys` returns early only on an EMPTY response,
// so a non-empty one ERASES every producer it omits: omitting a live producer evicts it from the
// BP peer map and cuts it out of the gossip mesh.
//
// Rank alone cannot identify those producers. A demoted producer retained by the
// `min_schedule_size` floor still holds its slot and still produces -- its next block is what
// clears the demotion -- yet it sorts into the tier the rank walk stops at. This test builds the
// same situation the cheap way: terminating an operator makes the rank walk skip it immediately,
// while the schedule it is producing under is unchanged.
BOOST_FIXTURE_TEST_CASE( getpeerkeys_returns_every_scheduled_producer, getpeerkeys_tester ) { try {
   std::vector<name> prod_names = activate_producers_with_operators();

   const auto scheduled = active_schedule_names();
   BOOST_REQUIRE( !scheduled.empty() );
   const auto dropped = scheduled.front();

   // Baseline: the rank walk alone already covers it.
   {
      auto peerkeys = get_peer_keys();
      BOOST_REQUIRE( std::any_of(peerkeys.begin(), peerkeys.end(),
         [&](const gpk_peerkeys_t& pk) { return pk.producer_name == dropped; }) );
   }

   terminate_operator( dropped );

   // Still scheduled -- the chain is producing its blocks from this very set.
   const auto after = active_schedule_names();
   BOOST_REQUIRE_MESSAGE(
      std::find(after.begin(), after.end(), dropped) != after.end(),
      dropped.to_string() << " should still be in the active schedule" );

   // ... so peer discovery must still return it, even though it is no longer an eligible operator
   // and the rank walk skips it.
   auto peerkeys = get_peer_keys();
   BOOST_REQUIRE_MESSAGE(
      std::any_of(peerkeys.begin(), peerkeys.end(),
         [&](const gpk_peerkeys_t& pk) { return pk.producer_name == dropped; }),
      "a scheduled producer was omitted from peer discovery and would be evicted from the peer map" );

   // Every scheduled producer, not just the one under test, and each exactly once.
   for (const auto& p : after) {
      const auto hits = std::count_if(peerkeys.begin(), peerkeys.end(),
         [&](const gpk_peerkeys_t& pk) { return pk.producer_name == p; });
      BOOST_REQUIRE_MESSAGE( hits == 1,
         p.to_string() << " appears " << hits << " times in peer discovery; expected exactly 1" );
   }
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
