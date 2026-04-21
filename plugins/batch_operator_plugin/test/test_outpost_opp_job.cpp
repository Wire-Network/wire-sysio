#include <atomic>
#include <chrono>
#include <thread>

#include <boost/test/unit_test.hpp>

#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>

#include "mocks/mock_depot_ops.hpp"
#include "mocks/mock_outpost_client.hpp"

using namespace std::literals;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT;
using sysio::opp::types::CHAIN_KIND_ETHEREUM;
using sysio::opp::types::CHAIN_KIND_SOLANA;
using sysio::outbound_envelope_record;
using sysio::outpost_opp_job;
using sysio::test::mock_depot_ops;
using sysio::test::mock_outpost_client;

namespace {

auto make_client(sysio::opp::types::ChainKind k, uint64_t id, uint32_t cid) {
   return std::make_shared<mock_outpost_client>(k, id, cid);
}

constexpr fc::microseconds kDeadline = fc::seconds(2);

} // namespace

BOOST_AUTO_TEST_SUITE(outpost_opp_job_tests)

// ─── run_outbound ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(run_outbound_skips_when_epoch_window_closed) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;
   depot.window_open = false;

   outpost_opp_job job(client, depot, kDeadline);
   job.run_outbound();

   BOOST_CHECK(client->outbound_calls.empty());
   BOOST_CHECK(depot.read_pending_calls.empty());
   BOOST_CHECK(depot.emitted_events.empty());
}

BOOST_AUTO_TEST_CASE(run_outbound_skips_when_not_elected) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;
   depot.elected = false;

   outpost_opp_job job(client, depot, kDeadline);
   job.run_outbound();

   BOOST_CHECK(client->outbound_calls.empty());
   BOOST_CHECK(depot.read_pending_calls.empty());
   BOOST_CHECK(depot.emitted_events.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_skips_when_not_elected) {
   auto client = make_client(CHAIN_KIND_SOLANA, 1, 0);
   mock_depot_ops depot;
   depot.elected = false;

   outpost_opp_job job(client, depot, kDeadline);
   job.run_inbound();

   BOOST_CHECK(client->inbound_calls.empty());
   BOOST_CHECK(depot.has_delivered_calls.empty());
   BOOST_CHECK(depot.deliver_calls.empty());
}

BOOST_AUTO_TEST_CASE(run_outbound_noop_when_no_pending_envelope) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;
   // depot.pending_response returns nullopt by default.

   outpost_opp_job job(client, depot, kDeadline);
   job.run_outbound();

   BOOST_REQUIRE_EQUAL(depot.read_pending_calls.size(), 1u);
   BOOST_CHECK(client->outbound_calls.empty());
   BOOST_CHECK(depot.emitted_events.empty());
}

BOOST_AUTO_TEST_CASE(run_outbound_delivers_and_emits_eth_depot_direction) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.outpost_id   = 0;
   rec.epoch_index  = 5;
   rec.raw_envelope = {'e', 'n', 'v'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   client->deliver_response = [](const auto&) { return std::string{"0x1234"}; };

   outpost_opp_job job(client, depot, kDeadline);
   job.run_outbound();

   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client->outbound_calls[0].epoch_index, 5u);
   BOOST_CHECK(client->outbound_calls[0].envelope_bytes == rec.raw_envelope);

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   auto& ev = depot.emitted_events[0];
   BOOST_CHECK_EQUAL(std::get<0>(ev), 5u);
   BOOST_CHECK(std::get<1>(ev) == DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM);
   BOOST_CHECK(std::get<3>(ev) == rec.raw_envelope);
}

BOOST_AUTO_TEST_CASE(run_outbound_emits_sol_depot_direction) {
   auto client = make_client(CHAIN_KIND_SOLANA, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'s', 'o', 'l'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };

   outpost_opp_job job(client, depot, kDeadline);
   job.run_outbound();

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   BOOST_CHECK(std::get<1>(depot.emitted_events[0]) == DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA);
}

BOOST_AUTO_TEST_CASE(run_outbound_only_delivers_once_per_epoch) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };

   outpost_opp_job job(client, depot, kDeadline);
   job.run_outbound();
   job.run_outbound(); // Second fire in the same epoch.

   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);

   // Advance epoch — a new delivery attempt is allowed.
   depot.epoch = 6;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
   BOOST_CHECK_EQUAL(client->outbound_calls[1].epoch_index, 6u);
}

