/**
 * @file test_underwriter_sync.cpp
 * @brief Unit tests for the underwriter plugin's sync-gate predicate
 *        (`sysio::underwriter_detail::head_is_recent`) — the pure decision
 *        behind deferring preflight until the chain plugin reports the node
 *        synced.
 */
#include <boost/test/unit_test.hpp>

#include <sysio/underwriter_plugin/sync_detail.hpp>
#include <sysio/underwriter_plugin/underwriter_plugin.hpp>

using sysio::underwriter_detail::head_is_recent;

BOOST_AUTO_TEST_SUITE(underwriter_sync_tests)

/// The reference "now" every case offsets from — an arbitrary fixed instant so
/// the tests are deterministic (no wall clock).
static const fc::time_point reference_now =
   fc::time_point{} + fc::days(365 * 50);

/// The default gate window the plugin runs with.
static const fc::microseconds default_window =
   fc::milliseconds(sysio::underwriter_defaults::sync_recency_ms);

BOOST_AUTO_TEST_CASE(head_at_now_is_synced) {
   BOOST_TEST(head_is_recent(reference_now, reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(head_slightly_behind_within_window_is_synced) {
   // One block interval behind — the steady-state of a caught-up node.
   BOOST_TEST(head_is_recent(reference_now - fc::milliseconds(500),
                             reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(head_exactly_at_window_boundary_is_synced) {
   BOOST_TEST(head_is_recent(reference_now - default_window,
                             reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(head_just_past_window_is_not_synced) {
   BOOST_TEST(!head_is_recent(reference_now - default_window - fc::microseconds(1),
                              reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(cold_boot_replay_gap_is_not_synced) {
   // A node mid-replay: head hours behind wall clock — the exact state whose
   // genesis-era table reads used to false-fail the preflight.
   BOOST_TEST(!head_is_recent(reference_now - fc::hours(2),
                              reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(head_ahead_of_now_is_synced) {
   // Clock skew / head stamped marginally ahead of the observer's clock must
   // never read as "behind".
   BOOST_TEST(head_is_recent(reference_now + fc::seconds(1),
                             reference_now, default_window));
}

BOOST_AUTO_TEST_CASE(zero_window_requires_head_at_or_past_now) {
   BOOST_TEST(head_is_recent(reference_now, reference_now, fc::microseconds(0)));
   BOOST_TEST(!head_is_recent(reference_now - fc::microseconds(1),
                              reference_now, fc::microseconds(0)));
}

BOOST_AUTO_TEST_SUITE_END()
