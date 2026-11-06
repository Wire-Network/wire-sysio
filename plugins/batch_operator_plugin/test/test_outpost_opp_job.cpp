#include "mocks/mock_depot_ops.hpp"
#include "mocks/mock_outpost_client.hpp"

#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>
#include <sysio/chain/exceptions.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace std::literals;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT;
using sysio::opp::types::CHAIN_KIND_EVM;
using sysio::opp::types::CHAIN_KIND_SVM;
using sysio::outbound_envelope_record;
using sysio::outpost_opp_job;
using sysio::test::mock_depot_ops;
using sysio::test::mock_outpost_client;

namespace {

auto make_client(sysio::opp::types::ChainKind k, uint64_t id, uint32_t cid) {
   return std::make_shared<mock_outpost_client>(k, id, cid);
}

constexpr fc::microseconds kDeadline = fc::seconds(2);
constexpr auto kTestEpoch = 5u;
constexpr auto kSolanaChainCode = 1u;
constexpr auto kSolanaChainId = 0u;
constexpr auto kInitialRetryDelay = 1s;
constexpr auto kRetryEpsilon = 1ms;
constexpr auto kSlowDeliveryDuration = 10s;
constexpr auto kProcessedSignature = "processed-signature";
constexpr auto kUnknownConfirmation = "confirmation unknown";
constexpr auto kExecutionFailure = "execution failed";
constexpr auto kCompletionQueryFailure = "depot query failed";
constexpr auto kIncompleteUpload = "more chunks remain";
constexpr auto kRepeatedPolls = 100u;
constexpr std::array kExpectedRetryDelays{1s, 2s, 4s, 8s, 16s, 30s, 30s};

/// Freeze time in tests that inspect repeated polls without exercising retry timing.
std::chrono::steady_clock::time_point fixed_retry_time() {
   return {};
}

/// Drive retry eligibility without real sleeps or wall-clock assumptions.
struct outbound_retry_fixture {
   std::shared_ptr<mock_outpost_client> client = make_client(CHAIN_KIND_SVM, kSolanaChainCode, kSolanaChainId);
   mock_depot_ops depot;
   std::chrono::steady_clock::time_point now{};
   outpost_opp_job job{client, depot, kDeadline, [this] { return now; }};

   /// Keep a single envelope pending until a test supplies completion evidence.
   outbound_retry_fixture() {
      depot.epoch = kTestEpoch;
      depot.pending_response = [](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
         outbound_envelope_record record;
         record.raw_envelope = {'x'};
         return record;
      };
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(outpost_opp_job_tests)

// ─── run_outbound ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(run_outbound_skips_when_epoch_window_closed) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.window_open = false;

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_outbound();

   BOOST_CHECK(client->outbound_calls.empty());
   BOOST_CHECK(depot.read_pending_calls.empty());
   BOOST_CHECK(depot.emitted_events.empty());
}

BOOST_AUTO_TEST_CASE(run_outbound_skips_when_not_elected) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.elected = false;

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_outbound();

   BOOST_CHECK(client->outbound_calls.empty());
   BOOST_CHECK(depot.read_pending_calls.empty());
   BOOST_CHECK(depot.emitted_events.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_skips_when_not_elected) {
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.elected = false;

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_inbound();

   BOOST_CHECK(client->inbound_calls.empty());
   BOOST_CHECK(depot.has_delivered_calls.empty());
   BOOST_CHECK(depot.deliver_calls.empty());
}

BOOST_AUTO_TEST_CASE(run_outbound_noop_when_no_pending_envelope) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   // depot.pending_response returns nullopt by default.

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_outbound();

   BOOST_REQUIRE_EQUAL(depot.read_pending_calls.size(), 1u);
   BOOST_CHECK(client->outbound_calls.empty());
   BOOST_CHECK(depot.emitted_events.empty());
}

BOOST_AUTO_TEST_CASE(run_outbound_delivers_and_emits_eth_depot_direction) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.chain_code   = 0;
   rec.epoch_index  = 5;
   rec.raw_envelope = {'e', 'n', 'v'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   client->deliver_response = [](const auto&) { return std::string{"0x1234"}; };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
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
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'s', 'o', 'l'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_outbound();

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   BOOST_CHECK(std::get<1>(depot.emitted_events[0]) == DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA);
}

/// Consecutive cron polls are throttled, and changing epoch makes the next envelope eligible.
BOOST_AUTO_TEST_CASE(run_outbound_throttles_repeated_polls_and_resets_on_new_epoch) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_outbound();
   job.run_outbound(); // Second fire in the same epoch.

   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);

   // Advance epoch — a new delivery attempt is allowed.
   depot.epoch = 6;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
   BOOST_CHECK_EQUAL(client->outbound_calls[1].epoch_index, 6u);
}

