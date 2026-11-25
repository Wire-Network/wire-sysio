#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include <boost/test/unit_test.hpp>

#include <fc/network/json_rpc/json_rpc_client.hpp>

#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>
#include <sysio/chain/exceptions.hpp>

#include "mocks/mock_depot_ops.hpp"
#include "mocks/mock_outpost_client.hpp"

using namespace std::literals;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT;
using sysio::opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT;
using sysio::opp::types::CHAIN_KIND_EVM;
using sysio::opp::types::CHAIN_KIND_SVM;
using sysio::outbound_backlog;
using sysio::outbound_envelope_record;
using sysio::outpost_opp_job;
using sysio::test::mock_depot_ops;
using sysio::test::mock_outpost_client;

namespace {

auto make_client(sysio::opp::types::ChainKind k, uint64_t id, uint32_t cid) {
   return std::make_shared<mock_outpost_client>(k, id, cid);
}

/// The delivery timeout the mock depot gives a chain without its own knobs.
constexpr fc::microseconds kDeadline = mock_depot_ops::default_delivery_timeout;
constexpr auto kTestEpoch = 5u;
constexpr auto kSolanaChainCode = 1u;
constexpr auto kSolanaChainId = 0u;
constexpr auto kProcessedSignature = "processed-signature";
constexpr auto kUnknownConfirmation = "confirmation unknown";
constexpr auto kExecutionFailure = "execution failed";
constexpr auto kCompletionQueryFailure = "depot query failed";
constexpr auto kRepeatedPolls = 100u;

/// Exercise ordinary cron ticks with one envelope pending until depot completion.
struct outbound_retry_fixture {
   std::shared_ptr<mock_outpost_client> client = make_client(CHAIN_KIND_SVM, kSolanaChainCode, kSolanaChainId);
   mock_depot_ops depot;
   outpost_opp_job job{client, depot};

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

BOOST_AUTO_TEST_CASE(run_outbound_skips_when_not_elected) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.elected = false;

   outpost_opp_job job(client, depot);
   job.run_outbound();

   BOOST_CHECK(client->outbound_calls.empty());
   BOOST_CHECK(depot.read_pending_calls.empty());
   BOOST_CHECK(depot.emitted_events.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_skips_when_not_elected) {
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.elected = false;

   outpost_opp_job job(client, depot);
   job.run_inbound();

   BOOST_CHECK(client->inbound_calls.empty());
   BOOST_CHECK(depot.has_delivered_calls.empty());
   BOOST_CHECK(depot.deliver_calls.empty());
}

BOOST_AUTO_TEST_CASE(run_outbound_noop_when_no_pending_envelope) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   // depot.pending_response returns nullopt by default.

   outpost_opp_job job(client, depot);
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

   outpost_opp_job job(client, depot);
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

   outpost_opp_job job(client, depot);
   job.run_outbound();

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   BOOST_CHECK(std::get<1>(depot.emitted_events[0]) == DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA);
}

/// Nothing holds a delivery back within an epoch: every tick calls the client, from the epoch's first tick on, until
/// the depot records this operator's inbound delivery; a new epoch is delivered at once.
BOOST_FIXTURE_TEST_CASE(run_outbound_calls_the_client_every_tick_until_the_depot_records_the_reply,
                        outbound_retry_fixture) {
   for (unsigned tick = 0; tick < kRepeatedPolls; ++tick) job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), kRepeatedPolls);
   BOOST_CHECK(std::ranges::all_of(client->outbound_calls,
                                   [](const auto& call) { return call.epoch_index == kTestEpoch; }));

   depot.has_delivered_response = [](uint64_t, uint32_t epoch) { return epoch == kTestEpoch; };
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), kRepeatedPolls);

   depot.epoch = kTestEpoch + 1;
   job.run_outbound();
   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), kRepeatedPolls + 1);
   BOOST_CHECK_EQUAL(client->outbound_calls.back().epoch_index, kTestEpoch + 1);
   // One crank per epoch.
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 2u);
}

// The outpost's per-epoch cranks ride the first outbound call of an epoch that
// returns: once per epoch, with the delivery's own deadline.
BOOST_AUTO_TEST_CASE(run_outbound_cranks_the_outpost_once_per_epoch_after_delivery) {
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };

   outpost_opp_job job(client, depot);
   job.run_outbound();
   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_REQUIRE_EQUAL(client->crank_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client->crank_calls[0].epoch_index, 5u);
   BOOST_CHECK(client->crank_calls[0].deadline == kDeadline);

   // Same epoch: the client is called again, awaiting its peers, but the crank is not.
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);

   depot.epoch = 6;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 2u);
   BOOST_CHECK_EQUAL(client->crank_calls[1].epoch_index, 6u);
}

