#include <boost/test/unit_test.hpp>

#include "mocks/mock_depot_ops.hpp"

#include <sysio/batch_operator_plugin/outpost_epoch_lookup.hpp>

using sysio::outbound_envelope_record;
using sysio::test::mock_depot_ops;

BOOST_AUTO_TEST_SUITE(depot_ops_interface_tests)

/// Regression coverage for SEC-7: high-bit slug chain codes must not truncate
/// when building the `byoutepoch` secondary-index exact-match bound.
BOOST_AUTO_TEST_CASE(byoutepoch_find_bound_preserves_slug_high_bits) {
   constexpr uint64_t chain_code = 1ull << 42;
   constexpr uint32_t epoch_index = 9;
   constexpr auto expected_key = "18889465931478580854793";

   BOOST_CHECK_EQUAL(fc::to_string(sysio::batch_operator_detail::outpost_epoch_key(chain_code, epoch_index)),
                     expected_key);
   BOOST_CHECK_EQUAL(sysio::batch_operator_detail::byoutepoch_find_bound(chain_code, epoch_index),
                     "{\"byoutepoch\":\"18889465931478580854793\"}");
}

BOOST_AUTO_TEST_CASE(defaults) {
   mock_depot_ops d;
   BOOST_CHECK_EQUAL(d.within_epoch_window(), true);
   BOOST_CHECK_EQUAL(d.current_epoch(), 1u);
   BOOST_CHECK(!d.read_pending_outbound(0, 1).has_value());
   BOOST_CHECK_EQUAL(d.has_delivered_envelope(0, 1), false);

   BOOST_REQUIRE_EQUAL(d.read_pending_calls.size(), 1u);
   BOOST_CHECK_EQUAL(d.read_pending_calls[0].chain_code, 0u);
   BOOST_CHECK_EQUAL(d.read_pending_calls[0].epoch_index, 1u);
   BOOST_REQUIRE_EQUAL(d.has_delivered_calls.size(), 1u);
}

BOOST_AUTO_TEST_CASE(outbound_envelope_record_round_trip) {
   mock_depot_ops d;
   outbound_envelope_record rec;
   rec.chain_code        = 4;
   rec.epoch_index       = 9;
   rec.envelope_hash_hex = "deadbeef";
   rec.raw_envelope      = {'p', 'b'};

   d.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };

   auto got = d.read_pending_outbound(4, 9);
   BOOST_REQUIRE(got.has_value());
   BOOST_CHECK_EQUAL(got->chain_code, 4u);
   BOOST_CHECK_EQUAL(got->epoch_index, 9u);
   BOOST_CHECK_EQUAL(got->envelope_hash_hex, "deadbeef");
   BOOST_CHECK(got->raw_envelope == rec.raw_envelope);
}

BOOST_AUTO_TEST_CASE(deliver_to_depot_records_payload) {
   mock_depot_ops d;
   std::vector<char> payload{'h', 'i'};
   d.deliver_to_depot(/*chain_code=*/2, payload);
   BOOST_REQUIRE_EQUAL(d.deliver_calls.size(), 1u);
   BOOST_CHECK_EQUAL(d.deliver_calls[0].chain_code, 2u);
   BOOST_CHECK(d.deliver_calls[0].raw_messages == payload);
}

BOOST_AUTO_TEST_CASE(emit_debug_envelope_stores_event) {
   mock_depot_ops d;
   sysio::opp::debugging::DebugEnvelopeEvent ev{
      42ull,
      sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM,
      sysio::chain::name{},
      std::vector<char>{'x'}
   };
   d.emit_debug_envelope(ev);
   BOOST_REQUIRE_EQUAL(d.emitted_events.size(), 1u);
   BOOST_CHECK_EQUAL(std::get<0>(d.emitted_events[0]), 42ull);
}

BOOST_AUTO_TEST_SUITE_END()