// The outpost's per-epoch cranks ride the first successful delivery of an epoch:
// once per epoch, after the envelope landed, with the delivery's own deadline.
BOOST_AUTO_TEST_CASE(run_outbound_cranks_the_outpost_once_per_epoch_after_delivery) {
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_outbound();
   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_REQUIRE_EQUAL(client->crank_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client->crank_calls[0].epoch_index, 5u);
   BOOST_CHECK(client->crank_calls[0].deadline == kDeadline);

   job.run_outbound(); // Same epoch: neither a delivery nor a crank.
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);

   depot.epoch = 6;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 2u);
   BOOST_CHECK_EQUAL(client->crank_calls[1].epoch_index, 6u);
}

// The path-2 consensus re-delivery of an already-delivered epoch is not a new
// delivery: it cranks nothing a second time.
BOOST_AUTO_TEST_CASE(run_outbound_consensus_retry_does_not_crank_again) {
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_outbound();
   BOOST_REQUIRE_EQUAL(client->crank_calls.size(), 1u);

   depot.epoch_boundary_past = true;
   job.run_outbound(); // The consensus retry re-delivers the same epoch...
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u); // ...without cranking again.
}

// A failed delivery cranks nothing: the crank belongs to a landed envelope.
BOOST_AUTO_TEST_CASE(run_outbound_does_not_crank_when_delivery_fails) {
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   client->deliver_response = [](const auto&) -> std::string { FC_THROW("rpc down"); };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK(client->crank_calls.empty());
}

// A failed crank is logged and retried by the next epoch's delivery; it never
// un-marks the delivery that already landed.
BOOST_AUTO_TEST_CASE(run_outbound_crank_failure_leaves_the_delivery_marked) {
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   client->crank_response = [](const auto&) { FC_THROW("crank refused"); };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);

   job.run_outbound(); // The epoch is delivered: no re-delivery, no second crank.
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);

   depot.epoch = 6;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 2u);
}

BOOST_AUTO_TEST_CASE(run_outbound_swallows_exceptions_and_does_not_mark_epoch) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
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

   std::chrono::steady_clock::time_point now{};
   outpost_opp_job job(client, depot, kDeadline, [&] { return now; });
   BOOST_CHECK_NO_THROW(job.run_outbound());
   // Visibility fix: emit fires BEFORE delivery, so a failed attempt still
   // lands a debug event with the attempted bytes.
   BOOST_CHECK_EQUAL(depot.emitted_events.size(), 1u);

   // A failed attempt stays eligible once its nonblocking backoff elapses.
   client->deliver_response = [](const auto&) { return std::string{"recovered"}; };
   now += kInitialRetryDelay;
   job.run_outbound();
   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 2u);
   // Two emits total: one for the failed attempt, one for the recovered retry.
   // Same epoch + same envelope ⇒ same hash, so the sink dedupes on disk.
   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 2u);
}

/// Partial progress retains the short continuation delay and cranks only after success.
BOOST_AUTO_TEST_CASE(run_outbound_retries_an_incomplete_delivery_after_backoff_without_cranking) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 5;
   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   client->deliver_response = [](const auto&) -> std::string {
      SYS_THROW(sysio::chain::outpost_delivery_incomplete_exception, "still open");
   };

   std::chrono::steady_clock::time_point now{};
   outpost_opp_job job(client, depot, kDeadline, [&] { return now; });
   BOOST_CHECK_NO_THROW(job.run_outbound());
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 0u);

   // Not marked: the next tick resumes it, and the crank rides the delivery that lands.
   client->deliver_response = [](const auto&) { return std::string{"0xfinished"}; };
   now += kInitialRetryDelay;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
}

