#include <boost/test/unit_test.hpp>

#include <sysio/net_plugin/local_txn_cache.hpp>

#include <algorithm>
#include <string_view>

using namespace sysio;

namespace {
   constexpr connection_id_t first_connection_id = 1;
   constexpr connection_id_t second_connection_id = 2;
   constexpr connection_id_t third_connection_id = 3;
   constexpr auto            transaction_cache_lifetime = fc::seconds(30);
   constexpr std::size_t     notice_only_test_cap = 2;
   constexpr uint32_t        expired_notice_time = 1000;
   constexpr uint32_t        live_transaction_time = 2000;

   /// Builds deterministic transaction IDs for local transaction cache tests.
   transaction_id_type make_transaction_id(std::string_view label) {
      return transaction_id_type::hash(label.data(), label.size());
   }

   /// Returns an expiration timestamp far enough in the future for cache insertion checks.
   time_point_sec make_expiration() {
      return time_point_sec{fc::time_point::now() + transaction_cache_lifetime};
   }

   /// Returns a deterministic notice-only expiration timestamp.
   time_point_sec make_notice_expiration(uint32_t seconds = live_transaction_time) {
      return time_point_sec{seconds};
   }

   /// Returns true when the peer list contains the expected connection ID.
   bool contains_connection(const connection_id_vector& connections, connection_id_t connection_id) {
      return std::find(connections.begin(), connections.end(), connection_id) != connections.end();
   }
} // namespace

BOOST_AUTO_TEST_SUITE(local_txn_cache_tests)

/// Verifies notice-only traffic creates bounded local cache entries for unknown IDs.
BOOST_AUTO_TEST_CASE(transaction_notice_records_unknown_ids_without_validated_accounting)
{
   local_txn_cache cache;
   const auto      unknown_id = make_transaction_id("unknown transaction notice");

   const auto result = cache.add_transaction_notice(unknown_id, make_notice_expiration(), first_connection_id);

   BOOST_REQUIRE(result.recorded);
   BOOST_CHECK(!result.already_have_trx);
   BOOST_CHECK(result.delta == local_txn_cache::entry_delta::none);
   BOOST_CHECK_EQUAL(cache.size(), 1u);
   BOOST_CHECK_EQUAL(cache.notice_only_size(first_connection_id), 1u);

   const auto connections = cache.peer_connections(unknown_id);
   BOOST_REQUIRE_EQUAL(connections.size(), 1u);
   BOOST_CHECK_EQUAL(connections.front(), first_connection_id);
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

   const auto notice_result = cache.add_transaction_notice(known_id, make_notice_expiration(), second_connection_id);
   BOOST_REQUIRE(notice_result.recorded);
   BOOST_CHECK(notice_result.already_have_trx);
   BOOST_CHECK(notice_result.delta == local_txn_cache::entry_delta::connection);
   BOOST_CHECK_EQUAL(cache.size(), 1u);

   const auto third_notice_result = cache.add_transaction_notice(known_id, make_notice_expiration(), third_connection_id);
   BOOST_REQUIRE(third_notice_result.recorded);
   BOOST_CHECK(third_notice_result.already_have_trx);
   BOOST_CHECK(third_notice_result.delta == local_txn_cache::entry_delta::connection);
   BOOST_CHECK_EQUAL(cache.size(), 1u);

   const auto connections = cache.peer_connections(known_id);
   BOOST_REQUIRE_EQUAL(connections.size(), 3u);
   BOOST_CHECK(std::is_sorted(connections.begin(), connections.end()));
   BOOST_CHECK(contains_connection(connections, first_connection_id));
   BOOST_CHECK(contains_connection(connections, second_connection_id));
   BOOST_CHECK(contains_connection(connections, third_connection_id));

   const auto duplicate_notice_result = cache.add_transaction_notice(known_id, make_notice_expiration(), second_connection_id);
   BOOST_REQUIRE(duplicate_notice_result.recorded);
   BOOST_CHECK(duplicate_notice_result.delta == local_txn_cache::entry_delta::none);
   BOOST_CHECK_EQUAL(cache.size(), 1u);
}

