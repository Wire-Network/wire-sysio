#include <boost/test/unit_test.hpp>
#include "external_chain_simulator.hpp"

using namespace sysio::testing::external;
using namespace sysio::opp;

BOOST_AUTO_TEST_SUITE(external_chain_simulator_tests)

BOOST_AUTO_TEST_CASE(custody_reports_share_a_monotonic_sequence_and_keep_chains_independent) {
   chain first(First), second(Second);
   const std::vector<char> key(33, 'a');
   attestations::SyndicateLIQ deposit;
   BOOST_REQUIRE(deposit.ParseFromString(first.deposit(key, Unit).second));
   attestations::LIQYield yield;
   BOOST_REQUIRE(yield.ParseFromString(first.yield(Unit).second));
   BOOST_CHECK_EQUAL(deposit.total_syndicated(), Unit);
   BOOST_CHECK_EQUAL(yield.total_syndicated(), 2 * Unit);
   BOOST_CHECK_EQUAL(deposit.sequence(), 1);
   BOOST_CHECK_EQUAL(yield.sequence(), 2);
   BOOST_CHECK_EQUAL(second.custody(), 0);
   BOOST_CHECK_EQUAL(second.sequence(), 0);
   BOOST_CHECK_THROW(second.lose(Unit), fc::exception);
}

BOOST_AUTO_TEST_CASE(envelopes_continue_both_canonical_hash_chains) {
   chain outpost(First);
   const auto one = outpost.envelope(1, {outpost.yield(Unit)});
   const auto two = outpost.envelope(2, {outpost.yield(Unit)});
   Envelope a, b;
   BOOST_REQUIRE(a.ParseFromArray(one.data(), one.size()));
   BOOST_REQUIRE(b.ParseFromArray(two.data(), two.size()));
   BOOST_CHECK_EQUAL(b.previous_envelope_hash(), oracle::digest_bytes(oracle::epoch_digest(a)));
   BOOST_CHECK_EQUAL(b.messages(0).header().previous_message_id(), a.messages(0).header().message_id());
}

BOOST_AUTO_TEST_CASE(frozen_returns_are_pending_and_settle_exactly_once) {
   chain outpost(First);
   outpost.donate(3 * Unit);
   attestations::DesyndicateLIQ message;
   message.set_request_id(1);
   message.set_chain_code(fc::slug_name{First.chain}.value);
   message.mutable_amount()->set_token_code(fc::slug_name{First.token}.value);
   message.mutable_amount()->set_amount(Unit);
   outpost.freeze();
   BOOST_CHECK(!outpost.receive(message));
   BOOST_CHECK(!outpost.receive(message));
   BOOST_CHECK_EQUAL(outpost.pending_count(), 1);
   BOOST_CHECK_EQUAL(outpost.custody(), 3 * Unit);
   auto conflict = message;
   conflict.mutable_amount()->set_amount(2 * Unit);
   BOOST_CHECK_THROW(outpost.receive(conflict), fc::exception);
   outpost.clear();
   BOOST_CHECK(outpost.settle(1));
   BOOST_CHECK(!outpost.receive(message));
   BOOST_CHECK_EQUAL(outpost.custody(), 2 * Unit);
   BOOST_CHECK_EQUAL(outpost.paid(), Unit);
   BOOST_CHECK_EQUAL(outpost.pending_count(), 0);
   BOOST_CHECK_THROW(outpost.receive(conflict), fc::exception);
   message.set_chain_code(fc::slug_name{Second.chain}.value);
   BOOST_CHECK_THROW(outpost.receive(message), fc::exception);
}
BOOST_AUTO_TEST_CASE(invalid_reports_do_not_mutate_custody_or_cursors) {
   chain outpost(First);
   const std::vector<char> key(33, 'a');
   BOOST_CHECK_THROW(outpost.deposit(key, 0), fc::exception);
   BOOST_CHECK_THROW(outpost.yield(std::numeric_limits<uint64_t>::max()), fc::exception);
   BOOST_CHECK_EQUAL(outpost.custody(), 0);
   BOOST_CHECK_EQUAL(outpost.sequence(), 0);
   outpost.donate(std::numeric_limits<uint64_t>::max());
   BOOST_CHECK_THROW(outpost.deposit(key, Unit), fc::exception);
   BOOST_CHECK_EQUAL(outpost.custody(), std::numeric_limits<uint64_t>::max());
   BOOST_CHECK_EQUAL(outpost.sequence(), 0);
}
BOOST_AUTO_TEST_SUITE_END()