/// Failed submissions still emit diagnostic bytes and retry after their scheduled delay.
BOOST_AUTO_TEST_CASE(run_outbound_emits_event_on_failure_and_retries_after_backoff) {
   // Reproduces the dev-026 Solana groups-of-7 symptom: delivery throws a
   // json_rpc_error with -32602/empty body, the cron tick swallows it, and
   // the next beat retries against the same epoch+envelope. Visibility fix
   // demands the bytes show up in the debug stream regardless of outcome.
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 5;
   outbound_envelope_record rec;
   rec.raw_envelope = {'b', 'y', 't', 'e', 's'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   client->deliver_response = [](const auto&) -> std::string {
      throw fc::network::json_rpc::json_rpc_error(-32602, "", fc::variant{});
   };

   std::chrono::steady_clock::time_point now{};
   outpost_opp_job job(client, depot, kDeadline, [&] { return now; });
   BOOST_CHECK_NO_THROW(job.run_outbound());

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   const auto& ev = depot.emitted_events[0];
   BOOST_CHECK_EQUAL(std::get<0>(ev), 5u);
   BOOST_CHECK(std::get<1>(ev) == DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA);
   BOOST_CHECK(std::get<3>(ev) == rec.raw_envelope);
   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 1u);

   // Same epoch, success on retry — proves _last_outbound_epoch wasn't
   // advanced by the failure.
   client->deliver_response = [](const auto&) { return std::string{"sig"}; };
   now += kInitialRetryDelay;
   job.run_outbound();
   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 2u);
   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 2u);
}

BOOST_AUTO_TEST_CASE(run_outbound_emit_failure_does_not_break_delivery) {
   // A throwing slot in the debug-envelope signal must never break the
   // producer cluster — the emit is wrapped in FC_LOG_AND_DROP and the
   // delivery still proceeds.
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 5;
   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   client->deliver_response = [](const auto&) { return std::string{"0xdeadbeef"}; };
   depot.emit_thrower = [] { FC_THROW("debug sink down"); };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   BOOST_CHECK_NO_THROW(job.run_outbound());

   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK(depot.emitted_events.empty()); // throwing emit was not recorded
}

/// Provisional success never exhausts retries, and inbound remains runnable between them.
BOOST_FIXTURE_TEST_CASE(run_outbound_retries_with_capped_backoff_while_inbound_is_empty, outbound_retry_fixture) {
   job.run_outbound();
   for (const auto delay : kExpectedRetryDelays) {
      const auto calls = client->outbound_calls.size();
      now += delay - kRetryEpsilon;
      for (unsigned tick = 0; tick < kRepeatedPolls; ++tick) {
         job.run_outbound();
         job.run_inbound();
      }
      BOOST_CHECK_EQUAL(client->outbound_calls.size(), calls);
      now += kRetryEpsilon;
      job.run_outbound();
      BOOST_CHECK_EQUAL(client->outbound_calls.size(), calls + 1);
   }
   BOOST_CHECK_EQUAL(client->inbound_calls.size(), kRepeatedPolls * kExpectedRetryDelays.size());
   BOOST_CHECK(depot.deliver_calls.empty());
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);
}

/// The boundary expedites one attempt, then subsequent polls obey backoff again.
BOOST_FIXTURE_TEST_CASE(run_outbound_boundary_resets_backoff_once, outbound_retry_fixture) {
   job.run_outbound();
   for (size_t attempt = 0; attempt < 5; ++attempt) {
      now += kExpectedRetryDelays[attempt];
      job.run_outbound();
   }
   const auto before_boundary = client->outbound_calls.size();
   depot.epoch_boundary_past = true;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), before_boundary + 1);
   depot.epoch_boundary_past = false;
   job.run_outbound();
   depot.epoch_boundary_past = true;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), before_boundary + 1);
   now += kInitialRetryDelay;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), before_boundary + 2);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);
   ++depot.epoch;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), before_boundary + 3);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 2u);
}

/// Successful terminal sends before the remote boundary still need a later consensus retry.
BOOST_FIXTURE_TEST_CASE(run_outbound_recovers_when_outpost_boundary_is_later, outbound_retry_fixture) {
   bool outpost_boundary_past = false;
   bool consensus = false;
   client->deliver_response = [&](const auto&) {
      consensus = outpost_boundary_past;
      return std::string{kProcessedSignature};
   };
   job.run_outbound();
   depot.epoch_boundary_past = true;
   job.run_outbound();
   BOOST_CHECK(!consensus);
   now += kInitialRetryDelay;
   job.run_outbound();
   BOOST_CHECK(!consensus);
   outpost_boundary_past = true;
   now += kExpectedRetryDelays[1];
   job.run_outbound();
   BOOST_CHECK(consensus);
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 4u);
}

