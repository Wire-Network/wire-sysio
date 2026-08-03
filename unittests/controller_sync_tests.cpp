/**
 * @file controller_sync_tests.cpp
 * @brief Unit tests for `controller::is_synced()` — the LIB-recency predicate operator-daemon
 *        plugins gate their deferred startup on.
 *
 * The boundary cases drive the testable overload `is_synced(now, recency_window)` with instants
 * chosen relative to the tester chain's ACTUAL last-irreversible block time, so every comparison
 * exercises the real controller code path deterministically (no wall clock). The two wall-clock
 * cases at the end cover the production convenience overload.
 */
#include <boost/test/unit_test.hpp>

#include <sysio/chain/controller.hpp>
#include <sysio/testing/tester.hpp>

using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(controller_sync_tests)

/// A produced tester chain plus its last-irreversible block time — the reference instant
/// every boundary case below offsets `now` from.
struct lib_fixture {
   tester         chain;
   fc::time_point lib_time;
   /// The default gate window the operator-daemon plugins run with.
   fc::microseconds default_window = fc::milliseconds(controller::default_sync_recency_ms);

   lib_fixture() {
      chain.produce_blocks(4);
      BOOST_REQUIRE(chain.control->fork_db_has_root());
      lib_time = chain.control->fork_db_root().block_time();
   }
};

BOOST_FIXTURE_TEST_CASE(lib_at_now_is_synced, lib_fixture) { try {
   // The identity point of the window arithmetic: now == LIB time (zero gap).
   BOOST_TEST(chain.control->is_synced(lib_time, default_window));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(lib_one_block_interval_behind_is_synced, lib_fixture) { try {
   // One block interval behind now — the steady-state of a caught-up node.
   BOOST_TEST(chain.control->is_synced(lib_time + fc::milliseconds(500), default_window));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(lib_exactly_at_window_boundary_is_synced, lib_fixture) { try {
   BOOST_TEST(chain.control->is_synced(lib_time + default_window, default_window));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(lib_just_past_window_is_not_synced, lib_fixture) { try {
   BOOST_TEST(!chain.control->is_synced(lib_time + default_window + fc::microseconds(1),
                                        default_window));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(cold_boot_replay_gap_is_not_synced, lib_fixture) { try {
   // A node mid-replay: LIB hours behind wall clock — the exact state whose genesis-era
   // table reads used to false-fail operator-daemon startups.
   BOOST_TEST(!chain.control->is_synced(lib_time + fc::hours(2), default_window));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(lib_ahead_of_now_is_synced, lib_fixture) { try {
   // Clock skew / LIB stamped marginally ahead of the observer's clock must never read
   // as "behind".
   BOOST_TEST(chain.control->is_synced(lib_time - fc::seconds(1), default_window));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(zero_window_requires_lib_at_or_past_now, lib_fixture) { try {
   BOOST_TEST(chain.control->is_synced(lib_time, fc::microseconds(0)));
   BOOST_TEST(!chain.control->is_synced(lib_time + fc::microseconds(1), fc::microseconds(0)));
} FC_LOG_AND_RETHROW() }

// ── the wall-clock convenience overload the plugins call ──

BOOST_AUTO_TEST_CASE(stale_chain_is_not_synced) { try {
   // The tester genesis timestamp is a fixed instant years in the past, so a freshly
   // produced chain's irreversible root time trails wall-clock now by that whole gap —
   // the cold-boot state the predicate exists to detect.
   tester chain;
   chain.produce_blocks(4);
   BOOST_REQUIRE(chain.control->fork_db_has_root());
   BOOST_TEST(!chain.control->is_synced());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(fresh_chain_is_synced) { try {
   // Jump block time from the fixed genesis instant to wall-clock now, then produce a few
   // more blocks so the near-now block becomes irreversible. The subsequent block times
   // land at/ahead of now (ahead counts as recent — clock-skew rule above), so the
   // LIB-recency predicate must hold.
   tester chain;
   chain.produce_block();
   chain.produce_block(fc::time_point::now() - chain.control->head().block_time());
   chain.produce_blocks(6);
   BOOST_REQUIRE(chain.control->fork_db_has_root());
   BOOST_TEST(chain.control->is_synced());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
