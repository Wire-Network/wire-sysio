#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/peer_scoring.hpp>

#include <memory>
#include <vector>

using namespace sysio::peer_scoring;

namespace {

/// A peer with a fixed score, whether the limit being enforced counts it, and whether it is already closing.
struct mock_peer {
   int32_t score   = baseline;
   bool    counted = true;
   bool    closing = false;

   int32_t get_peer_score() const { return score; }
   bool    closed() const { return closing; }
};
using mock_peer_ptr = std::shared_ptr<mock_peer>;

/// Visits its peers in order, as connections_manager::for_each_connection() does.
struct mock_peers {
   std::vector<mock_peer_ptr> peers;

   template <typename Function>
   void for_each_connection(Function&& f) const {
      for (const auto& peer : peers)
         f(peer);
   }
};

mock_peer_ptr make_peer(int32_t score, bool counted = true) {
   return std::make_shared<mock_peer>(mock_peer{score, counted});
}

/// select_eviction_victim with the counted peers evictable
mock_peer_ptr select_victim(const mock_peers& peers) {
   return select_eviction_victim<mock_peer_ptr>(peers, [](const mock_peer_ptr& peer) { return peer->counted; });
}

} // namespace

BOOST_AUTO_TEST_SUITE(peer_scoring_tests)

BOOST_AUTO_TEST_CASE(no_peers_no_victim) {
   BOOST_CHECK(!select_victim(mock_peers{}));
}

BOOST_AUTO_TEST_CASE(peers_at_or_above_threshold_keep_their_slot) {
   const mock_peers peers{{make_peer(eviction_threshold), make_peer(baseline), make_peer(max_score)}};
   BOOST_CHECK(!select_victim(peers));
}

BOOST_AUTO_TEST_CASE(lowest_score_below_threshold_is_evicted) {
   const auto lowest = make_peer(min_score);
   const mock_peers peers{{make_peer(baseline), make_peer(eviction_threshold - 1), lowest,
                           make_peer(eviction_threshold - 10)}};
   BOOST_CHECK(select_victim(peers) == lowest);
}

BOOST_AUTO_TEST_CASE(peers_not_evictable_are_skipped) {
   const auto counted = make_peer(eviction_threshold - 1);
   const mock_peers peers{{make_peer(min_score, false), counted, make_peer(min_score, false)}};
   BOOST_CHECK(select_victim(peers) == counted);

   counted->counted = false;
   BOOST_CHECK(!select_victim(peers));
}

BOOST_AUTO_TEST_CASE(closing_peers_are_never_evicted) {
   const auto closing = make_peer(min_score);
   closing->closing = true;
   const auto next = make_peer(eviction_threshold - 1);
   const mock_peers peers{{closing, next}};
   BOOST_CHECK(select_victim(peers) == next);

   next->closing = true;
   BOOST_CHECK(!select_victim(peers));
}

BOOST_AUTO_TEST_CASE(tie_evicts_first_visited) {
   const auto first = make_peer(eviction_threshold - 20);
   const mock_peers peers{{make_peer(baseline), first, make_peer(eviction_threshold - 20)}};
   BOOST_CHECK(select_victim(peers) == first);
}

BOOST_AUTO_TEST_SUITE_END()
