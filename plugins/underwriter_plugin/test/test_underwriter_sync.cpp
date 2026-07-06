/**
 * @file test_underwriter_sync.cpp
 * @brief Unit tests for the underwriter plugin's sync-gate predicate
 *        (`sysio::underwriter_detail::block_time_is_recent`) — the pure decision
 *        behind deferring preflight until the chain plugin reports the node
 *        synced.
 */
#include <boost/test/unit_test.hpp>

#include <sysio/underwriter_plugin/sync_detail.hpp>
#include <sysio/underwriter_plugin/underwriter_plugin.hpp>

using sysio::underwriter_detail::block_time_is_recent;

BOOST_AUTO_TEST_SUITE(underwriter_sync_tests)

/// The reference "now" every case offsets from — an arbitrary fixed instant so
/// the tests are deterministic (no wall clock).
static const fc::time_point reference_now =
   fc::time_point{} + fc::days(365 * 50);

/// The default gate window the plugin runs with.
static const fc::microseconds default_window =
   fc::milliseconds(sysio::underwriter_defaults::sync_recency_ms);

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
   // genesis-era table reads used to false-fail the preflight.
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

// ── startup_gate_payload: the /v1/underwriter/* body served until the ──
// ── deferred startup body completes (registration is unconditional at ──
// ── plugin_startup, so these payloads are what a cold-boot query sees) ──
//
// Wire-format pinning: the static_asserts below pin each shared `field::`
// KEY constant to its literal wire spelling once (a typo'd or renamed
// constant would otherwise change the HTTP format with builder and tests
// agreeing tautologically); every assertion after that is free to use the
// constants. The `status` VALUES stay literal in the cases for the same
// reason — an enum-member rename that would silently change the wire format
// fails here.

using sysio::underwriter_detail::startup_gate_payload;
using sysio::underwriter_detail::startup_state;
namespace field = sysio::underwriter_detail::field;

static_assert(std::string_view{field::status} == "status");
static_assert(std::string_view{field::head_behind_sec} == "head_behind_sec");
static_assert(std::string_view{field::lib_behind_sec} == "lib_behind_sec");
static_assert(std::string_view{field::detail} == "detail");

BOOST_AUTO_TEST_CASE(gate_payload_waiting_for_sync_reports_behind_gaps) {
   const auto body = startup_gate_payload(startup_state::waiting_for_sync,
                                          fc::seconds(204), fc::seconds(206))
                        .get_object();
   BOOST_TEST(body[field::status].as_string() == "waiting_for_sync");
   BOOST_TEST(body[field::head_behind_sec].as_int64() == 204);
   BOOST_TEST(body[field::lib_behind_sec].as_int64() == 206);
   BOOST_TEST(!body.contains(field::detail));
}

BOOST_AUTO_TEST_CASE(gate_payload_no_root_omits_lib_gap) {
   // Before the fork database has a root the caller passes an empty optional
   // and the key is omitted — an in-band sentinel would collide with real
   // negative (clock-skew) gaps after whole-second truncation.
   const auto body = startup_gate_payload(startup_state::waiting_for_sync,
                                          fc::seconds(3), std::nullopt)
                        .get_object();
   BOOST_TEST(body[field::head_behind_sec].as_int64() == 3);
   BOOST_TEST(!body.contains(field::lib_behind_sec));
}

BOOST_AUTO_TEST_CASE(gate_payload_passes_negative_skew_gaps_through) {
   // A block time ahead of local wall clock (clock skew) yields a negative
   // gap; it is reported as-is — the skew is itself diagnostic signal.
   const auto body = startup_gate_payload(startup_state::waiting_for_sync,
                                          fc::milliseconds(-1500), fc::milliseconds(-1500))
                        .get_object();
   BOOST_TEST(body[field::head_behind_sec].as_int64() == -1);
   BOOST_TEST(body[field::lib_behind_sec].as_int64() == -1);
}

BOOST_AUTO_TEST_CASE(gate_payload_truncates_to_whole_seconds) {
   // Durations travel as fc::microseconds; the JSON boundary emits whole
   // seconds.
   const auto body = startup_gate_payload(startup_state::waiting_for_sync,
                                          fc::milliseconds(2500), fc::milliseconds(1999))
                        .get_object();
   BOOST_TEST(body[field::head_behind_sec].as_int64() == 2);
   BOOST_TEST(body[field::lib_behind_sec].as_int64() == 1);
}

BOOST_AUTO_TEST_CASE(gate_payload_preflight_failed_carries_detail) {
   const auto body = startup_gate_payload(startup_state::preflight_failed,
                                          fc::microseconds(0), std::nullopt)
                        .get_object();
   BOOST_TEST(body[field::status].as_string() == "preflight_failed");
   BOOST_TEST(body[field::detail].as_string() ==
              std::string{sysio::underwriter_detail::preflight_failed_detail});
   BOOST_TEST(!body.contains(field::head_behind_sec));
   BOOST_TEST(!body.contains(field::lib_behind_sec));
}

BOOST_AUTO_TEST_CASE(gate_payload_wiring_failed_carries_detail) {
   const auto body = startup_gate_payload(startup_state::wiring_failed,
                                          fc::microseconds(0), std::nullopt)
                        .get_object();
   BOOST_TEST(body[field::status].as_string() == "wiring_failed");
   BOOST_TEST(body[field::detail].as_string() ==
              std::string{sysio::underwriter_detail::wiring_failed_detail});
   BOOST_TEST(!body.contains(field::head_behind_sec));
   BOOST_TEST(!body.contains(field::lib_behind_sec));
}

BOOST_AUTO_TEST_CASE(gate_payload_startup_failed_carries_detail) {
   // The catch-all terminal state: the deferred startup body threw past the
   // specific preflight/wiring failure paths.
   const auto body = startup_gate_payload(startup_state::startup_failed,
                                          fc::microseconds(0), std::nullopt)
                        .get_object();
   BOOST_TEST(body[field::status].as_string() == "startup_failed");
   BOOST_TEST(body[field::detail].as_string() ==
              std::string{sysio::underwriter_detail::startup_failed_detail});
   BOOST_TEST(!body.contains(field::head_behind_sec));
   BOOST_TEST(!body.contains(field::lib_behind_sec));
}

BOOST_AUTO_TEST_CASE(gate_payload_preflight_retrying_is_status_only) {
   const auto body =
      startup_gate_payload(startup_state::preflight_retrying, fc::microseconds(0), std::nullopt)
         .get_object();
   BOOST_TEST(body[field::status].as_string() == "preflight_retrying");
   BOOST_TEST(body.size() == 1u);
}

BOOST_AUTO_TEST_CASE(gate_payload_active_is_status_only) {
   const auto body = startup_gate_payload(startup_state::active,
                                          fc::seconds(99), fc::seconds(99))
                        .get_object();
   BOOST_TEST(body[field::status].as_string() == "active");
   BOOST_TEST(body.size() == 1u);
}

// `active_status` is pinned at compile time next to its definition in
// sync_detail.hpp (static_assert), so no runtime case is needed for it.

BOOST_AUTO_TEST_SUITE_END()