// A failed delivery cranks nothing: the crank belongs to a call that returned.
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

   outpost_opp_job job(client, depot);
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK(client->crank_calls.empty());
}

// A failed crank is logged and retried by the next epoch's delivery; repeat calls in the same epoch never crank again.
BOOST_AUTO_TEST_CASE(run_outbound_crank_failure_is_not_retried_in_the_same_epoch) {
   auto client = make_client(CHAIN_KIND_SVM, 1, 0);
   mock_depot_ops depot;
   depot.epoch = 5;

   outbound_envelope_record rec;
   rec.raw_envelope = {'x'};
   depot.pending_response = [rec](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      return rec;
   };
   client->crank_response = [](const auto&) { FC_THROW("crank refused"); };

   outpost_opp_job job(client, depot);
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);

   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);

   depot.epoch = 6;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 2u);
}

BOOST_AUTO_TEST_CASE(run_outbound_swallows_exceptions_and_retries_next_tick) {
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

   outpost_opp_job job(client, depot);
   BOOST_CHECK_NO_THROW(job.run_outbound());
   // Visibility fix: emit fires BEFORE delivery, so a failed attempt still
   // lands a debug event with the attempted bytes.
   BOOST_CHECK_EQUAL(depot.emitted_events.size(), 1u);
   BOOST_CHECK(client->crank_calls.empty());

   client->deliver_response = [](const auto&) { return std::string{"recovered"}; };
   job.run_outbound();
   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 2u);
   // Two emits total: one for the failed attempt, one for the recovered retry.
   // Same epoch + same envelope ⇒ same hash, so the sink dedupes on disk.
   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 2u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);
}

/// An incomplete delivery is neither a failure nor a handled epoch: the job
/// does not crank, and the next tick delivers again; the crank rides the first
/// call that returns.
BOOST_AUTO_TEST_CASE(run_outbound_retries_an_incomplete_delivery_next_tick_without_cranking) {
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

   outpost_opp_job job(client, depot);
   BOOST_CHECK_NO_THROW(job.run_outbound());
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 0u);

   client->deliver_response = [](const auto&) { return std::string{"0xfinished"}; };
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);
}

BOOST_AUTO_TEST_CASE(run_outbound_emits_event_on_failure_and_retries_next_tick) {
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

   outpost_opp_job job(client, depot);
   BOOST_CHECK_NO_THROW(job.run_outbound());

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   const auto& ev = depot.emitted_events[0];
   BOOST_CHECK_EQUAL(std::get<0>(ev), 5u);
   BOOST_CHECK(std::get<1>(ev) == DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA);
   BOOST_CHECK(std::get<3>(ev) == rec.raw_envelope);
   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 1u);

   client->deliver_response = [](const auto&) { return std::string{"sig"}; };
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

   outpost_opp_job job(client, depot);
   BOOST_CHECK_NO_THROW(job.run_outbound());

   BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 1u);
   BOOST_CHECK(depot.emitted_events.empty()); // throwing emit was not recorded
}

/// A local completion row stops calls, and its disappearance restores them.
BOOST_FIXTURE_TEST_CASE(run_outbound_rechecks_depot_completion_without_latching_it, outbound_retry_fixture) {
   bool completed = true;
   depot.has_delivered_response = [&](uint64_t, uint32_t) { return completed; };
   job.run_outbound();
   BOOST_CHECK(client->outbound_calls.empty());
   completed = false;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   completed = true;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   completed = false;
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 2u);
}

/// Nothing to send is provisional: the next tick calls again, and only the first call cranks.
BOOST_FIXTURE_TEST_CASE(run_outbound_empty_success_keeps_calling, outbound_retry_fixture) {
   client->deliver_response = [](const auto&) { return std::string{}; };
   job.run_outbound();
   job.run_outbound();
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 3u);
   BOOST_CHECK_EQUAL(client->crank_calls.size(), 1u);
}

/// Execution errors and unknown confirmations leave the first delivery eligible on the next tick.
BOOST_FIXTURE_TEST_CASE(run_outbound_retries_errors_and_unknown_confirmations, outbound_retry_fixture) {
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
      BOOST_CHECK_EQUAL(client->outbound_calls.size(), calls + 2);
      client->deliver_response = [](const auto&) { return std::string{kProcessedSignature}; };
      job.run_outbound();
      BOOST_CHECK_EQUAL(client->outbound_calls.size(), calls + 3);
   }
}