BOOST_AUTO_TEST_CASE(run_outbound_swallows_exceptions_and_does_not_mark_epoch) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 5;
   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   client->deliver_response = [](const auto&) -> std::string {
      FC_THROW("boom");
   };

   outpost_opp_job job(client, depot, kDeadline);
   BOOST_CHECK_NO_THROW(job.run_outbound());
   BOOST_CHECK(depot.emitted_events.empty()); // no signal on failure

   // The failed attempt should NOT bump _last_outbound_epoch; next tick retries.
   client->deliver_response = [](const auto&) { return std::string{"recovered"}; };
   job.run_outbound();
   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 2u);
   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
}

// ─── run_inbound ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(run_inbound_skips_when_epoch_window_closed) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;
   depot.window_open = false;

   outpost_opp_job job(client, depot, kDeadline);
   job.run_inbound();

   BOOST_CHECK(client->inbound_calls.empty());
   BOOST_CHECK(depot.deliver_calls.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_skips_when_already_delivered) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;
   depot.has_delivered_response = [](uint64_t, uint32_t) { return true; };

   outpost_opp_job job(client, depot, kDeadline);
   job.run_inbound();

   BOOST_REQUIRE_EQUAL(depot.has_delivered_calls.size(), 1u);
   BOOST_CHECK(client->inbound_calls.empty());
   BOOST_CHECK(depot.deliver_calls.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_noop_when_remote_has_nothing) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;

   outpost_opp_job job(client, depot, kDeadline);
   job.run_inbound();

   BOOST_REQUIRE_EQUAL(client->inbound_calls.size(), 1u);
   BOOST_CHECK(depot.deliver_calls.empty());
   BOOST_CHECK(depot.emitted_events.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_delivers_to_depot_and_emits_eth_depot_signal) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 7;
   std::vector<char> raw{'i', 'n', 'b'};
   client->inbound_response = [raw](const auto&) { return raw; };

   outpost_opp_job job(client, depot, kDeadline);
   job.run_inbound();

   BOOST_REQUIRE_EQUAL(depot.deliver_calls.size(), 1u);
   BOOST_CHECK_EQUAL(depot.deliver_calls[0].outpost_id, 0u);
   BOOST_CHECK(depot.deliver_calls[0].raw_messages == raw);

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   auto& ev = depot.emitted_events[0];
   BOOST_CHECK_EQUAL(std::get<0>(ev), 7u);
   BOOST_CHECK(std::get<1>(ev) == DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT);
   BOOST_CHECK(std::get<3>(ev) == raw);
}

BOOST_AUTO_TEST_CASE(run_inbound_emits_sol_depot_signal) {
   auto client = make_client(CHAIN_KIND_SOLANA, 1, 0);
   mock_depot_ops depot;
   std::vector<char> raw{'s'};
   client->inbound_response = [raw](const auto&) { return raw; };

   outpost_opp_job job(client, depot, kDeadline);
   job.run_inbound();

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   BOOST_CHECK(std::get<1>(depot.emitted_events[0]) == DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT);
}

// ─── concurrency ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(run_outbound_and_run_inbound_serialize_on_state_mx) {
   auto client = make_client(CHAIN_KIND_ETHEREUM, 0, 31337);
   mock_depot_ops depot;

   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   std::vector<char> inbound_bytes{'i'};
   client->inbound_response = [inbound_bytes](const auto&) { return inbound_bytes; };

   // Simulate a slow outbound delivery so the inbound thread must wait for
   // the mutex — if serialization worked, inbound's deliver_to_depot fires
   // AFTER outbound's debug-event emission.
   std::atomic<bool> outbound_started{false};
   client->deliver_response = [&](const auto&) {
      outbound_started = true;
      std::this_thread::sleep_for(100ms);
      return std::string{"tx"};
   };

   outpost_opp_job job(client, depot, kDeadline);

   std::thread outbound([&] { job.run_outbound(); });
   while (!outbound_started) std::this_thread::sleep_for(1ms);
   std::thread inbound([&]  { job.run_inbound();  });

   outbound.join();
   inbound.join();

   // Exactly one emitted debug event for outbound and one for inbound.
   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 2u);
   // Outbound must have emitted first (serialization test).
   BOOST_CHECK(std::get<1>(depot.emitted_events[0]) == DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM);
   BOOST_CHECK(std::get<1>(depot.emitted_events[1]) == DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT);
}

BOOST_AUTO_TEST_SUITE_END()
