/**
 * @file test_sync_gate.cpp
 * @brief Unit tests for the chain_plugin sync gate (`sysio/chain_plugin/sync_gate.hpp`)
 *        — the recency predicate behind `chain_plugin::is_synced()` and the
 *        stale→synced edge detector behind the `chain_plugin::synced()` signal.
 *
 * The pure-predicate cases migrated verbatim from the underwriter plugin's private
 * gate (`test_underwriter_sync.cpp`) when the gate was promoted to chain_plugin.
 */
#include <boost/test/unit_test.hpp>

#include <sysio/chain_plugin/sync_gate.hpp>
#include <sysio/testing/tester.hpp>

using namespace sysio::testing;
using sysio::chain_apis::block_time_is_recent;
using sysio::chain_apis::lib_time_is_recent;
using sysio::chain_apis::sync_transition;

BOOST_AUTO_TEST_SUITE(sync_gate_tests)

/// The reference "now" every pure case offsets from — an arbitrary fixed instant so
/// the tests are deterministic (no wall clock).
static const fc::time_point reference_now =
   fc::time_point{} + fc::days(365 * 50);

/// The default gate window the plugin runs with.
static const fc::microseconds default_window =
   fc::milliseconds(sysio::chain_apis::default_sync_recency_ms);

// ── block_time_is_recent: the pure recency predicate ──

BOOST_AUTO_TEST_CASE(head_at_now_is_synced) {
   BOOST_TEST(block_time_is_recent(reference_now, reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(head_slightly_behind_within_window_is_synced) {
   // One block interval behind — the steady-state of a caught-up node.
   BOOST_TEST(block_time_is_recent(reference_now - fc::milliseconds(500),
                                   reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(head_exactly_at_window_boundary_is_synced) {
   BOOST_TEST(block_time_is_recent(reference_now - default_window,
                                   reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(head_just_past_window_is_not_synced) {
   BOOST_TEST(!block_time_is_recent(reference_now - default_window - fc::microseconds(1),
                                    reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(cold_boot_replay_gap_is_not_synced) {
   // A node mid-replay: head hours behind wall clock — the exact state whose
   // genesis-era table reads used to false-fail operator-daemon startups.
   BOOST_TEST(!block_time_is_recent(reference_now - fc::hours(2),
                                    reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(head_ahead_of_now_is_synced) {
   // Clock skew / head stamped marginally ahead of the observer's clock must
   // never read as "behind".
   BOOST_TEST(block_time_is_recent(reference_now + fc::seconds(1),
                                   reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(zero_window_requires_head_at_or_past_now) {
   BOOST_TEST(block_time_is_recent(reference_now, reference_now, fc::microseconds(0)));
   BOOST_TEST(!block_time_is_recent(reference_now - fc::microseconds(1),
                                    reference_now, fc::microseconds(0)));
}

// ── sync_transition: the stale→synced edge detector behind `synced()` ──

BOOST_AUTO_TEST_CASE(transition_initially_stale_never_fires_while_stale) {
   sync_transition transition;
   BOOST_TEST(!transition.update(false));
   BOOST_TEST(!transition.update(false));
}

BOOST_AUTO_TEST_CASE(transition_fires_exactly_once_on_first_sync) {
   sync_transition transition;
   BOOST_TEST(!transition.update(false));
   BOOST_TEST(transition.update(true));
   // Staying synced is not a new transition.
   BOOST_TEST(!transition.update(true));
   BOOST_TEST(!transition.update(true));
}

BOOST_AUTO_TEST_CASE(transition_fires_when_first_observation_is_synced) {
   // Initial state is stale by contract, so a node that is synced at the very
   // first evaluation (e.g. fresh genesis) still gets its emission.
   sync_transition transition;
   BOOST_TEST(transition.update(true));
}

BOOST_AUTO_TEST_CASE(transition_rearms_after_going_stale) {
   // Re-arm semantics: a node that stalls and later re-syncs transitions again —
   // there is deliberately no once-per-process latch (a latch that fired with no
   // consumer connected would leave a later subscriber waiting forever).
   sync_transition transition;
   BOOST_TEST(transition.update(true));
   BOOST_TEST(!transition.update(false));
   BOOST_TEST(transition.update(true));
   BOOST_TEST(!transition.update(true));
}

// ── lib_time_is_recent: the controller-backed predicate `is_synced()` delegates to ──

BOOST_AUTO_TEST_CASE(stale_chain_is_not_synced) { try {
   // The tester genesis timestamp is a fixed instant years in the past, so a
   // freshly-produced chain's irreversible root time trails wall-clock now by
   // that whole gap — the cold-boot replay state the gate exists to detect.
   tester chain;
   chain.produce_blocks(4);
   BOOST_REQUIRE(chain.control->fork_db_has_root());
   BOOST_TEST(!lib_time_is_recent(*chain.control, fc::time_point::now(), default_window));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(fresh_chain_is_synced) { try {
   // Jump block time from the fixed genesis instant to wall-clock now, then
   // produce a few more blocks so the near-now block becomes irreversible. The
   // subsequent block times land at/ahead of now (ahead counts as recent —
   // clock-skew rule above), so the LIB-recency predicate must hold.
   tester chain;
   chain.produce_block();
   chain.produce_block(fc::time_point::now() - chain.control->head().block_time());
   chain.produce_blocks(6);
   BOOST_REQUIRE(chain.control->fork_db_has_root());
   BOOST_TEST(lib_time_is_recent(*chain.control, fc::time_point::now(), default_window));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