/// A failed completion query is unknown state; the next tick must recover.
BOOST_FIXTURE_TEST_CASE(run_outbound_recovers_from_completion_query_failure, outbound_retry_fixture) {
   depot.has_delivered_response = [](uint64_t, uint32_t) -> bool { FC_THROW(kCompletionQueryFailure); };
   BOOST_CHECK_NO_THROW(job.run_outbound());
   BOOST_CHECK(client->outbound_calls.empty());
   depot.has_delivered_response = {};
   job.run_outbound();
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
}

/// Missing work stays eligible, and epoch zero never sends.
BOOST_FIXTURE_TEST_CASE(run_outbound_missing_envelope_and_genesis_stay_eligible, outbound_retry_fixture) {
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

/// Two of three seated operators online recover a majority after their deliveries land on a fork that is abandoned:
/// the next tick delivers again on the canonical chain, which tips at once on the majority, and the inbound reply,
/// read only once finalized, completes the epoch for both. No restart or third operator is supplied.
BOOST_AUTO_TEST_CASE(run_outbound_recovers_rolled_back_deliveries_with_two_online_operators) {
   constexpr size_t seated_operators = 3;
   constexpr size_t online_operators = 2;
   const std::vector<char> outbound{'e', 'n', 'v'};
   const std::vector<char> inbound{'f', 'i', 'n'};
   for (const bool rollback_initial : {false, true}) {
      BOOST_TEST_CONTEXT("rollback_initial " << rollback_initial) {
         std::array<mock_depot_ops, online_operators> depots;
         std::array<std::shared_ptr<mock_outpost_client>, online_operators> clients;
         std::array<std::unique_ptr<outpost_opp_job>, online_operators> jobs;
         std::array<bool, online_operators> canonical_deliveries{};
         std::array<bool, online_operators> fork_deliveries{};
         std::array<bool, online_operators> depot_deliveries{};
         bool   abandoned_fork      = rollback_initial;
         bool   canonical_consensus = false;
         bool   fork_consensus      = false;
         bool   finalized           = false;
         size_t effects             = 0;
         for (size_t op = 0; op < online_operators; ++op) {
            depots[op].epoch = kTestEpoch;
            depots[op].pending_response = [&](uint64_t, uint32_t epoch) -> std::optional<outbound_envelope_record> {
               if (epoch != kTestEpoch) return std::nullopt;
               outbound_envelope_record record;
               record.raw_envelope = outbound;
               return record;
            };
            depots[op].has_delivered_response = [&, op](uint64_t, uint32_t) { return depot_deliveries[op]; };
            depots[op].deliver_thrower = [&, op] {
               depot_deliveries[op] = true;
               if (std::count(depot_deliveries.begin(), depot_deliveries.end(), true) > seated_operators / 2) {
                  for (auto& depot : depots) depot.epoch = kTestEpoch + 1;
               }
            };
            clients[op] = make_client(CHAIN_KIND_SVM, kSolanaChainCode, kSolanaChainId);
            clients[op]->deliver_response = [&, op](const auto& call) {
               BOOST_CHECK(call.envelope_bytes == outbound);
               auto& deliveries = abandoned_fork ? fork_deliveries : canonical_deliveries;
               auto& consensus  = abandoned_fork ? fork_consensus : canonical_consensus;
               deliveries[op]   = true;
               // The outpost tips on the delivery that completes a strict majority.
               if (!consensus && std::count(deliveries.begin(), deliveries.end(), true) > seated_operators / 2) {
                  consensus = true;
                  if (!abandoned_fork) ++effects;
               }
               return std::string{kProcessedSignature};
            };
            clients[op]->inbound_response = [&](const auto& call) {
               return call.epoch_index == kTestEpoch && canonical_consensus && finalized ? inbound
                                                                                        : std::vector<char>{};
            };
            jobs[op] = std::make_unique<outpost_opp_job>(clients[op], depots[op]);
         }

         for (auto& job : jobs) job->run_outbound();
         BOOST_CHECK_EQUAL(canonical_consensus, !rollback_initial);
         BOOST_CHECK_EQUAL(fork_consensus, rollback_initial);
         // The fork is abandoned, and with it whatever tipped there.
         fork_deliveries = {};
         fork_consensus  = false;
         abandoned_fork  = false;

         for (unsigned poll = 0; poll < kRepeatedPolls; ++poll) {
            for (auto& job : jobs) job->run_inbound();
         }
         for (const auto& depot : depots) BOOST_CHECK(depot.deliver_calls.empty());

         // The next tick delivers again; the canonical chain tips on it.
         for (auto& job : jobs) job->run_outbound();
         BOOST_REQUIRE(canonical_consensus);
         BOOST_CHECK_EQUAL(effects, 1u);

         finalized = true;
         for (auto& job : jobs) job->run_inbound();
         for (size_t op = 0; op < online_operators; ++op) {
            BOOST_CHECK_EQUAL(depots[op].epoch, kTestEpoch + 1);
            BOOST_REQUIRE_EQUAL(depots[op].deliver_calls.size(), 1u);
            BOOST_CHECK(depots[op].deliver_calls.front().raw_messages == inbound);
            BOOST_CHECK_EQUAL(clients[op]->outbound_calls.size(), 2u);
            BOOST_CHECK_EQUAL(clients[op]->crank_calls.size(), 1u);
         }
         // The depot recorded both replies: nothing more is sent.
         for (auto& job : jobs) job->run_outbound();
         for (size_t op = 0; op < online_operators; ++op) BOOST_CHECK_EQUAL(clients[op]->outbound_calls.size(), 2u);
         BOOST_CHECK_EQUAL(effects, 1u);
      }
   }
}

// ─── run_inbound ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(run_inbound_skips_when_already_delivered) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;
   depot.has_delivered_response = [](uint64_t, uint32_t) { return true; };

   outpost_opp_job job(client, depot);
   job.run_inbound();

   BOOST_REQUIRE_EQUAL(depot.has_delivered_calls.size(), 1u);
   BOOST_CHECK(client->inbound_calls.empty());
   BOOST_CHECK(depot.deliver_calls.empty());
}

