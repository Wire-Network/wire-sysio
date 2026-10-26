#include <fc/filesystem.hpp>
#include <sysio/producer_plugin/detail/snapshot_provider_schedule.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace {

namespace provider_detail = sysio::producer_plugin_detail;
using disposition = provider_detail::snapshot_provider_schedule_disposition;
using snapshot_scheduler = sysio::chain::snapshot_scheduler;

/** Enabled cadence used by reconciliation tests. */
constexpr uint32_t configured_block_spacing = 17;
/** Mismatched provider-owned cadence used to exercise replacement. */
constexpr uint32_t stale_block_spacing = 19;
/** Operator-owned cadence used to exercise preservation. */
constexpr uint32_t manual_block_spacing = 7;
/** Open-ended recurrence boundary used by persisted schedules. */
constexpr uint32_t maximum_block_num = std::numeric_limits<uint32_t>::max();

/** Own a temporary persisted scheduler for each reconciliation test. */
struct snapshot_provider_schedule_fixture {
   fc::temp_directory temp_directory;
   snapshot_scheduler scheduler;

   snapshot_provider_schedule_fixture() { scheduler.set_db_path(temp_directory.path()); }
};

/** Add an operator-owned recurrence spanning every future block at the requested cadence. */
snapshot_scheduler::snapshot_schedule_result schedule_manual(snapshot_scheduler& scheduler, uint32_t block_spacing,
                                                             std::string description = "operator manual") {
   return scheduler.schedule_snapshot(
      snapshot_scheduler::snapshot_request_information{
         .block_spacing = block_spacing,
         .start_block_num = block_spacing,
         .end_block_num = maximum_block_num,
         .snapshot_description = std::move(description),
      },
      {});
}

/** Find a scheduled request by its stable identifier. */
const snapshot_scheduler::snapshot_schedule_information*
find_request(const snapshot_scheduler::get_snapshot_requests_result& requests, uint32_t request_id) {
   const auto found =
      std::find_if(requests.snapshot_requests.begin(), requests.snapshot_requests.end(),
                   [request_id](const auto& request) { return request.snapshot_request_id == request_id; });
   return found == requests.snapshot_requests.end() ? nullptr : &*found;
}

} // namespace

BOOST_AUTO_TEST_SUITE(snapshot_provider_schedule_tests)

BOOST_FIXTURE_TEST_CASE(absent_then_enabled_configuration_creates_one_stable_request,
                        snapshot_provider_schedule_fixture) {
   const auto absent = provider_detail::reconcile_snapshot_provider_schedule(scheduler, std::nullopt);
   BOOST_CHECK(absent.disposition == disposition::disabled);
   BOOST_CHECK(!absent.block_spacing);
   BOOST_CHECK(!absent.request_id);
   BOOST_CHECK(absent.removed_request_ids.empty());
   BOOST_CHECK(scheduler.get_snapshot_requests().snapshot_requests.empty());

   const auto enabled = provider_detail::reconcile_snapshot_provider_schedule(scheduler, configured_block_spacing);
   BOOST_CHECK(enabled.disposition == disposition::created_automatic);
   BOOST_REQUIRE(enabled.block_spacing);
   BOOST_CHECK_EQUAL(*enabled.block_spacing, configured_block_spacing);
   BOOST_REQUIRE(enabled.request_id);

   const auto requests = scheduler.get_snapshot_requests();
   BOOST_REQUIRE_EQUAL(requests.snapshot_requests.size(), 1U);
   const auto* automatic = find_request(requests, *enabled.request_id);
   BOOST_REQUIRE(automatic);
   BOOST_CHECK_EQUAL(automatic->block_spacing, configured_block_spacing);
   BOOST_CHECK_EQUAL(automatic->start_block_num, configured_block_spacing);
   BOOST_CHECK_EQUAL(automatic->end_block_num, maximum_block_num);
   BOOST_CHECK_EQUAL(automatic->snapshot_description, provider_detail::snapshot_provider_schedule_description);

   const auto unchanged = provider_detail::reconcile_snapshot_provider_schedule(scheduler, configured_block_spacing);
   BOOST_CHECK(unchanged.disposition == disposition::reused_automatic);
   BOOST_REQUIRE(unchanged.request_id);
   BOOST_CHECK_EQUAL(*unchanged.request_id, *enabled.request_id);
   BOOST_CHECK(unchanged.removed_request_ids.empty());
   BOOST_CHECK_EQUAL(scheduler.get_snapshot_requests().snapshot_requests.size(), 1U);
}