/// A local completion row stops sends, but its disappearance must restore recovery.
BOOST_FIXTURE_TEST_CASE(run_outbound_rechecks_depot_completion_without_latching_it, outbound_retry_fixture) {
   bool completed = true;
   depot.has_delivered_response = [&](uint64_t, uint32_t) { return completed; };
   job.run_outbound();
   BOOST_CHECK(client->outbound_calls.empty());
   completed = false;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   now += kInitialRetryDelay;
   completed = true;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   completed = false;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
}

/// A confirmed settled shortcut is still provisional and cannot consume a final retry.
BOOST_FIXTURE_TEST_CASE(run_outbound_empty_success_keeps_recovery_eligible, outbound_retry_fixture) {
   client->deliver_response = [](const auto&) { return std::string{}; };
   job.run_outbound();
   depot.epoch_boundary_past = true;
   job.run_outbound();
   now += kInitialRetryDelay;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 3u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);
}

/// Rate limits are measured after RPC completion, including a slow successful send.
BOOST_FIXTURE_TEST_CASE(run_outbound_backoff_starts_after_delivery_completes, outbound_retry_fixture) {
   client->deliver_response = [&](const auto&) {
      now += kSlowDeliveryDuration;
      return std::string{kProcessedSignature};
   };
   job.run_outbound();
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   now += kInitialRetryDelay;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
}

/// Execution errors and unknown confirmations both leave the epoch recoverable and rate limited.
BOOST_FIXTURE_TEST_CASE(run_outbound_backs_off_errors_and_unknown_confirmations, outbound_retry_fixture) {
   for (const bool unknown_confirmation : {false, true}) {
      ++depot.epoch;
      client->deliver_response = [unknown_confirmation](const auto&) -> std::string {
         if (unknown_confirmation)
            FC_THROW_EXCEPTION(fc::timeout_exception, kUnknownConfirmation);
         FC_THROW(kExecutionFailure);
      };
      const auto calls = client->outbound_calls.size();
      job.run_outbound();
      job.run_outbound();
      BOOST_CHECK_EQUAL(client->outbound_calls.size(), calls + 1);
      now += kInitialRetryDelay;
      client->deliver_response = [](const auto&) { return std::string{kProcessedSignature}; };
      job.run_outbound();
      BOOST_CHECK_EQUAL(client->outbound_calls.size(), calls + 2);
   }
}

/// A failed completion query is unknown state, not a permanently handled epoch.
BOOST_FIXTURE_TEST_CASE(run_outbound_recovers_from_completion_query_failure, outbound_retry_fixture) {
   depot.has_delivered_response = [](uint64_t, uint32_t) -> bool { FC_THROW(kCompletionQueryFailure); };
   BOOST_CHECK_NO_THROW(job.run_outbound());
   BOOST_CHECK(client->outbound_calls.empty());
   depot.has_delivered_response = {};
   job.run_outbound();
   BOOST_CHECK(client->outbound_calls.empty());
   now += kInitialRetryDelay;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
}

/// Incomplete continuations retain a short delay without requiring proof of progress.
BOOST_FIXTURE_TEST_CASE(run_outbound_partial_progress_keeps_short_backoff, outbound_retry_fixture) {
   client->deliver_response = [](const auto&) -> std::string {
      SYS_THROW(sysio::chain::outpost_delivery_incomplete_exception, kIncompleteUpload);
   };
   for (unsigned attempt = 0; attempt < 6; ++attempt) {
      job.run_outbound();
      BOOST_CHECK_EQUAL(client->outbound_calls.size(), attempt + 1);
      now += kInitialRetryDelay;
   }
   BOOST_CHECK(client->crank_calls.empty());
}

/// Missing work spends no retry delay, and epoch zero never sends.
BOOST_FIXTURE_TEST_CASE(run_outbound_missing_envelope_and_genesis_do_not_spend_backoff, outbound_retry_fixture) {
   const auto pending = depot.pending_response;
   depot.pending_response = {};
   job.run_outbound();
   depot.pending_response = pending;
   depot.epoch = 0;
   job.run_outbound();
   BOOST_CHECK(client->outbound_calls.empty());
   depot.epoch = kTestEpoch;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
}