BOOST_AUTO_TEST_CASE(run_inbound_noop_when_remote_has_nothing) {
   auto client = make_client(CHAIN_KIND_EVM, 0, 31337);
   mock_depot_ops depot;

   outpost_opp_job job(client, depot);
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

   outpost_opp_job job(client, depot);
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

   outpost_opp_job job(client, depot);
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

   outpost_opp_job job(client, depot);
   BOOST_CHECK_NO_THROW(job.run_inbound());

   BOOST_REQUIRE_EQUAL(depot.emitted_events.size(), 1u);
   const auto& ev = depot.emitted_events[0];
   BOOST_CHECK_EQUAL(std::get<0>(ev), 7u);
   BOOST_CHECK(std::get<1>(ev) == DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT);
   BOOST_CHECK(std::get<3>(ev) == raw);
   // Throwing deliver was not recorded by the mock.
   BOOST_CHECK(depot.deliver_calls.empty());
   BOOST_CHECK(depot.chkcons_if_due_calls.empty());
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

   outpost_opp_job job(client, depot);
   BOOST_CHECK_NO_THROW(job.run_inbound());

   BOOST_REQUIRE_EQUAL(depot.deliver_calls.size(), 1u);
   BOOST_CHECK(depot.deliver_calls[0].raw_messages == raw);
   BOOST_CHECK(depot.emitted_events.empty()); // throwing emit was not recorded
}

/// Right after its own current-epoch delivery lands, the job asks the depot to push `chkcons` if the chain is due;
/// a delivery the depot refused asks nothing.
BOOST_AUTO_TEST_CASE(run_inbound_asks_for_chkcons_after_its_own_accepted_delivery) {
   constexpr uint64_t chain_code = 4;
   auto client = make_client(CHAIN_KIND_EVM, chain_code, 31337);
   mock_depot_ops depot;
   depot.epoch = 7;
   client->inbound_response = [](const auto&) { return std::vector<char>{'i'}; };

   outpost_opp_job job(client, depot);
   depot.deliver_accepted = false;
   job.run_inbound();
   BOOST_CHECK_EQUAL(depot.deliver_calls.size(), 1u);
   BOOST_CHECK(depot.chkcons_if_due_calls.empty());

   depot.deliver_accepted = true;
   job.run_inbound();
   BOOST_CHECK_EQUAL(depot.deliver_calls.size(), 2u);
   BOOST_CHECK(depot.chkcons_if_due_calls == std::vector<uint64_t>{chain_code});
}

// ─── run_backlog ────────────────────────────────────────────────────────────

