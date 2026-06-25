#include <boost/test/unit_test.hpp>

#include <sysio/net_plugin/local_txn_cache.hpp>

#include <algorithm>
#include <string_view>

using namespace sysio;

namespace {
   constexpr connection_id_t first_connection_id = 1;
   constexpr connection_id_t second_connection_id = 2;
   constexpr auto            transaction_cache_lifetime = fc::seconds(30);

   /// Builds deterministic transaction IDs for local transaction cache tests.
   transaction_id_type make_transaction_id(std::string_view label) {
      return transaction_id_type::hash(label.data(), label.size());
   }

   /// Returns an expiration timestamp far enough in the future for cache insertion checks.
   time_point_sec make_expiration() {
      return time_point_sec{fc::time_point::now() + transaction_cache_lifetime};
   }

   /// Returns true when the peer list contains the expected connection ID.
   bool contains_connection(const connection_id_vector& connections, connection_id_t connection_id) {
      return std::find(connections.begin(), connections.end(), connection_id) != connections.end();
   }
} // namespace

BOOST_AUTO_TEST_SUITE(local_txn_cache_tests)

/// Verifies notice-only traffic cannot create local transaction cache entries for unknown IDs.
BOOST_AUTO_TEST_CASE(transaction_notice_ignores_unknown_ids)
{
   local_txn_cache cache;
   const auto      unknown_id = make_transaction_id("unknown transaction notice");

   const auto result = cache.add_transaction_notice(unknown_id, first_connection_id);

   BOOST_CHECK(!result.recorded);
   BOOST_CHECK(result.delta == local_txn_cache::entry_delta::none);
   BOOST_CHECK_EQUAL(cache.size(), 0u);
   BOOST_CHECK(cache.peer_connections(unknown_id).empty());
}

/// Verifies transaction notices only add peer accounting for transaction IDs already known locally.
BOOST_AUTO_TEST_CASE(transaction_notice_records_known_ids)
{
   local_txn_cache cache;
   const auto      known_id = make_transaction_id("known transaction notice");

   const auto transaction_result = cache.add_transaction(known_id, make_expiration(), first_connection_id);
   BOOST_REQUIRE(transaction_result.recorded);
   BOOST_CHECK(transaction_result.delta == local_txn_cache::entry_delta::full);
   BOOST_REQUIRE_EQUAL(cache.size(), 1u);

   const auto notice_result = cache.add_transaction_notice(known_id, second_connection_id);
   BOOST_REQUIRE(notice_result.recorded);
   BOOST_CHECK(notice_result.already_have_trx);
   BOOST_CHECK(notice_result.delta == local_txn_cache::entry_delta::connection);
   BOOST_CHECK_EQUAL(cache.size(), 1u);

   const auto connections = cache.peer_connections(known_id);
   BOOST_REQUIRE_EQUAL(connections.size(), 2u);
   BOOST_CHECK(contains_connection(connections, first_connection_id));
   BOOST_CHECK(contains_connection(connections, second_connection_id));

   const auto duplicate_notice_result = cache.add_transaction_notice(known_id, second_connection_id);
   BOOST_REQUIRE(duplicate_notice_result.recorded);
   BOOST_CHECK(duplicate_notice_result.delta == local_txn_cache::entry_delta::none);
   BOOST_CHECK_EQUAL(cache.size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()