/// Verifies full transaction arrival upgrades a notice-only entry and preserves prior notice peers.
BOOST_AUTO_TEST_CASE(transaction_arrival_upgrades_notice_only_entry)
{
   local_txn_cache cache;
   const auto      known_id = make_transaction_id("upgraded transaction notice");

   const auto notice_result = cache.add_transaction_notice(known_id, make_notice_expiration(), first_connection_id);
   BOOST_REQUIRE(notice_result.recorded);
   BOOST_CHECK_EQUAL(cache.notice_only_size(first_connection_id), 1u);

   const auto transaction_result = cache.add_transaction(known_id, make_expiration(), second_connection_id);
   BOOST_REQUIRE(transaction_result.recorded);
   BOOST_CHECK(!transaction_result.already_have_trx);
   BOOST_CHECK(transaction_result.delta == local_txn_cache::entry_delta::full);
   BOOST_CHECK_EQUAL(cache.notice_only_size(first_connection_id), 0u);
   BOOST_CHECK_EQUAL(cache.size(), 1u);

   const auto connections = cache.peer_connections(known_id);
   BOOST_REQUIRE_EQUAL(connections.size(), 2u);
   BOOST_CHECK(contains_connection(connections, first_connection_id));
   BOOST_CHECK(contains_connection(connections, second_connection_id));

   const auto duplicate_transaction_result = cache.add_transaction(known_id, make_expiration(), second_connection_id);
   BOOST_REQUIRE(duplicate_transaction_result.recorded);
   BOOST_CHECK(duplicate_transaction_result.already_have_trx);
   BOOST_CHECK(duplicate_transaction_result.delta == local_txn_cache::entry_delta::none);
}

/// Verifies notice-only entries are capped per connection and evicted in least-recently-used order.
BOOST_AUTO_TEST_CASE(transaction_notice_enforces_per_connection_lru_cap)
{
   local_txn_cache cache{notice_only_test_cap};
   const auto      first_id = make_transaction_id("notice lru first");
   const auto      second_id = make_transaction_id("notice lru second");
   const auto      third_id = make_transaction_id("notice lru third");

   BOOST_REQUIRE(cache.add_transaction_notice(first_id, make_notice_expiration(), first_connection_id).recorded);
   BOOST_REQUIRE(cache.add_transaction_notice(second_id, make_notice_expiration(), first_connection_id).recorded);

   const auto refreshed_first_result = cache.add_transaction_notice(first_id, make_notice_expiration(), first_connection_id);
   BOOST_REQUIRE(refreshed_first_result.recorded);

   BOOST_REQUIRE(cache.add_transaction_notice(third_id, make_notice_expiration(), first_connection_id).recorded);

   BOOST_CHECK_EQUAL(cache.notice_only_size(first_connection_id), notice_only_test_cap);
   BOOST_CHECK_EQUAL(cache.size(), notice_only_test_cap);
   BOOST_CHECK(cache.peer_connections(second_id).empty());
   BOOST_CHECK_EQUAL(cache.peer_connections(first_id).size(), 1u);
   BOOST_CHECK_EQUAL(cache.peer_connections(third_id).size(), 1u);
}

/// Verifies notice-only entries expire independently from longer full-transaction cache entries.
BOOST_AUTO_TEST_CASE(transaction_notice_expires_with_short_notice_cutoff)
{
   local_txn_cache cache;
   const auto      notice_id = make_transaction_id("expired notice");
   const auto      transaction_id = make_transaction_id("live transaction");

   BOOST_REQUIRE(cache.add_transaction_notice(notice_id, make_notice_expiration(expired_notice_time), first_connection_id).recorded);
   BOOST_REQUIRE(cache.add_transaction(transaction_id, make_notice_expiration(live_transaction_time), second_connection_id).recorded);

   const auto removed = cache.expire(make_notice_expiration(expired_notice_time), make_notice_expiration(expired_notice_time));

   BOOST_CHECK_EQUAL(removed, 1u);
   BOOST_CHECK(cache.peer_connections(notice_id).empty());
   BOOST_CHECK_EQUAL(cache.notice_only_size(first_connection_id), 0u);
   BOOST_REQUIRE_EQUAL(cache.peer_connections(transaction_id).size(), 1u);
   BOOST_CHECK_EQUAL(cache.peer_connections(transaction_id).front(), second_connection_id);
}

/// Verifies notice-only caps are isolated per peer.
BOOST_AUTO_TEST_CASE(transaction_notice_cap_is_per_connection)
{
   local_txn_cache cache{notice_only_test_cap};
   const auto      first_id = make_transaction_id("first connection notice");
   const auto      second_id = make_transaction_id("second connection notice");
   const auto      third_id = make_transaction_id("third connection notice");

   BOOST_REQUIRE(cache.add_transaction_notice(first_id, make_notice_expiration(), first_connection_id).recorded);
   BOOST_REQUIRE(cache.add_transaction_notice(second_id, make_notice_expiration(), first_connection_id).recorded);
   BOOST_REQUIRE(cache.add_transaction_notice(third_id, make_notice_expiration(), second_connection_id).recorded);
   BOOST_REQUIRE(cache.add_transaction_notice(third_id, make_notice_expiration(), third_connection_id).recorded);

   BOOST_CHECK_EQUAL(cache.notice_only_size(first_connection_id), notice_only_test_cap);
   BOOST_CHECK_EQUAL(cache.notice_only_size(second_connection_id), 1u);
   BOOST_CHECK_EQUAL(cache.notice_only_size(third_connection_id), 1u);
   BOOST_CHECK_EQUAL(cache.size(), 3u);
}

BOOST_AUTO_TEST_SUITE_END()