namespace {

/// `outpost_opp_job`'s outpost backlog reads per chain minimum while nothing is reported.
constexpr int64_t kBacklogReadsPerMinimum = 4;

/// An operator elected on the chain's decided epoch `kTestEpoch`, whose outpost emitted that epoch and holds a backlog.
/// The job reads the fixture's clock, which moves only when a case moves it.
struct backlog_fixture {
   std::shared_ptr<mock_outpost_client> client = make_client(CHAIN_KIND_SVM, kSolanaChainCode, kSolanaChainId);
   mock_depot_ops  depot;
   fc::time_point  now = fc::time_point::now();
   outpost_opp_job job{client, depot, [this] { return now; }};
   static constexpr uint32_t pending   = 3;
   static constexpr uint64_t oldest_ms = 1'800'000'000'123;
   /// The epochs whose `qreports` row the depot holds for this operator.
   std::set<uint32_t> reported;

   /// The least time between two backlog reads while nothing is reported.
   fc::microseconds read_pace() const {
      return fc::microseconds(depot.chain_minimum.count() / kBacklogReadsPerMinimum);
   }

   backlog_fixture() {
      depot.epoch   = kTestEpoch;
      depot.decided = true;
      client->backlog_response = [] {
         return outbound_backlog{
            .outbound_epoch = kTestEpoch, .pending_count = pending, .oldest_pending_ms = oldest_ms};
      };
      depot.has_reported_response = [this](uint64_t, uint32_t epoch) { return reported.contains(epoch); };
   }
};

} // namespace

/// A decided epoch whose emitted envelope leaves a backlog is reported once, with the count and age from one read.
BOOST_FIXTURE_TEST_CASE(run_backlog_reports_a_decided_epochs_backlog_once, backlog_fixture) {
   job.run_backlog();
   BOOST_REQUIRE_EQUAL(depot.report_calls.size(), 1u);
   BOOST_CHECK_EQUAL(depot.report_calls[0].chain_code, kSolanaChainCode);
   BOOST_CHECK_EQUAL(depot.report_calls[0].epoch_index, kTestEpoch);
   BOOST_CHECK_EQUAL(depot.report_calls[0].pending_count, pending);
   BOOST_CHECK_EQUAL(depot.report_calls[0].oldest_pending_ms, oldest_ms);
   BOOST_REQUIRE_EQUAL(client->backlog_calls.size(), 1u);
   BOOST_CHECK(client->backlog_calls[0] == kDeadline);

   // The report now stands: within the grace nothing is read or reported again.
   reported.insert(kTestEpoch);
   for (unsigned tick = 0; tick < kRepeatedPolls; ++tick) job.run_backlog();
   BOOST_CHECK_EQUAL(client->backlog_calls.size(), 1u);
   BOOST_CHECK_EQUAL(depot.report_calls.size(), 1u);

   // The next epoch is reported afresh, once the read pace allows.
   now += read_pace();
   depot.epoch = kTestEpoch + 1;
   client->backlog_response = [] {
      return outbound_backlog{.outbound_epoch = kTestEpoch + 1, .pending_count = 1, .oldest_pending_ms = oldest_ms};
   };
   job.run_backlog();
   BOOST_REQUIRE_EQUAL(depot.report_calls.size(), 2u);
   BOOST_CHECK_EQUAL(depot.report_calls[1].epoch_index, kTestEpoch + 1);
}

/// An undecided epoch, or one this operator is not elected on, is neither read nor reported.
BOOST_FIXTURE_TEST_CASE(run_backlog_waits_for_a_decided_epoch_it_is_elected_on, backlog_fixture) {
   depot.decided = false;
   job.run_backlog();
   depot.decided = true;
   depot.elected = false;
   job.run_backlog();
   depot.elected = true;
   depot.epoch   = 0;
   job.run_backlog();
   BOOST_CHECK(client->backlog_calls.empty());
   BOOST_CHECK(depot.has_reported_calls.empty());
   BOOST_CHECK(depot.report_calls.empty());
}

/// A backlog is reported only once the outpost emitted the depot's current epoch, and only when something is queued.
BOOST_FIXTURE_TEST_CASE(run_backlog_needs_the_current_epochs_emit_and_a_backlog, backlog_fixture) {
   for (const uint32_t emitted : {kTestEpoch - 1, kTestEpoch + 1, 0u}) {
      client->backlog_response = [emitted] {
         return outbound_backlog{.outbound_epoch = emitted, .pending_count = pending, .oldest_pending_ms = oldest_ms};
      };
      job.run_backlog();
      now += read_pace();
   }
   client->backlog_response = [] { return outbound_backlog{.outbound_epoch = kTestEpoch}; };
   job.run_backlog();
   BOOST_CHECK_EQUAL(client->backlog_calls.size(), 4u);
   BOOST_CHECK(depot.report_calls.empty());
}