BOOST_FIXTURE_TEST_CASE(stale_reserved_request_is_replaced, snapshot_provider_schedule_fixture) {
   const auto stale = provider_detail::reconcile_snapshot_provider_schedule(scheduler, stale_block_spacing);
   BOOST_REQUIRE(stale.request_id);

   const auto replaced = provider_detail::reconcile_snapshot_provider_schedule(scheduler, configured_block_spacing);
   BOOST_CHECK(replaced.disposition == disposition::created_automatic);
   BOOST_REQUIRE(replaced.request_id);
   BOOST_CHECK_NE(*replaced.request_id, *stale.request_id);
   BOOST_REQUIRE_EQUAL(replaced.removed_request_ids.size(), 1U);
   BOOST_CHECK_EQUAL(replaced.removed_request_ids.front(), *stale.request_id);

   const auto requests = scheduler.get_snapshot_requests();
   BOOST_REQUIRE_EQUAL(requests.snapshot_requests.size(), 1U);
   const auto* automatic = find_request(requests, *replaced.request_id);
   BOOST_REQUIRE(automatic);
   BOOST_CHECK_EQUAL(automatic->block_spacing, configured_block_spacing);
   BOOST_CHECK_EQUAL(automatic->snapshot_description, provider_detail::snapshot_provider_schedule_description);
}

BOOST_FIXTURE_TEST_CASE(exact_manual_recurrence_is_reused_without_reserved_request,
                        snapshot_provider_schedule_fixture) {
   const auto manual = schedule_manual(scheduler, configured_block_spacing);

   const auto reused = provider_detail::reconcile_snapshot_provider_schedule(scheduler, configured_block_spacing);
   BOOST_CHECK(reused.disposition == disposition::reused_manual);
   BOOST_REQUIRE(reused.request_id);
   BOOST_CHECK_EQUAL(*reused.request_id, manual.snapshot_request_id);
   BOOST_CHECK(reused.removed_request_ids.empty());

   const auto requests = scheduler.get_snapshot_requests();
   BOOST_REQUIRE_EQUAL(requests.snapshot_requests.size(), 1U);
   const auto* preserved = find_request(requests, manual.snapshot_request_id);
   BOOST_REQUIRE(preserved);
   BOOST_CHECK_EQUAL(preserved->snapshot_description, "operator manual");
}

BOOST_FIXTURE_TEST_CASE(disabling_removes_reserved_request_and_preserves_manual_schedule,
                        snapshot_provider_schedule_fixture) {
   const auto automatic = provider_detail::reconcile_snapshot_provider_schedule(scheduler, configured_block_spacing);
   BOOST_REQUIRE(automatic.request_id);
   const auto manual = schedule_manual(scheduler, manual_block_spacing);

   const auto disabled = provider_detail::reconcile_snapshot_provider_schedule(scheduler, std::nullopt);
   BOOST_CHECK(disabled.disposition == disposition::disabled);
   BOOST_CHECK(!disabled.block_spacing);
   BOOST_CHECK(!disabled.request_id);
   BOOST_REQUIRE_EQUAL(disabled.removed_request_ids.size(), 1U);
   BOOST_CHECK_EQUAL(disabled.removed_request_ids.front(), *automatic.request_id);

   const auto requests = scheduler.get_snapshot_requests();
   BOOST_REQUIRE_EQUAL(requests.snapshot_requests.size(), 1U);
   const auto* preserved = find_request(requests, manual.snapshot_request_id);
   BOOST_REQUIRE(preserved);
   BOOST_CHECK_EQUAL(preserved->snapshot_description, "operator manual");
}

BOOST_FIXTURE_TEST_CASE(removed_reserved_request_is_repaired, snapshot_provider_schedule_fixture) {
   const auto original = provider_detail::reconcile_snapshot_provider_schedule(scheduler, configured_block_spacing);
   BOOST_REQUIRE(original.request_id);
   scheduler.unschedule_snapshot(*original.request_id);

   const auto repaired = provider_detail::reconcile_snapshot_provider_schedule(scheduler, configured_block_spacing);
   BOOST_CHECK(repaired.disposition == disposition::created_automatic);
   BOOST_REQUIRE(repaired.request_id);
   BOOST_CHECK_NE(*repaired.request_id, *original.request_id);
   BOOST_CHECK(repaired.removed_request_ids.empty());

   const auto requests = scheduler.get_snapshot_requests();
   BOOST_REQUIRE_EQUAL(requests.snapshot_requests.size(), 1U);
   const auto* replacement = find_request(requests, *repaired.request_id);
   BOOST_REQUIRE(replacement);
   BOOST_CHECK_EQUAL(replacement->snapshot_description, provider_detail::snapshot_provider_schedule_description);
}

BOOST_AUTO_TEST_SUITE_END()