/// The production jobs recover two-of-three consensus after both final allowed sends roll back.
/// Mock sends return processed success on a discarded fork; inbound stays empty until
/// canonical settlement is explicitly finalized. No restart or third operator is supplied.
BOOST_AUTO_TEST_CASE(run_outbound_recovers_rolled_back_success_with_two_online_operators) {
   constexpr size_t seated_operators = 3;
   constexpr size_t online_operators = 2;
   const std::vector<char> outbound{'e', 'n', 'v'};
   const std::vector<char> inbound{'f', 'i', 'n'};
   for (const bool rollback_initial : {false, true}) {
      std::chrono::steady_clock::time_point now{};
      std::array<mock_depot_ops, online_operators> depots;
      std::array<std::shared_ptr<mock_outpost_client>, online_operators> clients;
      std::array<std::unique_ptr<outpost_opp_job>, online_operators> jobs;
      std::array<bool, online_operators> canonical_deliveries{};
      std::array<bool, online_operators> fork_deliveries{};
      std::array<bool, online_operators> depot_deliveries{};
      bool abandoned_fork = rollback_initial;
      bool canonical_consensus = false;
      bool fork_consensus = false;
      bool finalized = false;
      size_t effects = 0;
      size_t fork_effects = 0;
      for (size_t op = 0; op < online_operators; ++op) {
         depots[op].epoch = kTestEpoch;
         depots[op].pending_response = [&](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
            outbound_envelope_record record;
            record.raw_envelope = outbound;
            return record;
         };
         depots[op].has_delivered_response = [&, op](uint64_t, uint32_t) { return depot_deliveries[op]; };
         depots[op].deliver_thrower = [&, op] {
            depot_deliveries[op] = true;
            if (std::count(depot_deliveries.begin(), depot_deliveries.end(), true) > seated_operators / 2) {
               for (auto& depot : depots)
                  depot.epoch = kTestEpoch + 1;
            }
         };
         clients[op] = make_client(CHAIN_KIND_SVM, kSolanaChainCode, kSolanaChainId);
         clients[op]->deliver_response = [&, op](const auto& call) {
            BOOST_CHECK(call.envelope_bytes == outbound);
            auto& deliveries = abandoned_fork ? fork_deliveries : canonical_deliveries;
            auto& consensus = abandoned_fork ? fork_consensus : canonical_consensus;
            auto& applied_effects = abandoned_fork ? fork_effects : effects;
            deliveries[op] = true;
            if (!consensus && depots[op].epoch_boundary_past &&
                std::count(deliveries.begin(), deliveries.end(), true) > seated_operators / 2) {
               consensus = true;
               ++applied_effects;
            }
            return std::string{kProcessedSignature};
         };
         clients[op]->inbound_response = [&](const auto&) {
            return canonical_consensus && finalized ? inbound : std::vector<char>{};
         };
         jobs[op] = std::make_unique<outpost_opp_job>(clients[op], depots[op], kDeadline, [&] { return now; });
         jobs[op]->run_outbound();
      }
      fork_deliveries = canonical_deliveries;
      abandoned_fork = true;
      for (size_t op = 0; op < online_operators; ++op) {
         depots[op].epoch_boundary_past = true;
         jobs[op]->run_outbound();
         BOOST_REQUIRE_EQUAL(clients[op]->outbound_calls.size(), 2u);
      }
      BOOST_REQUIRE(fork_consensus);
      BOOST_REQUIRE_EQUAL(fork_effects, 1u);
      // Both terminal calls succeeded and tipped consensus, but only on the
      // abandoned fork. Remove that provisional state before normal polling.
      fork_deliveries = {};
      fork_consensus = false;
      fork_effects = 0;
      abandoned_fork = false;
      BOOST_CHECK(!canonical_consensus);
      for (unsigned poll = 0; poll < kRepeatedPolls; ++poll) {
         for (auto& job : jobs) {
            job->run_outbound();
            job->run_inbound();
         }
      }
      for (size_t op = 0; op < online_operators; ++op) {
         BOOST_CHECK_EQUAL(clients[op]->outbound_calls.size(), 2u);
         BOOST_CHECK(depots[op].deliver_calls.empty());
      }
      now += kInitialRetryDelay;
      for (auto& job : jobs) {
         job->run_outbound();
         job->run_inbound();
      }
      BOOST_REQUIRE(canonical_consensus);
      for (const auto& depot : depots)
         BOOST_CHECK(depot.deliver_calls.empty());
      finalized = true;
      for (auto& job : jobs)
         job->run_inbound();
      for (size_t op = 0; op < online_operators; ++op) {
         BOOST_CHECK_EQUAL(depots[op].epoch, kTestEpoch + 1);
         BOOST_REQUIRE_EQUAL(depots[op].deliver_calls.size(), 1u);
         BOOST_CHECK(depots[op].deliver_calls.front().raw_messages == inbound);
         BOOST_CHECK_EQUAL(clients[op]->outbound_calls.size(), 3u);
         BOOST_CHECK_EQUAL(clients[op]->crank_calls.size(), 1u);
      }
      BOOST_CHECK_EQUAL(effects, 1u);
   }
}