/// A refused report, or a failed read, leaves the epoch unreported: a cycle past the read pace tries again.
BOOST_FIXTURE_TEST_CASE(run_backlog_tries_again_after_a_refusal_or_failed_read, backlog_fixture) {
   depot.report_accepted = false;
   job.run_backlog();
   depot.report_accepted = true;
   now += read_pace();
   job.run_backlog();
   BOOST_CHECK_EQUAL(depot.report_calls.size(), 2u);

   backlog_fixture failing;
   failing.client->backlog_response = []() -> outbound_backlog { FC_THROW("outpost unreachable"); };
   BOOST_CHECK_NO_THROW(failing.job.run_backlog());
   BOOST_CHECK(failing.depot.report_calls.empty());
   failing.client->backlog_response = [] {
      return outbound_backlog{.outbound_epoch = kTestEpoch, .pending_count = pending, .oldest_pending_ms = oldest_ms};
   };
   failing.now += failing.read_pace();
   failing.job.run_backlog();
   BOOST_CHECK_EQUAL(failing.depot.report_calls.size(), 1u);
}

/// While nothing is reported for the epoch, the outpost is read at most a quarter of the chain minimum apart, not
/// every cycle, whatever the read found or however it failed.
BOOST_FIXTURE_TEST_CASE(run_backlog_reads_an_unreported_epoch_at_its_pace, backlog_fixture) {
   client->backlog_response = [] { return outbound_backlog{.outbound_epoch = kTestEpoch}; };
   for (unsigned tick = 0; tick < kRepeatedPolls; ++tick) job.run_backlog();
   BOOST_CHECK_EQUAL(client->backlog_calls.size(), 1u);
   now += read_pace() - fc::microseconds(1);
   job.run_backlog();
   BOOST_CHECK_EQUAL(client->backlog_calls.size(), 1u);
   now += fc::microseconds(1);
   job.run_backlog();
   BOOST_CHECK_EQUAL(client->backlog_calls.size(), 2u);

   client->backlog_response = []() -> outbound_backlog { FC_THROW("outpost unreachable"); };
   now += read_pace();
   for (unsigned tick = 0; tick < kRepeatedPolls; ++tick) job.run_backlog();
   BOOST_CHECK_EQUAL(client->backlog_calls.size(), 3u);
   BOOST_CHECK(depot.report_calls.empty());
}

/// The backlog job keeps its own lock: a delivery goes ahead while a backlog read is still waiting on the outpost.
BOOST_FIXTURE_TEST_CASE(run_backlog_never_holds_up_a_delivery, backlog_fixture) {
   static constexpr auto    stalled_read_limit = 30s;
   std::promise<void>       release;
   std::shared_future<void> released = release.get_future().share();
   std::atomic<bool>        reading{false};
   std::atomic<bool>        read_done{false};
   client->backlog_response = [&reading, &read_done, released] {
      reading = true;
      released.wait_for(stalled_read_limit);
      read_done = true;
      return outbound_backlog{.outbound_epoch = kTestEpoch, .pending_count = pending, .oldest_pending_ms = oldest_ms};
   };
   depot.pending_response = [](uint64_t, uint32_t) -> std::optional<outbound_envelope_record> {
      outbound_envelope_record record;
      record.raw_envelope = {'x'};
      return record;
   };

   std::thread backlog([this] { job.run_backlog(); });
   while (!reading) std::this_thread::sleep_for(1ms);
   job.run_outbound();
   BOOST_CHECK(!read_done);
   BOOST_CHECK_EQUAL(client->outbound_calls.size(), 1u);
   release.set_value();
   backlog.join();
   BOOST_CHECK_EQUAL(depot.report_calls.size(), 1u);
}

