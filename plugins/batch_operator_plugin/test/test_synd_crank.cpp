#include "../src/synd_crank.hpp"

#include <boost/test/unit_test.hpp>
#include <chrono>

namespace uw = sysio::batch_operator_detail::underwriter;
namespace detail = sysio::batch_operator_detail;
namespace trace_field = detail::transaction_trace_field;
using mvo = fc::mutable_variant_object;
using push_results = sysio::chain_apis::read_write::push_transaction_results;
using push_result = sysio::chain::next_function_variant<push_results>;

namespace {
constexpr uint32_t initial_epoch = 20;
const fc::microseconds idle_interval = fc::minutes(5);
const fc::time_point start = fc::time_point::from_iso_string("2026-11-19T00:00:00");

/// A normal API result whose root action either emits an inline action or does no further work.
push_results trace_result(bool productive) {
   fc::variants children;
   if (productive) children.emplace_back(mvo());
   return {{}, mvo()(trace_field::action_traces,
      fc::variants{mvo()(trace_field::inline_traces, children)})};
}

/// Deliver a real immediate push result through the callback completion path.
void finish(detail::async_action_completion& completion, push_results result) {
   completion.complete_push_result(push_result{std::move(result)}, [] {}, [](const fc::exception_ptr&) {});
}

/// An acknowledged envelope request that no longer requests outcome synchronization.
uw::request settled_request() {
   return {.id = 1, .issuer = sysio::chain::name{uw::synd::account},
      .schema = sysio::chain::name{uw::envelope_schema}, .state = uw::request_state::APPROVED,
      .outcome_acknowledged = true};
}
} // namespace

BOOST_AUTO_TEST_SUITE(synd_crank_tests)

/// Approved, acknowledged, pruned and out-of-window requests cannot hide an epoch refill, including after restart.
BOOST_AUTO_TEST_CASE(epoch_refill_survives_request_settlement_and_restart) {
   uw::synd_crank_state state;
   const uw::plan actions;
   const std::vector<uw::request> settled{settled_request()};
   BOOST_CHECK(state.due(actions, settled, initial_epoch, start, idle_interval));
   state.note(initial_epoch, start);
   BOOST_CHECK(!state.due(actions, settled, initial_epoch, start + fc::seconds(15), idle_interval));
   BOOST_CHECK(state.due(actions, settled, initial_epoch + 1, start + fc::seconds(60), idle_interval));
   BOOST_CHECK(state.due(actions, {}, initial_epoch + 1, start + fc::seconds(60), idle_interval));
   uw::synd_crank_state restarted;
   BOOST_CHECK(restarted.due(actions, {}, initial_epoch, start, idle_interval));
}

/// Empty/refill-waiting work gets one epoch attempt, rather than a transaction on every poll.
BOOST_AUTO_TEST_CASE(empty_trace_waits_for_epoch_or_idle_fallback) {
   uw::synd_crank_state state;
   state.note(initial_epoch, start);
   state.completion = std::make_shared<detail::async_action_completion>();
   finish(*state.completion, trace_result(false));
   for (uint32_t poll = 1; poll < 20; ++poll)
      BOOST_CHECK(!state.due({}, {}, initial_epoch, start + fc::seconds(15 * poll), idle_interval));
   BOOST_CHECK(state.due({}, {}, initial_epoch, start + idle_interval, idle_interval));
   BOOST_CHECK(state.due({}, {}, initial_epoch + 1, start + fc::seconds(60), idle_interval));
   state.note(initial_epoch + 1, start + fc::seconds(60));
   BOOST_CHECK(!state.due({}, {}, initial_epoch + 1, start + fc::seconds(75), idle_interval));
}

/// Productive work can spend a shared contract budget; the next poll must continue even within the same epoch.
BOOST_AUTO_TEST_CASE(productive_trace_continues_and_next_attempt_resets_progress) {
   uw::synd_crank_state state;
   state.note(initial_epoch, start);
   state.completion = std::make_shared<detail::async_action_completion>();
   finish(*state.completion, trace_result(true));
   BOOST_CHECK(state.due({}, {}, initial_epoch, start + fc::seconds(15), idle_interval));
   state.note(initial_epoch, start + fc::seconds(15));
   BOOST_CHECK(!state.due({}, {}, initial_epoch, start + fc::seconds(30), idle_interval));
   state.completion = std::make_shared<detail::async_action_completion>();
   finish(*state.completion, trace_result(false));
   BOOST_CHECK(!state.due({}, {}, initial_epoch, start + fc::seconds(45), idle_interval));
}

/// The real planner suppresses accepts/approvals/claims under the cord; scheduling still allows contract sync.
BOOST_AUTO_TEST_CASE(frozen_planner_still_cranks_for_epoch_sync_and_productive_trace) {
   uw::plan_inputs in;
   in.frozen = true;
   in.requests = {settled_request()};
   in.requests.front().outcome_acknowledged = false;
   const uw::plan actions = uw::plan_actions(in);
   BOOST_CHECK(actions.accepts.empty());
   BOOST_CHECK(actions.approves.empty());
   BOOST_CHECK(actions.claims.empty());
   uw::synd_crank_state state;
   state.note(initial_epoch, start);
   BOOST_CHECK(state.due(actions, in.requests, initial_epoch, start + fc::seconds(15), idle_interval));
   in.requests.front().outcome_acknowledged = true;
   BOOST_CHECK(!state.due(actions, in.requests, initial_epoch, start + fc::seconds(15), idle_interval));
   BOOST_CHECK(state.due(actions, in.requests, initial_epoch + 1, start + fc::seconds(60), idle_interval));
   BOOST_CHECK(state.due(actions, in.requests, initial_epoch, start + idle_interval, idle_interval));
   state.completion = std::make_shared<detail::async_action_completion>();
   finish(*state.completion, trace_result(true));
   BOOST_CHECK(state.due(actions, in.requests, initial_epoch, start + fc::seconds(15), idle_interval));
}

