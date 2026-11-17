#include "../src/synd_crank.hpp"

#include <boost/test/unit_test.hpp>

using namespace sysio::batch_operator_detail::underwriter;
using mvo = fc::mutable_variant_object;

namespace {
const synd_pair pair{fc::slug_name{"ETH"}, fc::slug_name{"LIQETH"}};
constexpr uint32_t queue_epoch = 10;
constexpr uint32_t refill_epoch = 20;

/// Values-only rows and query accounting for a pair with an acknowledged request and pending principal.
struct synd_fixture {
   fc::variants ledger{mvo()("queue_epoch", queue_epoch)};
   fc::variants envelopes{mvo()("chain_code", "ETH")("token_code", "LIQETH")("epoch_index", queue_epoch)
      ("state", "RELEASABLE")("request_id", 1)("released", 50)("synd_total", 100)("yield_total", 0)};
   fc::variants configs{mvo()("synd_burst", 50)("synd_refill", 50)};
   fc::variants buckets{mvo()("level", 0)("last_epoch", refill_epoch)};
   fc::variants items;
   std::string fail_table;
   uint32_t calls = 0;
   uint32_t rows = 0;

   /// Envelopes carry independent outcome and invalidation progress even after requests have been pruned.
   synd_fixture() {
      set("outcome", "APPROVED");
      set("burned", 0);
   }

   /// Enforce the production query bounds, including the persisted cursor used to skip retained history.
   std::optional<fc::variants> read(const char* table, const fc::variant_object& key, bool exact, uint32_t limit) {
      ++calls;
      BOOST_CHECK_EQUAL(key[synd::field::chain_code].as_uint64(), pair.first.value);
      BOOST_CHECK_EQUAL(key[synd::field::token_code].as_uint64(), pair.second.value);
      if (fail_table == table) return std::nullopt;
      const fc::variants* values = nullptr;
      if (table == synd::table_ledger) values = &ledger;
      if (table == synd::table_envelopes || table == synd::table_items) {
         values = &envelopes;
         if (table == synd::table_items) values = &items;
         BOOST_CHECK(!exact);
         BOOST_CHECK_EQUAL(limit, synd::envelopes_per_pair);
         BOOST_CHECK_EQUAL(key[synd::field::epoch_index].as_uint64(), queue_epoch);
      } else {
         BOOST_CHECK(exact);
         BOOST_CHECK_EQUAL(limit, 1u);
      }
      if (table == synd::table_syndconfig) values = &configs;
      if (table == synd::table_buckets) {
         values = &buckets;
         BOOST_CHECK_EQUAL(key[synd::field::direction].as_uint64(),
                           magic_enum::enum_integer(bucket_direction::SYNDICATION));
      }
      BOOST_REQUIRE(values);
      auto result = *values;
      if (result.size() > limit) result.resize(limit);
      rows += result.size();
      return result;
   }

   /// Observe the same production reader without a chain or RPC service.
   std::optional<synd_work> observe(uint32_t epoch, const std::vector<request>& requests = {}) {
      return read_synd_work(pair, epoch, requests,
         [this](const char* table, const fc::variant_object& key, bool exact, uint32_t limit) {
            return read(table, key, exact, limit);
         });
   }