/// A standing report is made again while the chain stays idle a chain minimum after the last one, re-running the
/// depot's tally; never while the chain has traffic. A report found standing (after a restart) starts that grace then.
BOOST_FIXTURE_TEST_CASE(run_backlog_reports_again_while_the_chain_stays_idle_past_the_grace, backlog_fixture) {
   const fc::microseconds grace = depot.chain_minimum;
   reported.insert(kTestEpoch);

   job.run_backlog();   // found standing: the grace starts now
   BOOST_CHECK(client->backlog_calls.empty());
   now += grace - fc::microseconds(1);
   job.run_backlog();
   BOOST_CHECK(client->backlog_calls.empty());

   now += fc::microseconds(1);
   depot.traffic_recorded = true;
   job.run_backlog();   // the chain has traffic: the report did its job
   BOOST_CHECK(client->backlog_calls.empty());

   depot.traffic_recorded = false;
   job.run_backlog();   // idle past the grace: reported again
   BOOST_CHECK_EQUAL(client->backlog_calls.size(), 1u);
   BOOST_REQUIRE_EQUAL(depot.report_calls.size(), 1u);
   BOOST_CHECK_EQUAL(depot.report_calls[0].epoch_index, kTestEpoch);

   now += grace - fc::microseconds(1);
   job.run_backlog();   // the grace runs from the re-report
   BOOST_CHECK_EQUAL(depot.report_calls.size(), 1u);
   now += fc::microseconds(1);
   job.run_backlog();
   BOOST_CHECK_EQUAL(depot.report_calls.size(), 2u);
}

// ─── per-chain epochs ───────────────────────────────────────────────────────

namespace {

/// Two outposts sharing one depot, each on its own consensus epoch: this operator is elected on chain A's epoch 7
/// and not on chain B's epoch 3.
constexpr uint64_t kChainA      = 11;
constexpr uint64_t kChainB      = 12;
constexpr uint32_t kChainAEpoch = 7;
constexpr uint32_t kChainBEpoch = 3;
constexpr uint32_t kChainAId    = 31337;
constexpr uint32_t kChainBId    = 0;

/// A depot with chain A elected at its epoch and chain B not, every envelope pending.
struct two_chain_fixture {
   std::shared_ptr<mock_outpost_client> client_a = make_client(CHAIN_KIND_EVM, kChainA, kChainAId);
   std::shared_ptr<mock_outpost_client> client_b = make_client(CHAIN_KIND_SVM, kChainB, kChainBId);
   mock_depot_ops  depot;
   outpost_opp_job job_a{client_a, depot};
   outpost_opp_job job_b{client_b, depot};

   two_chain_fixture() {
      // The defaults would elect every chain; each chain answers with its own knobs instead.
      depot.elected        = false;
      depot.chains[kChainA] = mock_depot_ops::chain_knobs{.elected = true, .epoch = kChainAEpoch};
      depot.chains[kChainB] = mock_depot_ops::chain_knobs{.elected = false, .epoch = kChainBEpoch};
      depot.pending_response = [](uint64_t chain_code, uint32_t epoch) -> std::optional<outbound_envelope_record> {
         outbound_envelope_record record;
         record.chain_code   = chain_code;
         record.epoch_index  = epoch;
         record.raw_envelope = {'x'};
         return record;
      };
   }

   /// Whether every epoch query so far named `chain_code`.
   bool queried_only(uint64_t chain_code) const {
      return !depot.epoch_queries.empty() &&
             std::ranges::all_of(depot.epoch_queries, [&](uint64_t queried) { return queried == chain_code; });
   }
};

} // namespace

/// A job for chain A consults chain A's epoch and election, never chain B's: it delivers A's epoch, and B's job,
/// not elected on B's own epoch, does nothing.
BOOST_FIXTURE_TEST_CASE(each_job_consults_its_own_chains_epoch, two_chain_fixture) {
   job_a.run_outbound();
   BOOST_CHECK(queried_only(kChainA));
   BOOST_REQUIRE_EQUAL(client_a->outbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client_a->outbound_calls[0].epoch_index, kChainAEpoch);
   BOOST_REQUIRE_EQUAL(depot.read_pending_calls.size(), 1u);
   BOOST_CHECK_EQUAL(depot.read_pending_calls[0].chain_code, kChainA);
   BOOST_CHECK_EQUAL(depot.read_pending_calls[0].epoch_index, kChainAEpoch);

   depot.epoch_queries.clear();
   job_b.run_outbound();
   job_b.run_inbound();
   job_b.run_backlog();
   BOOST_CHECK(queried_only(kChainB));
   BOOST_CHECK(client_b->outbound_calls.empty());
   BOOST_CHECK(client_b->inbound_calls.empty());
   BOOST_CHECK(client_b->backlog_calls.empty());
   BOOST_CHECK_EQUAL(depot.read_pending_calls.size(), 1u);
}