// ─── run_inbound ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(run_inbound_skips_when_epoch_window_closed) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.window_open = false;

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_inbound();

   BOOST_CHECK(client->inbound_calls.empty());
   BOOST_CHECK(depot.deliver_calls.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_skips_when_already_delivered) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.has_delivered_response = [](uint64_t, uint32_t) { return true; };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_inbound();

   BOOST_REQUIRE_EQUAL(depot.has_delivered_calls.size(), 1u);
   BOOST_CHECK(client->inbound_calls.empty());
   BOOST_CHECK(depot.deliver_calls.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_noop_when_remote_has_nothing) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_inbound();

   BOOST_REQUIRE_EQUAL(client->inbound_calls.size(), 1u);
   BOOST_CHECK(depot.deliver_calls.empty());
   BOOST_CHECK(depot.emitted_events.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_delivers_to_depot_and_emits_eth_depot_signal) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 7;
   std::vector<char> raw{'i', 'n', 'b'};
   client->inbound_response = [raw](const auto&) { return raw; };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_inbound();

   BOOST_REQUIRE_EQUAL(depot.deliver_calls.size(), 1u);
   BOOST_CHECK_EQUAL(depot.deliver_calls[0].chain_code, 0u);
   BOOST_CHECK(depot.deliver_calls[0].raw_messages == raw);

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   auto& ev = depot.emitted_events[0];
   BOOST_CHECK_EQUAL(std::get<0>(ev), 7u);
   BOOST_CHECK(std::get<1>(ev) == DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT);
   BOOST_CHECK(std::get<3>(ev) == raw);
}

BOOST_AUTO_TEST_CASE(run_inbound_emits_sol_depot_signal) {
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   std::vector<char> raw{'s'};
   client->inbound_response = [raw](const auto&) { return raw; };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   job.run_inbound();

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   BOOST_CHECK(std::get<1>(depot.emitted_events[0]) == DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT);
}

BOOST_AUTO_TEST_CASE(run_inbound_emits_before_deliver_to_depot) {
   // When the WIRE-side push_action throws (transient mempool / serializer
   // hiccup), the bytes pulled from the outpost must already be captured
   // so the debugging trail isn't lost.
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 7;
   std::vector<char> raw{'i', 'n'};
   client->inbound_response = [raw](const auto&) { return raw; };
   depot.deliver_thrower = [] { FC_THROW("transient mempool"); };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   BOOST_CHECK_NO_THROW(job.run_inbound());

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   const auto& ev = depot.emitted_events[0];
   BOOST_CHECK_EQUAL(std::get<0>(ev), 7u);
   BOOST_CHECK(std::get<1>(ev) == DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT);
   BOOST_CHECK(std::get<3>(ev) == raw);
   // Throwing deliver was not recorded by the mock.
   BOOST_CHECK(depot.deliver_calls.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_emit_failure_does_not_break_delivery) {
   // A throwing emit in the inbound path must not prevent the WIRE-side
   // push_action — same FC_LOG_AND_DROP guarantee as outbound.
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.epoch = 7;
   std::vector<char> raw{'i'};
   client->inbound_response = [raw](const auto&) { return raw; };
   depot.emit_thrower = [] { FC_THROW("debug sink down"); };

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);
   BOOST_CHECK_NO_THROW(job.run_inbound());

   BOOST_REQUIRE_EQUAL(depot.deliver_calls.size(), 1u);
   BOOST_CHECK(depot.deliver_calls[0].raw_messages == raw);
   BOOST_CHECK(depot.emitted_events.empty()); // throwing emit was not recorded
}

// ─── concurrency ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(run_outbound_and_run_inbound_serialize_on_state_mx) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
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

   outpost_opp_job job(client, depot, kDeadline, fixed_retry_time);

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