/// An epoch read failure cannot suppress accepts, approvals, synchronization or the idle fallback.
BOOST_AUTO_TEST_CASE(epoch_read_failure_disables_only_epoch_trigger) {
   uw::synd_crank_state state;
   state.note(initial_epoch, start);
   uw::plan actions;
   BOOST_CHECK(!state.due(actions, {}, std::nullopt, start + fc::seconds(15), idle_interval));
   actions.accepts.resize(1);
   BOOST_CHECK(state.due(actions, {}, std::nullopt, start + fc::seconds(15), idle_interval));
   actions.accepts.clear();
   actions.approves.push_back(1);
   BOOST_CHECK(state.due(actions, {}, std::nullopt, start + fc::seconds(15), idle_interval));
   actions.approves.clear();
   uw::request request = settled_request();
   request.outcome_acknowledged = false;
   BOOST_CHECK(state.due(actions, {request}, std::nullopt, start + fc::seconds(15), idle_interval));
   BOOST_CHECK(state.due(actions, {}, std::nullopt, start + idle_interval, idle_interval));
   state.note(std::nullopt, start + fc::seconds(15));
   BOOST_REQUIRE(state.epoch);
   BOOST_CHECK_EQUAL(*state.epoch, initial_epoch);
   BOOST_CHECK(state.due(actions, {}, initial_epoch + 1, start + fc::seconds(60), idle_interval));
}

/// Decision time survives delayed accepts. An absent singleton is epoch zero, not an observation failure.
BOOST_AUTO_TEST_CASE(decision_clock_epoch_zero_and_skipped_epochs) {
   uw::synd_crank_state state;
   BOOST_CHECK(state.due({}, {}, 0, start, idle_interval));
   state.note(0, start);
   BOOST_CHECK(!state.due({}, {}, 0, start + fc::seconds(60), idle_interval));
   BOOST_CHECK(state.due({}, {}, 0, start + idle_interval, idle_interval));
   BOOST_CHECK(state.due({}, {}, initial_epoch, start + fc::seconds(60), idle_interval));
   state.note(initial_epoch, start + fc::seconds(60));
   BOOST_CHECK(!state.due({}, {}, initial_epoch, start + fc::seconds(75), idle_interval));
}

/// Both actual API trace forms report inline work; root actions and malformed output alone do not.
BOOST_AUTO_TEST_CASE(inline_progress_handles_nested_and_flat_trace_output) {
   BOOST_CHECK(detail::trace_has_inline_actions(trace_result(true).processed));
   BOOST_CHECK(!detail::trace_has_inline_actions(trace_result(false).processed));
   BOOST_CHECK(!detail::trace_has_inline_actions(fc::variant{}));
   BOOST_CHECK(!detail::trace_has_inline_actions(mvo()(trace_field::action_traces, false)));
   const fc::variant flat = mvo()(trace_field::action_traces,
      fc::variants{mvo()(trace_field::creator_action_ordinal, 0u),
                   mvo()(trace_field::creator_action_ordinal, 1u)});
   BOOST_CHECK(detail::trace_has_inline_actions(flat));
   const fc::variant root_only = mvo()(trace_field::action_traces,
      fc::variants{mvo()(trace_field::creator_action_ordinal, 0u)});
   BOOST_CHECK(!detail::trace_has_inline_actions(root_only));
}

/// Deferred results expose progress, and duplicate callbacks cannot replace the first result.
BOOST_AUTO_TEST_CASE(deferred_and_duplicate_callbacks_preserve_progress) {
   detail::async_action_completion completion;
   const push_result deferred{std::function<sysio::chain::t_or_exception<push_results>()>{[] {
      return sysio::chain::t_or_exception<push_results>{trace_result(true)};
   }}};
   completion.complete_push_result(deferred, [] {}, [](const fc::exception_ptr&) {});
   BOOST_CHECK(completion.succeeded());
   BOOST_CHECK(completion.has_inline_actions());
   finish(completion, trace_result(false));
   BOOST_CHECK(completion.has_inline_actions());
   detail::async_action_completion failed;
   const push_result error{std::make_shared<fc::exception>()};
   failed.complete_push_result(error, [] {}, [](const fc::exception_ptr&) {});
   BOOST_CHECK(!failed.succeeded());
   BOOST_CHECK(!failed.has_inline_actions());
}

/// A timed-out caller retains only shared completion state; its late real push callback wakes the next poll.
BOOST_AUTO_TEST_CASE(late_callback_wakes_scheduler_after_timeout) {
   uw::synd_crank_state state;
   state.note(initial_epoch, start);
   state.completion = std::make_shared<detail::async_action_completion>();
   std::future<void> done = state.completion->get_future();
   const sysio::chain::plugin_interface::next_function<push_results> callback = detail::create_push_action_callback(
      {}, state.completion, uw::synd::account, uw::synd::action_crank);
   BOOST_CHECK(done.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
   BOOST_CHECK(!state.due({}, {}, initial_epoch, start + fc::seconds(15), idle_interval));
   callback(push_result{trace_result(true)});
   BOOST_CHECK(done.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
   BOOST_CHECK(state.due({}, {}, initial_epoch, start + fc::seconds(30), idle_interval));
}

BOOST_AUTO_TEST_SUITE_END()