/// Each job hands every outpost call its own chain's delivery timeout: the delivery, the crank, the inbound read and
/// the backlog read.
BOOST_FIXTURE_TEST_CASE(each_job_gives_its_outpost_calls_its_chains_delivery_timeout, two_chain_fixture) {
   const fc::microseconds chain_a_timeout = fc::seconds(30);
   const fc::microseconds chain_b_timeout = fc::seconds(7);
   depot.chains[kChainA].delivery_timeout = chain_a_timeout;
   depot.chains[kChainA].decided          = true;
   depot.chains[kChainB] = mock_depot_ops::chain_knobs{
      .elected = true, .epoch = kChainBEpoch, .decided = true, .delivery_timeout = chain_b_timeout};

   for (outpost_opp_job* job : {&job_a, &job_b}) {
      job->run_outbound();
      job->run_inbound();
      job->run_backlog();
   }
   for (const auto& [client, timeout] : {std::pair{client_a, chain_a_timeout}, std::pair{client_b, chain_b_timeout}}) {
      BOOST_REQUIRE_EQUAL(client->outbound_calls.size(), 1u);
      BOOST_REQUIRE_EQUAL(client->crank_calls.size(), 1u);
      BOOST_REQUIRE_EQUAL(client->inbound_calls.size(), 1u);
      BOOST_REQUIRE_EQUAL(client->backlog_calls.size(), 1u);
      BOOST_CHECK(client->outbound_calls[0].deadline == timeout);
      BOOST_CHECK(client->crank_calls[0].deadline == timeout);
      BOOST_CHECK(client->inbound_calls[0].deadline == timeout);
      BOOST_CHECK(client->backlog_calls[0] == timeout);
   }
}

/// Electing chain B on its own epoch lets B's job deliver B's epoch, while A keeps delivering A's.
BOOST_FIXTURE_TEST_CASE(each_job_delivers_its_own_chains_epoch_index, two_chain_fixture) {
   depot.chains[kChainB].elected = true;
   const std::vector<char> inbound{'i', 'n'};
   client_a->inbound_response = [inbound](const auto&) { return inbound; };
   client_b->inbound_response = [inbound](const auto&) { return inbound; };

   job_a.run_inbound();
   job_b.run_inbound();

   BOOST_REQUIRE_EQUAL(client_a->inbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client_a->inbound_calls[0].epoch_index, kChainAEpoch);
   BOOST_REQUIRE_EQUAL(client_b->inbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client_b->inbound_calls[0].epoch_index, kChainBEpoch);
   BOOST_REQUIRE_EQUAL(depot.has_delivered_calls.size(), 2u);
   BOOST_CHECK_EQUAL(depot.has_delivered_calls[0].chain_code, kChainA);
   BOOST_CHECK_EQUAL(depot.has_delivered_calls[0].epoch_index, kChainAEpoch);
   BOOST_CHECK_EQUAL(depot.has_delivered_calls[1].chain_code, kChainB);
   BOOST_CHECK_EQUAL(depot.has_delivered_calls[1].epoch_index, kChainBEpoch);
   BOOST_REQUIRE_EQUAL(depot.deliver_calls.size(), 2u);
   BOOST_CHECK_EQUAL(depot.deliver_calls[0].chain_code, kChainA);
   BOOST_CHECK_EQUAL(depot.deliver_calls[1].chain_code, kChainB);
   BOOST_CHECK(depot.chkcons_if_due_calls == (std::vector<uint64_t>{kChainA, kChainB}));
}

/// Each job answers to its own chain's completion: A's recorded reply stops A alone.
BOOST_FIXTURE_TEST_CASE(each_job_follows_its_own_chains_completion, two_chain_fixture) {
   depot.chains[kChainB].elected = true;
   job_a.run_outbound();
   job_b.run_outbound();
   BOOST_CHECK_EQUAL(client_a->outbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client_b->outbound_calls.size(), 1u);

   // A's inbound reply is recorded, B's is not: only B keeps calling its client.
   depot.has_delivered_response = [](uint64_t chain_code, uint32_t) { return chain_code == kChainA; };
   job_a.run_outbound();
   job_b.run_outbound();
   BOOST_CHECK_EQUAL(client_a->outbound_calls.size(), 1u);
   BOOST_CHECK_EQUAL(client_b->outbound_calls.size(), 2u);
}

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

   outpost_opp_job job(client, depot);

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
