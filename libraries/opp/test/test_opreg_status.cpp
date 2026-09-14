/// Pure-logic unit tests for `sysio::opp::depot::opreg_status::compute_is_active`.
///
/// Exercises the awareness decision table consumed by `batch_operator_plugin`
/// and `underwriter_plugin` each tick. The actual chain read happens through
/// `chain_plugin::read_table_rows` (integration territory; covered by the
/// flow tests in `wire-tools-ts`); this test pins the string-to-decision
/// mapping in isolation so a refactor that drifts the spellings or the
/// fall-through behavior fails here first.

#include <boost/test/unit_test.hpp>

#include <sysio/opp/depot/opreg_status.hpp>

namespace s = sysio::opp::depot::opreg_status;

BOOST_AUTO_TEST_SUITE(opreg_status_tests)

BOOST_AUTO_TEST_CASE(active_status_marks_operator_active_regardless_of_previous) {
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active(s::active, /*previous=*/true));
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active(s::active, /*previous=*/false));
}

BOOST_AUTO_TEST_CASE(slashed_status_halts_relay_regardless_of_previous) {
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(s::slashed, /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(s::slashed, /*previous=*/false));
}

BOOST_AUTO_TEST_CASE(terminated_status_halts_relay_regardless_of_previous) {
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(s::terminated, /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(s::terminated, /*previous=*/false));
}

/// The transient states an operator actually passes through. The relay loop
/// relies on these preserving the flag so a mid-warmup or mid-cooldown tick
/// doesn't push a still-eligible operator offline.
BOOST_AUTO_TEST_CASE(transient_status_preserves_previous_value) {
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active("OPERATOR_STATUS_WARMUP",   /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active("OPERATOR_STATUS_WARMUP",   /*previous=*/false));
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active("OPERATOR_STATUS_COOLDOWN", /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active("OPERATOR_STATUS_COOLDOWN", /*previous=*/false));
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active("OPERATOR_STATUS_UNKNOWN",  /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active("OPERATOR_STATUS_UNKNOWN",  /*previous=*/false));
}

/// A spelling the enum does not carry — a stale read, or a name from a proto
/// revision this build doesn't have — must also preserve the flag rather than
/// read as a terminal state.
BOOST_AUTO_TEST_CASE(unrecognized_spelling_preserves_previous_value) {
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active("",                         /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active("",                         /*previous=*/false));
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active("OPERATOR_STATUS_STANDBY",  /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active("OPERATOR_STATUS_STANDBY",  /*previous=*/false));
}

/// Spelling regression guard — the constants must match the protobuf
/// `OperatorStatus` enum exactly. If `protoc` ever renames or reorders
/// these spellings (e.g. dropping the `OPERATOR_STATUS_` prefix), this
/// test catches the divergence before it lands in production.
BOOST_AUTO_TEST_CASE(canonical_spellings_match_protobuf_enum_names) {
   BOOST_REQUIRE_EQUAL(std::string{"OPERATOR_STATUS_ACTIVE"},     std::string{s::active});
   BOOST_REQUIRE_EQUAL(std::string{"OPERATOR_STATUS_SLASHED"},    std::string{s::slashed});
   BOOST_REQUIRE_EQUAL(std::string{"OPERATOR_STATUS_TERMINATED"}, std::string{s::terminated});
}

BOOST_AUTO_TEST_SUITE_END()
