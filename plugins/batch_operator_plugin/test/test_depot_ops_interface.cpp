#include <boost/test/unit_test.hpp>

#include "mocks/mock_depot_ops.hpp"

#include <sysio/batch_operator_plugin/outpost_epoch_lookup.hpp>

#include <fc/variant_object.hpp>

#include <limits>
#include <string>
#include <string_view>

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

/// A chain epoch's `byoutepoch` range runs from its own key to the next key, exclusive: the next epoch of the chain,
/// or the next chain's epoch 0 after the last epoch.
BOOST_AUTO_TEST_CASE(byoutepoch_range_spans_one_chain_epoch) {
   namespace detail = sysio::batch_operator_detail;
   constexpr uint64_t chain_code  = 1ull << 42;
   constexpr uint32_t epoch_index = 9;
   const detail::index_range range = detail::byoutepoch_range(chain_code, epoch_index);
   BOOST_CHECK_EQUAL(range.lower_bound, detail::byoutepoch_find_bound(chain_code, epoch_index));
   BOOST_CHECK_EQUAL(range.upper_bound, detail::byoutepoch_find_bound(chain_code, epoch_index + 1));

   const detail::index_range last = detail::byoutepoch_range(chain_code, std::numeric_limits<uint32_t>::max());
   BOOST_CHECK_EQUAL(last.upper_bound, detail::byoutepoch_find_bound(chain_code + 1, 0));
}

/// Every deliverer's row of a chain epoch is searched: the operator is found whether it delivered first or later.
BOOST_AUTO_TEST_CASE(delivered_by_searches_every_deliverer) {
   namespace detail = sysio::batch_operator_detail;
   const auto row = [](std::string_view batch_op) {
      return fc::variant(fc::mutable_variant_object()(detail::envelope_batch_op_field, std::string(batch_op)));
   };
   const sysio::chain::name self{"batchop.b"};
   BOOST_CHECK(detail::delivered_by({row("batchop.a"), row("batchop.b")}, self));
   BOOST_CHECK(detail::delivered_by({row("batchop.b"), row("batchop.a")}, self));
   BOOST_CHECK(!detail::delivered_by({row("batchop.a"), row("batchop.c")}, self));
   BOOST_CHECK(!detail::delivered_by({}, self));
}

BOOST_AUTO_TEST_CASE(defaults) {
   mock_depot_ops d;
   BOOST_CHECK_EQUAL(d.is_elected(0), true);
   BOOST_CHECK_EQUAL(d.current_epoch(0), 1u);
   BOOST_CHECK_EQUAL(d.is_decided(0), false);
   BOOST_CHECK_EQUAL(d.has_traffic(0), false);
   BOOST_CHECK(d.minimum_duration(0) == mock_depot_ops::default_minimum_duration);
   BOOST_CHECK(d.delivery_timeout(0) == mock_depot_ops::default_delivery_timeout);
   BOOST_CHECK(!d.read_pending_outbound(0, 1).has_value());
   BOOST_CHECK_EQUAL(d.has_delivered_envelope(0, 1), false);
   BOOST_CHECK_EQUAL(d.has_reported_backlog(0, 1), false);

   BOOST_REQUIRE_EQUAL(d.read_pending_calls.size(), 1u);
   BOOST_CHECK_EQUAL(d.read_pending_calls[0].chain_code, 0u);
   BOOST_CHECK_EQUAL(d.read_pending_calls[0].epoch_index, 1u);
   BOOST_REQUIRE_EQUAL(d.has_delivered_calls.size(), 1u);
   BOOST_REQUIRE_EQUAL(d.has_reported_calls.size(), 1u);
   BOOST_CHECK(d.chkcons_if_due_calls.empty());
   BOOST_CHECK(d.report_calls.empty());
}

/// Each epoch query answers for the chain it names: a chain's own knobs override the defaults, and every query
/// records the chain it asked about.
BOOST_AUTO_TEST_CASE(epoch_queries_answer_per_chain) {
   constexpr uint64_t own_chain     = 2;
   constexpr uint64_t default_chain = 3;
   constexpr uint32_t own_epoch     = 9;
   const fc::microseconds own_minimum = fc::seconds(30);
   mock_depot_ops d;
   d.chains[own_chain] = mock_depot_ops::chain_knobs{.elected               = false,
                                                     .epoch                 = own_epoch,
                                                     .decided               = true,
                                                     .traffic_recorded      = true,
                                                     .chain_minimum         = own_minimum};

   BOOST_CHECK_EQUAL(d.current_epoch(own_chain), own_epoch);
   BOOST_CHECK_EQUAL(d.is_elected(own_chain), false);
   BOOST_CHECK_EQUAL(d.is_decided(own_chain), true);
   BOOST_CHECK_EQUAL(d.has_traffic(own_chain), true);
   BOOST_CHECK(d.minimum_duration(own_chain) == own_minimum);
   BOOST_CHECK_EQUAL(d.current_epoch(default_chain), d.epoch);
   BOOST_CHECK_EQUAL(d.is_elected(default_chain), d.elected);

   const std::vector<uint64_t> expected{own_chain, own_chain, own_chain, own_chain, own_chain, default_chain,
                                        default_chain};
   BOOST_CHECK(d.epoch_queries == expected);
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
   BOOST_CHECK(d.deliver_to_depot(/*chain_code=*/2, payload));
   d.deliver_accepted = false;
   BOOST_CHECK(!d.deliver_to_depot(/*chain_code=*/2, payload));
   BOOST_REQUIRE_EQUAL(d.deliver_calls.size(), 2u);
   BOOST_CHECK_EQUAL(d.deliver_calls[0].chain_code, 2u);
   BOOST_CHECK(d.deliver_calls[0].raw_messages == payload);
}

/// A backlog report and the post-delivery `chkcons` request are recorded with what they named.
BOOST_AUTO_TEST_CASE(backlog_reports_and_chkcons_requests_are_recorded) {
   mock_depot_ops d;
   constexpr uint64_t chain_code = 2;
   constexpr uint32_t epoch      = 9;
   constexpr uint32_t pending    = 3;
   constexpr uint64_t oldest_ms  = 1'800'000'000'000;
   BOOST_CHECK(d.report_backlog(chain_code, epoch, pending, oldest_ms));
   d.report_accepted = false;
   BOOST_CHECK(!d.report_backlog(chain_code, epoch, pending, oldest_ms));
   BOOST_REQUIRE_EQUAL(d.report_calls.size(), 2u);
   BOOST_CHECK_EQUAL(d.report_calls[0].chain_code, chain_code);
   BOOST_CHECK_EQUAL(d.report_calls[0].epoch_index, epoch);
   BOOST_CHECK_EQUAL(d.report_calls[0].pending_count, pending);
   BOOST_CHECK_EQUAL(d.report_calls[0].oldest_pending_ms, oldest_ms);

   d.has_reported_response = [](uint64_t, uint32_t epoch_index) { return epoch_index == epoch; };
   BOOST_CHECK(d.has_reported_backlog(chain_code, epoch));
   BOOST_CHECK(!d.has_reported_backlog(chain_code, epoch + 1));

   d.push_chkcons_if_due(chain_code);
   BOOST_CHECK(d.chkcons_if_due_calls == std::vector<uint64_t>{chain_code});
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