   /// Change only the first envelope's lifecycle or progress.
   void set(const char* field, const fc::variant& value) {
      envelopes.front() = mvo(envelopes.front().get_object()).set(field, value);
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(synd_crank_tests)

/// Acknowledgment and pruning of bond requests must not remove pending release from scheduling.
BOOST_AUTO_TEST_CASE(release_survives_acknowledged_or_pruned_requests_and_restart) {
   synd_fixture fixture;
   request settled{.id = 1, .issuer = sysio::chain::name{synd::account},
      .schema = sysio::chain::name{envelope_schema}, .state = request_state::APPROVED,
      .outcome_acknowledged = true};
   const auto acknowledged = fixture.observe(refill_epoch + 1, {settled});
   const auto pruned = fixture.observe(refill_epoch + 1);
   BOOST_REQUIRE(acknowledged);
   BOOST_REQUIRE(pruned);
   BOOST_CHECK(acknowledged->ready);
   BOOST_CHECK(*acknowledged == *pruned);
   synd_crank_retry restarted;
   BOOST_CHECK(restarted.due(*pruned, fc::time_point{}));
   BOOST_CHECK(!synd_sync_work({settled}).ready);
   settled.outcome_acknowledged = false;
   BOOST_CHECK(synd_sync_work({settled}).ready);
}

/// Capacity becomes eligible at a depot epoch change, including a zero-refill bucket with residual capacity.
BOOST_AUTO_TEST_CASE(refill_waits_without_submitting_and_wakes_on_epoch_progress) {
   synd_fixture fixture;
   BOOST_CHECK(!fixture.observe(refill_epoch)->ready);
   BOOST_CHECK(!fixture.observe(refill_epoch - 1)->ready);
   BOOST_CHECK(fixture.observe(refill_epoch + 1)->ready);
   fixture.configs = {mvo()("synd_burst", 0)("synd_refill", 50)};
   BOOST_CHECK(!fixture.observe(refill_epoch + 1)->ready);
   fixture.configs = {mvo()("synd_burst", 50)("synd_refill", 0)};
   BOOST_CHECK(!fixture.observe(refill_epoch + 1)->ready);
   fixture.buckets = {mvo()("level", 1)("last_epoch", refill_epoch)};
   BOOST_CHECK(fixture.observe(refill_epoch)->ready);
   fixture.buckets.clear(); // the contract initializes a missing bucket full
   BOOST_CHECK(fixture.observe(refill_epoch)->ready);
   fixture.configs.clear();
   BOOST_CHECK(!fixture.observe(refill_epoch + 1)->ready);
}

/// Quiet, open and held queues do not become busy solely because a request still exists.
BOOST_AUTO_TEST_CASE(idle_and_held_envelopes_do_not_request_release) {
   for (const auto* state : {"OPEN", "HELD", "DONE", "INVALID"}) {
      synd_fixture fixture;
      fixture.set("state", state);
      if (std::string(state) == "HELD") fixture.set("outcome", "PENDING");
      BOOST_CHECK(!fixture.observe(refill_epoch + 1)->ready);
   }
   synd_fixture empty;
   empty.ledger.clear();
   BOOST_CHECK(!empty.observe(refill_epoch + 1)->ready);
   BOOST_CHECK_EQUAL(empty.calls, 1u);
   synd_fixture finished;
   finished.set("released", 100);
   BOOST_CHECK(!finished.observe(refill_epoch + 1)->ready);
   BOOST_CHECK_EQUAL(finished.calls, 2u);
}

/// A waiting unbonded prefix still gates FIFO; bonded requests need refresh, and held rows can be bypassed.
BOOST_AUTO_TEST_CASE(request_refresh_and_fifo_prefix_are_preserved) {
   synd_fixture fixture;
   fixture.set("state", "REQUESTED");
   fixture.set("outcome", "PENDING");
   request row{.id = 1, .state = request_state::OPEN};
   BOOST_CHECK(!fixture.observe(refill_epoch + 1, {row})->ready);
   row.state = request_state::HELD;
   BOOST_CHECK(!fixture.observe(refill_epoch + 1, {row})->ready);
   row.state = request_state::BONDED;
   BOOST_CHECK(fixture.observe(refill_epoch + 1, {row})->ready);
   fixture.set("state", "WAITING");
   BOOST_CHECK(fixture.observe(refill_epoch)->ready);
   const auto releasable = fixture.envelopes.front();
   fixture.set("state", "HELD");
   fixture.envelopes.push_back(mvo(releasable.get_object()).set("state", "RELEASABLE"));
   BOOST_CHECK(fixture.observe(refill_epoch + 1)->ready);
}

/// Read failures withhold the entire observation, while malformed ABI rows propagate to the worker's logged catch.
BOOST_AUTO_TEST_CASE(read_failures_are_distinct_from_empty_queues) {
   for (const auto* table : {synd::table_ledger, synd::table_envelopes, synd::table_syndconfig, synd::table_buckets}) {
      synd_fixture fixture;
      fixture.fail_table = table;
      BOOST_CHECK(!fixture.observe(refill_epoch + 1));
   }
   synd_fixture malformed;
   malformed.set("state", "UNKNOWN");
   BOOST_CHECK_THROW(malformed.observe(refill_epoch + 1), fc::exception);
}

/// Retained envelope history is skipped by the exact ledger cursor, and reads remain bounded on a full page.
BOOST_AUTO_TEST_CASE(queue_observation_has_fixed_row_and_call_bounds) {
   synd_fixture fixture;
   fixture.envelopes.resize(synd::envelopes_per_pair + 100, fixture.envelopes.front());
   BOOST_CHECK(fixture.observe(refill_epoch + 1)->ready);
   BOOST_CHECK_EQUAL(fixture.calls, 4u);
   BOOST_CHECK_EQUAL(fixture.rows, synd::envelopes_per_pair + 3);
}

/// A ruling rewinds held envelopes independently of request acknowledgment, and invalidation may span cranks.
BOOST_AUTO_TEST_CASE(ruled_holds_and_finished_history_continue_without_idle_delay) {
   for (const auto* outcome : {"VALID", "INVALID"}) {
      synd_fixture fixture;
      fixture.set("state", "HELD");
      fixture.set("outcome", outcome);
      const auto work = *fixture.observe(refill_epoch);
      BOOST_CHECK(work.ready);
      synd_crank_retry retry;
      retry.note(work, fc::time_point{});
      fixture.set("burned", 10);
      BOOST_CHECK(retry.due(*fixture.observe(refill_epoch), fc::time_point{}));
   }
   synd_fixture history;
   history.set("state", "HELD");
   history.set("outcome", "PENDING");
   history.envelopes.resize(synd::envelopes_per_pair, history.envelopes.front());
   BOOST_CHECK(history.observe(refill_epoch)->ready);
}

/// Lifetime yield totals are not pending yield; only remaining YIELD items can bypass an empty principal bucket.
BOOST_AUTO_TEST_CASE(released_yield_does_not_make_refill_waiting_principal_ready) {
   synd_fixture fixture;
   fixture.set("yield_total", 25);
   fixture.set("released", 75);
   BOOST_CHECK(!fixture.observe(refill_epoch)->ready);
   fixture.items = {mvo()("chain_code", "ETH")("token_code", "LIQETH")("epoch_index", queue_epoch)
      ("id", 3)("kind", "YIELD")("remaining", 25)};
   BOOST_CHECK(fixture.observe(refill_epoch)->ready);
   fixture.items.front() = mvo(fixture.items.front().get_object()).set("remaining", 0);
   BOOST_CHECK(!fixture.observe(refill_epoch)->ready);
}

/// Invalidations bypass principal capacity in every unfinished lifecycle state and survive request pruning.
BOOST_AUTO_TEST_CASE(persisted_outcomes_precede_request_lookups_and_bucket_checks) {
   for (const auto* state : {"REQUESTED", "RELEASABLE", "HELD"}) {
      synd_fixture fixture;
      fixture.set("state", state);
      fixture.set("outcome", "INVALID");
      fixture.items = {
         mvo()("chain_code", "ETH")("token_code", "LIQETH")("epoch_index", queue_epoch)
            ("id", 1)("kind", "YIELD")("remaining", 25),
         mvo()("chain_code", "ETH")("token_code", "LIQETH")("epoch_index", queue_epoch)
            ("id", 2)("kind", "YIELD")("remaining", 25)};
      auto work = *fixture.observe(refill_epoch);
      BOOST_CHECK(work.ready);
      synd_crank_retry retry;
      retry.note(work, fc::time_point{});
      fixture.items.erase(fixture.items.begin()); // deletion advances no released or burned total
      BOOST_CHECK(retry.due(*fixture.observe(refill_epoch), fc::time_point{}));
   }
   for (const auto* outcome : {"APPROVED", "VALID"}) {
      synd_fixture fixture;
      fixture.set("state", "REQUESTED");
      fixture.set("outcome", outcome);
      BOOST_CHECK(fixture.observe(refill_epoch)->ready);
   }
}

/// An unchanged snapshot backs off even after a failed push; observed progress and new capacity wake immediately.
BOOST_AUTO_TEST_CASE(retry_spacing_tracks_progress_and_refill) {
   synd_fixture fixture;
   auto work = *fixture.observe(refill_epoch + 1);
   synd_crank_retry retry;
   const auto now = fc::time_point::from_iso_string("2026-11-17T00:00:00");
   BOOST_CHECK(retry.due(work, now));
   retry.note(work, now);
   BOOST_CHECK(!retry.due(work, now + fc::seconds(1)));
   BOOST_CHECK(retry.due(work, now + synd_crank_retry::initial_wait));
   retry.note(work, now + synd_crank_retry::initial_wait);
   BOOST_CHECK(!retry.due(work, now + synd_crank_retry::initial_wait * 2));
   fixture.set("released", 75);
   work = *fixture.observe(refill_epoch + 1);
   BOOST_CHECK(retry.due(work, now + fc::seconds(1)));
   retry.note(work, now);
   BOOST_CHECK(retry.due(*fixture.observe(refill_epoch + 2), now + fc::seconds(1)));
   BOOST_CHECK(!retry.due(*fixture.observe(refill_epoch), now + fc::hours(1)));
   for (int count = 0; count < 10; ++count) retry.note(work, now);
   BOOST_CHECK(retry.wait <= synd_crank_retry::maximum_wait);
}

BOOST_AUTO_TEST_SUITE_END()
