/// Pure-logic unit tests for `sysio::opp::depot::opreg_status::compute_is_active`.
///
/// Exercises the awareness decision table consumed by `batch_operator_plugin`
/// and `underwriter_plugin` each tick. The actual chain read happens through
/// `chain_plugin::read_table_rows` (integration territory; covered by the
/// flow tests in `wire-tools-ts`); this test pins the status-to-decision
/// mapping in isolation.
///
/// Every status is fed in as the descriptor's own name, so the cases exercise
/// the same spellings the ABI serializer emits rather than copies of them.

#include <boost/test/unit_test.hpp>

#include <sysio/opp/depot/opreg_status.hpp>

#include <string>

namespace s = sysio::opp::depot::opreg_status;
namespace t = sysio::opp::types;

BOOST_AUTO_TEST_SUITE(opreg_status_tests)

namespace {
/// The status string as the depot's ABI serializer spells it.
const std::string& name_of(t::OperatorStatus value) { return t::OperatorStatus_Name(value); }
} // namespace

BOOST_AUTO_TEST_CASE(active_status_marks_operator_active_regardless_of_previous) {
   BOOST_REQUIRE_EQUAL(true, s::compute_is_active(name_of(t::OPERATOR_STATUS_ACTIVE), /*previous=*/true));
   BOOST_REQUIRE_EQUAL(true, s::compute_is_active(name_of(t::OPERATOR_STATUS_ACTIVE), /*previous=*/false));
}

BOOST_AUTO_TEST_CASE(slashed_status_halts_relay_regardless_of_previous) {
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(name_of(t::OPERATOR_STATUS_SLASHED), /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(name_of(t::OPERATOR_STATUS_SLASHED), /*previous=*/false));
}

BOOST_AUTO_TEST_CASE(terminated_status_halts_relay_regardless_of_previous) {
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(name_of(t::OPERATOR_STATUS_TERMINATED), /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(name_of(t::OPERATOR_STATUS_TERMINATED), /*previous=*/false));
}

/// The transient states an operator actually passes through. The relay loop
/// relies on these preserving the flag so a mid-warmup or mid-cooldown tick
/// doesn't push a still-eligible operator offline.
BOOST_AUTO_TEST_CASE(transient_status_preserves_previous_value) {
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active(name_of(t::OPERATOR_STATUS_WARMUP),   /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(name_of(t::OPERATOR_STATUS_WARMUP),   /*previous=*/false));
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active(name_of(t::OPERATOR_STATUS_COOLDOWN), /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(name_of(t::OPERATOR_STATUS_COOLDOWN), /*previous=*/false));
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active(name_of(t::OPERATOR_STATUS_UNKNOWN),  /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active(name_of(t::OPERATOR_STATUS_UNKNOWN),  /*previous=*/false));
}

/// A spelling the enum does not carry — a stale read, or a name from a proto
/// revision this build doesn't have — must also preserve the flag rather than
/// read as a terminal state.
BOOST_AUTO_TEST_CASE(unrecognized_spelling_preserves_previous_value) {
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active("",                        /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active("",                        /*previous=*/false));
   BOOST_REQUIRE_EQUAL(true,  s::compute_is_active("OPERATOR_STATUS_STANDBY", /*previous=*/true));
   BOOST_REQUIRE_EQUAL(false, s::compute_is_active("OPERATOR_STATUS_STANDBY", /*previous=*/false));
}

/// Coverage guard: every value the proto declares must be decided by one of
/// the cases above. A new OperatorStatus lands here as a failure, forcing the
/// author to choose its relay semantics rather than inheriting `previous` by
/// silent default.
BOOST_AUTO_TEST_CASE(every_declared_status_is_parsed_and_accounted_for) {
   const auto* descriptor = t::OperatorStatus_descriptor();
   BOOST_REQUIRE_EQUAL(6, descriptor->value_count());

   for (int i = 0; i < descriptor->value_count(); ++i) {
      const auto& spelling = descriptor->value(i)->name();

      t::OperatorStatus parsed{};
      BOOST_REQUIRE_MESSAGE(t::OperatorStatus_Parse(spelling, &parsed),
                            "proto value did not parse: " << spelling);

      // Decided (independent of `previous`) iff terminal or active.
      const bool decided = s::compute_is_active(spelling, true) == s::compute_is_active(spelling, false);
      const bool terminal_or_active = parsed == t::OPERATOR_STATUS_ACTIVE
                                   || parsed == t::OPERATOR_STATUS_TERMINATED
                                   || parsed == t::OPERATOR_STATUS_SLASHED;
      BOOST_REQUIRE_MESSAGE(decided == terminal_or_active,
                            "unclassified OperatorStatus: " << spelling);
   }
}

BOOST_AUTO_TEST_SUITE_END()
