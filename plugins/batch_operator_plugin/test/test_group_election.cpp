#include "../src/chain_epochs.hpp"
#include "../src/group_election.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/slug_name.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

using namespace sysio::batch_operator_detail;
using fc::operator""_s;
using mvo = fc::mutable_variant_object;

namespace {

/// A three-group sliding window; `self` sits in group 1.
constexpr auto self         = "batchop.b";
constexpr auto on_duty_peer = "batchop.a";
constexpr auto group_peer   = "batchop.c";
constexpr auto stranger     = "batchop.d";

constexpr uint8_t on_duty_group = 0;
constexpr uint8_t self_group    = 1;
constexpr uint8_t idle_group    = 2;

/// The epoch whose modulo-3 rotation index (1) differs from the group actually
/// on duty (0) — the exact reading that made the old diagnostic self-contradict.
constexpr uint32_t epoch_index = 1;

constexpr std::size_t on_duty_group_size = 2;

/// The chain the single-chain diagnostics report on.
constexpr fc::slug_name reported_chain = "ETHEREUM"_s;

fc::variants group(std::initializer_list<const char*> members) {
   fc::variants accounts;
   for (const auto* member : members) {
      accounts.emplace_back(std::string(member));
   }
   return accounts;
}

/// `batch_op_groups` as the epochstate carries it: group 0 on duty, `self` in 1.
fc::variants sliding_window() {
   return fc::variants{fc::variant(group({on_duty_peer, stranger})),
                       fc::variant(group({self, group_peer})),
                       fc::variant(group({stranger, group_peer}))};
}

} // namespace

BOOST_AUTO_TEST_SUITE(batch_operator_group_election_tests)

/// An operator seated in a group that is not on duty is not elected, and the
/// diagnostic names the group the decision actually consulted.
///
/// Regression guard: the message once printed `epoch_index % 3`, the
/// static-rotation index the sliding window replaced. At epoch 1 that yields 1
/// — equal to `my_group` — so the line read "not elected (my_group=1,
/// active_group=1)". Any return to a derived active group fails here.
BOOST_AUTO_TEST_CASE(not_elected_reports_the_group_on_duty) try {
   const auto election = evaluate_group_election(on_duty_group, sliding_window(), sysio::chain::name(self));

   BOOST_CHECK_EQUAL(static_cast<unsigned>(election.my_group), static_cast<unsigned>(self_group));
   BOOST_CHECK_EQUAL(static_cast<unsigned>(election.current_group), static_cast<unsigned>(on_duty_group));
   BOOST_CHECK(!election.is_elected);

   BOOST_CHECK_EQUAL(not_elected_message(reported_chain.value, epoch_index, election),
                     std::string("batch_operator: chain ETHEREUM not elected for epoch 1 (my_group=1, "
                                 "active_group=0)"));
} FC_LOG_AND_RETHROW();

/// The on-duty group's roster is captured for the operator seated in it.
BOOST_AUTO_TEST_CASE(elected_captures_the_on_duty_roster) try {
   const auto election =
      evaluate_group_election(on_duty_group, sliding_window(), sysio::chain::name(on_duty_peer));

   BOOST_CHECK_EQUAL(static_cast<unsigned>(election.my_group), static_cast<unsigned>(on_duty_group));
   BOOST_CHECK_EQUAL(static_cast<unsigned>(election.current_group), static_cast<unsigned>(on_duty_group));
   BOOST_CHECK(election.is_elected);
   BOOST_CHECK_EQUAL(election.current_group_members.size(), on_duty_group_size);
   BOOST_CHECK_EQUAL(election.current_group_members[0].to_string(), std::string(on_duty_peer));
} FC_LOG_AND_RETHROW();

/// The on-duty roster is read from the group on duty, not from the epoch index.
/// With group 2 on duty at epoch 1, a modulo-3 reading would collect group 1.
BOOST_AUTO_TEST_CASE(on_duty_roster_follows_the_window_not_the_epoch) try {
   const auto election = evaluate_group_election(idle_group, sliding_window(), sysio::chain::name(self));

   BOOST_CHECK_EQUAL(static_cast<unsigned>(election.current_group), static_cast<unsigned>(idle_group));
   BOOST_CHECK(!election.is_elected);
   BOOST_CHECK_EQUAL(election.current_group_members[0].to_string(), std::string(stranger));
   BOOST_CHECK_EQUAL(not_elected_message(reported_chain.value, epoch_index, election),
                     std::string("batch_operator: chain ETHEREUM not elected for epoch 1 (my_group=1, "
                                 "active_group=2)"));
} FC_LOG_AND_RETHROW();

/// An operator seated in no group keeps the sentinel and is never elected.
BOOST_AUTO_TEST_CASE(unseated_operator_keeps_the_group_sentinel) try {
   const auto election =
      evaluate_group_election(on_duty_group, sliding_window(), sysio::chain::name("batchop.z"));

   BOOST_CHECK_EQUAL(static_cast<unsigned>(election.my_group), static_cast<unsigned>(GROUP_NONE));
   BOOST_CHECK(!election.is_elected);
   BOOST_CHECK_EQUAL(not_elected_message(reported_chain.value, epoch_index, election),
                     std::string("batch_operator: chain ETHEREUM not elected for epoch 1 (my_group=255, "
                                 "active_group=0)"));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()

// ─── Per-chain consensus epochs ─────────────────────────────────────────────

namespace {

/// Two outpost chains on their own consensus epochs: a fast one with 10 s epochs, far into its count, and a slow
/// one with 360 s epochs (the default), early in its count. `self` is on duty on the fast chain only.
constexpr fc::slug_name fast_chain = "SOLANA"_s;
constexpr fc::slug_name slow_chain = "ETHEREUM"_s;
/// A chain with epoch state but no `epochcfg` row.
constexpr fc::slug_name unconfigured_chain = "BASE"_s;
constexpr uint32_t fast_duration_sec = 10;
constexpr uint32_t slow_duration_sec = 360;
constexpr uint32_t fast_epoch        = 42;
constexpr uint32_t slow_epoch        = 3;
/// When each chain's current epoch started, in unix seconds.
constexpr int64_t fast_start_sec = 1'800'000'400;
constexpr int64_t slow_start_sec = 1'800'000'000;
/// `--batch-epoch-poll-ms` at its default.
constexpr fc::microseconds max_poll = fc::seconds(15);
/// How long after the fast chain's epoch started its traffic began.
constexpr int64_t fast_traffic_after_sec = 3;

fc::time_point at_sec(int64_t unix_sec) { return fc::time_point(fc::seconds(unix_sec)); }

/// A chain's pending traffic, as its `epochstate` carries it: zero while it has none.
struct traffic {
   fc::time_point since{};
   fc::time_point due_at{};
};

/// An `epochstate` value as the chain renders it; `previous` is the group that served the previous epoch.
fc::variant_object state_row(fc::slug_name chain, uint32_t epoch, int64_t start_sec, uint8_t on_duty,
                             const traffic& pending = traffic(), bool paused = false,
                             std::initializer_list<const char*> previous = {}) {
   return mvo()(epoch::field::chain_code, chain.value)
               (epoch::field::current_epoch_index, epoch)
               (epoch::field::current_epoch_start, at_sec(start_sec))
               (epoch::field::traffic_since, pending.since)
               (epoch::field::due_at, pending.due_at)
               (epoch::field::current_batch_op_group, on_duty)
               (epoch::field::batch_op_groups, sliding_window())
               (epoch::field::is_paused, paused)
               (epoch::field::previous_group, group(previous));
}

/// Parts of a chain's minimum that make its late-delivery window in these rows: half of it, the contract's default.
constexpr uint32_t late_window_divisor = 2;

/// An `epochcfg` value as the chain renders it, its late-delivery window half its minimum.
fc::variant_object config_row(fc::slug_name chain, uint32_t duration_sec) {
   return mvo()(epoch::field::chain_code, chain.value)(epoch::field::epoch_duration_sec, duration_sec)(
      epoch::field::late_window_sec, duration_sec / late_window_divisor);
}

/// A `sysio.msgch::outpcons` value as the chain renders it: `chain`'s accepted winner is for `epoch`.
fc::variant_object consensus_row(fc::slug_name chain, uint32_t epoch) {
   return mvo()(consensus::field::chain_code, chain.value)(consensus::field::epoch_index, epoch);
}

/// The fast chain's pending traffic: it began `fast_traffic_after_sec` into the epoch, so it is due one minimum after
/// the epoch start, its traffic wait (half the minimum) ending sooner.
traffic fast_traffic() {
   const fc::time_point since = at_sec(fast_start_sec + fast_traffic_after_sec);
   return {.since = since, .due_at = at_sec(fast_start_sec) + fc::seconds(fast_duration_sec)};
}

/// Both chains' rows: `self` sits in group 1, on duty on the fast chain only. The fast chain's epoch is decided and
/// has traffic; the slow chain's is undecided, its row still holding the previous epoch's winner, and idle.
decoded_chain_epochs decode_both() {
   return decode_chain_epochs(
      fc::variants{fc::variant(state_row(fast_chain, fast_epoch, fast_start_sec, self_group, fast_traffic())),
                   fc::variant(state_row(slow_chain, slow_epoch, slow_start_sec, on_duty_group))},
      fc::variants{fc::variant(config_row(fast_chain, fast_duration_sec)),
                   fc::variant(config_row(slow_chain, slow_duration_sec))},
      fc::variants{fc::variant(consensus_row(fast_chain, fast_epoch)),
                   fc::variant(consensus_row(slow_chain, slow_epoch - 1))},
      sysio::chain::name(self), max_poll);
}

} // namespace

BOOST_AUTO_TEST_SUITE(batch_operator_chain_epoch_tests)

/// Each chain's view carries its own epoch index, timing, traffic, decision, duration and election, read from its own
/// rows.
BOOST_AUTO_TEST_CASE(each_chain_reads_its_own_epoch_and_election) try {
   const decoded_chain_epochs decoded = decode_both();
   BOOST_CHECK(decoded.skipped.empty());
   BOOST_REQUIRE_EQUAL(decoded.views.size(), 2u);

   const chain_epoch_view& fast = decoded.views.at(fast_chain.value);
   BOOST_CHECK_EQUAL(fast.chain_code, fast_chain.value);
   BOOST_CHECK_EQUAL(fast.epoch_index, fast_epoch);
   BOOST_CHECK(fast.epoch_start == at_sec(fast_start_sec));
   BOOST_CHECK(fast.traffic_since == fast_traffic().since);
   BOOST_CHECK(fast.due_at == fast_traffic().due_at);
   BOOST_CHECK(fast.decided);
   BOOST_CHECK(!fast.in_previous_group);
   BOOST_CHECK_EQUAL(fast.epoch_duration.count(), fc::seconds(fast_duration_sec).count());
   BOOST_CHECK_EQUAL(fast.late_window.count(), fc::seconds(fast_duration_sec / late_window_divisor).count());
   BOOST_CHECK(fast.election.is_elected);
   BOOST_CHECK_EQUAL(static_cast<unsigned>(fast.election.my_group), static_cast<unsigned>(self_group));

   const chain_epoch_view& slow = decoded.views.at(slow_chain.value);
   BOOST_CHECK_EQUAL(slow.epoch_index, slow_epoch);
   BOOST_CHECK(slow.epoch_start == at_sec(slow_start_sec));
   BOOST_CHECK(slow.traffic_since == fc::time_point());
   BOOST_CHECK(slow.due_at == fc::time_point());
   BOOST_CHECK(!slow.decided);
   BOOST_CHECK_EQUAL(slow.epoch_duration.count(), fc::seconds(slow_duration_sec).count());
   BOOST_CHECK(!slow.election.is_elected);
   BOOST_CHECK_EQUAL(static_cast<unsigned>(slow.election.my_group), static_cast<unsigned>(self_group));
   BOOST_CHECK_EQUAL(static_cast<unsigned>(slow.election.current_group), static_cast<unsigned>(on_duty_group));

   // Each chain's cadence follows its own duration.
   BOOST_CHECK_EQUAL(fast.cadence.poll_interval.count(), min_poll_interval.count());
   BOOST_CHECK_EQUAL(slow.cadence.poll_interval.count(), max_poll.count());
   BOOST_CHECK_EQUAL(fast.cadence.delivery_timeout.count(), min_delivery_timeout.count());
   BOOST_CHECK_EQUAL(slow.cadence.delivery_timeout.count(), max_delivery_timeout.count());
} FC_LOG_AND_RETHROW();

/// The operator's seat in the group that served the previous epoch is read from `previous_group`, whatever its
/// standing in the current window; a row without the field does not decode.
BOOST_AUTO_TEST_CASE(previous_group_membership_is_read_from_its_own_field) try {
   const auto view_with_previous = [](std::initializer_list<const char*> previous) {
      return decode_chain_epoch(state_row(slow_chain, slow_epoch, slow_start_sec, on_duty_group, traffic(), false,
                                          previous),
                                config_row(slow_chain, slow_duration_sec), decided_epochs{},
                                sysio::chain::name(self), max_poll);
   };
   const chain_epoch_view sat = view_with_previous({on_duty_peer, self});
   BOOST_CHECK(sat.in_previous_group);
   BOOST_CHECK(!sat.election.is_elected);
   BOOST_CHECK(!view_with_previous({on_duty_peer, stranger}).in_previous_group);
   BOOST_CHECK(!view_with_previous({}).in_previous_group);

   fc::mutable_variant_object without(state_row(slow_chain, slow_epoch, slow_start_sec, on_duty_group));
   without.erase(epoch::field::previous_group);
   BOOST_CHECK_THROW(decode_chain_epoch(without, config_row(slow_chain, slow_duration_sec), decided_epochs{},
                                        sysio::chain::name(self), max_poll),
                     fc::exception);
} FC_LOG_AND_RETHROW();

/// A member of the previous epoch's group may deliver it late only while the chain's late-delivery window, from the
/// current epoch's start, is open: never after it, never from outside the group, and never in epoch 1, which has no
/// previous epoch.
BOOST_AUTO_TEST_CASE(a_late_delivery_is_open_only_within_the_window) try {
   chain_epoch_view view = decode_chain_epoch(
      state_row(slow_chain, slow_epoch, slow_start_sec, on_duty_group, traffic(), false, {on_duty_peer, self}),
      config_row(slow_chain, slow_duration_sec), decided_epochs{}, sysio::chain::name(self), max_poll);
   const fc::time_point closes = view.epoch_start + view.late_window;
   BOOST_CHECK(late_delivery_open(view, view.epoch_start));
   BOOST_CHECK(late_delivery_open(view, closes - fc::milliseconds(1)));
   BOOST_CHECK(!late_delivery_open(view, closes));

   chain_epoch_view outsider = view;
   outsider.in_previous_group = false;
   BOOST_CHECK(!late_delivery_open(outsider, view.epoch_start));

   chain_epoch_view first_epoch = view;
   first_epoch.epoch_index = 1;
   BOOST_CHECK(!late_delivery_open(first_epoch, view.epoch_start));

   fc::mutable_variant_object no_window(config_row(slow_chain, slow_duration_sec));
   no_window.erase(epoch::field::late_window_sec);
   BOOST_CHECK_THROW(decode_chain_epoch(state_row(slow_chain, slow_epoch, slow_start_sec, on_duty_group), no_window,
                                        decided_epochs{}, sysio::chain::name(self), max_poll),
                     fc::exception);
} FC_LOG_AND_RETHROW();

/// A paused chain elects nobody, though the operator's seat in its window still stands.
BOOST_AUTO_TEST_CASE(a_paused_chain_elects_nobody_but_keeps_the_seat) try {
   constexpr bool paused = true;
   const chain_epoch_view view =
      decode_chain_epoch(state_row(fast_chain, fast_epoch, fast_start_sec, self_group, fast_traffic(), paused),
                         config_row(fast_chain, fast_duration_sec), decided_epochs{{fast_chain.value, fast_epoch}},
                         sysio::chain::name(self), max_poll);
   BOOST_CHECK(view.is_paused);
   BOOST_CHECK(!view.election.is_elected);
   BOOST_CHECK_EQUAL(static_cast<unsigned>(view.election.my_group), static_cast<unsigned>(self_group));
   BOOST_CHECK_EQUAL(view.epoch_index, fast_epoch);
} FC_LOG_AND_RETHROW();

/// A chain with epoch state but no config, or a row that does not decode, is left out with a reason; the other
/// chains still decode.
BOOST_AUTO_TEST_CASE(a_chain_without_config_or_a_bad_row_is_left_out) try {
   const fc::variant_object broken = mvo()(epoch::field::chain_code, fast_chain.value);
   const decoded_chain_epochs decoded = decode_chain_epochs(
      fc::variants{fc::variant(state_row(unconfigured_chain, fast_epoch, fast_start_sec, self_group)),
                   fc::variant(broken),
                   fc::variant(state_row(slow_chain, slow_epoch, slow_start_sec, self_group))},
      fc::variants{fc::variant(config_row(fast_chain, fast_duration_sec)),
                   fc::variant(config_row(slow_chain, slow_duration_sec))},
      fc::variants{}, sysio::chain::name(self), max_poll);

   BOOST_REQUIRE_EQUAL(decoded.views.size(), 1u);
   BOOST_CHECK(decoded.views.contains(slow_chain.value));
   BOOST_CHECK(decoded.views.at(slow_chain.value).election.is_elected);
   BOOST_REQUIRE_EQUAL(decoded.skipped.size(), 2u);
   BOOST_CHECK(decoded.skipped[0].find("BASE") != std::string::npos);
   BOOST_CHECK(decoded.skipped[1].find("did not decode") != std::string::npos);
} FC_LOG_AND_RETHROW();

/// The poll is 1/24 of the chain minimum and the delivery timeout 1/2, so the default 360 s minimum keeps a 15 s poll
/// and a 60 s timeout; the poll is floored at a second and capped at `--batch-epoch-poll-ms`, and the timeout held
/// between 5 s and 60 s.
BOOST_AUTO_TEST_CASE(the_cadence_follows_the_epoch_duration) try {
   struct expectation {
      uint32_t         duration_sec;
      fc::microseconds max_poll_interval;
      fc::microseconds poll_interval;
      fc::microseconds delivery_timeout;
   };
   const std::vector<expectation> expectations{
      {slow_duration_sec, max_poll, fc::seconds(15), max_delivery_timeout},
      {fast_duration_sec, max_poll, min_poll_interval, min_delivery_timeout},
      {15, max_poll, min_poll_interval, fc::milliseconds(7500)},
      {60, max_poll, fc::milliseconds(2500), fc::seconds(30)},
      {120, max_poll, fc::seconds(5), max_delivery_timeout},
      {3600, max_poll, max_poll, max_delivery_timeout},
      // A poll option below the floor is honoured.
      {slow_duration_sec, fc::milliseconds(500), fc::milliseconds(500), max_delivery_timeout},
      // A duration under sysio.epoch's 10 s floor still gets the shortest timeout.
      {1, max_poll, min_poll_interval, min_delivery_timeout},
   };
   for (const expectation& e : expectations) {
      const epoch_cadence cadence = derive_cadence(fc::seconds(e.duration_sec), e.max_poll_interval);
      BOOST_TEST_CONTEXT("duration " << e.duration_sec << " s, max poll " << e.max_poll_interval.count() << " us") {
         BOOST_CHECK_EQUAL(cadence.poll_interval.count(), e.poll_interval.count());
         BOOST_CHECK_EQUAL(cadence.delivery_timeout.count(), e.delivery_timeout.count());
      }
   }
} FC_LOG_AND_RETHROW();

/// A chain's outpost calls and depot pushes take its own delivery timeout, and the longest one while it has no view.
BOOST_AUTO_TEST_CASE(each_chain_takes_its_own_delivery_timeout) try {
   const chain_epochs views = decode_both().views;
   BOOST_CHECK_EQUAL(delivery_timeout_of(views, fast_chain.value).count(), min_delivery_timeout.count());
   BOOST_CHECK_EQUAL(delivery_timeout_of(views, slow_chain.value).count(), max_delivery_timeout.count());
   BOOST_CHECK_EQUAL(delivery_timeout_of(views, unconfigured_chain.value).count(), max_delivery_timeout.count());
   BOOST_CHECK_EQUAL(delivery_timeout_of({}, fast_chain.value).count(), max_delivery_timeout.count());
} FC_LOG_AND_RETHROW();

/// A chain whose `outpcons` row does not decode reads as undecided, with the reason; a row for another epoch, or no
/// row, leaves the epoch undecided too.
BOOST_AUTO_TEST_CASE(a_bad_consensus_row_reads_as_undecided) try {
   const fc::variant_object broken = mvo()(consensus::field::chain_code, fast_chain.value);
   const decoded_chain_epochs decoded = decode_chain_epochs(
      fc::variants{fc::variant(state_row(fast_chain, fast_epoch, fast_start_sec, self_group)),
                   fc::variant(state_row(slow_chain, slow_epoch, slow_start_sec, self_group))},
      fc::variants{fc::variant(config_row(fast_chain, fast_duration_sec)),
                   fc::variant(config_row(slow_chain, slow_duration_sec))},
      fc::variants{fc::variant(broken)}, sysio::chain::name(self), max_poll);

   BOOST_REQUIRE_EQUAL(decoded.views.size(), 2u);
   BOOST_CHECK(!decoded.views.at(fast_chain.value).decided);
   BOOST_CHECK(!decoded.views.at(slow_chain.value).decided);
   BOOST_REQUIRE_EQUAL(decoded.skipped.size(), 1u);
   BOOST_CHECK(decoded.skipped[0].find("outpcons") != std::string::npos);

   // Before its genesis advance a chain has no epoch to decide, whatever an `outpcons` row says.
   const chain_epoch_view genesis =
      decode_chain_epoch(state_row(fast_chain, 0, 0, self_group), config_row(fast_chain, fast_duration_sec),
                         decided_epochs{{fast_chain.value, 0}}, sysio::chain::name(self), max_poll);
   BOOST_CHECK(!genesis.decided);
} FC_LOG_AND_RETHROW();

/// A chain is due once its `due_at` has passed, and never while it is idle.
BOOST_AUTO_TEST_CASE(a_chain_is_due_only_with_traffic) try {
   const chain_epochs views = decode_both().views;
   const chain_epoch_view& fast = views.at(fast_chain.value);
   BOOST_CHECK(!is_advance_due(fast, fast.due_at - fc::milliseconds(1)));
   BOOST_CHECK(is_advance_due(fast, fast.due_at));
   const chain_epoch_view& idle = views.at(slow_chain.value);
   BOOST_CHECK(!is_advance_due(idle, idle.epoch_start + fc::days(30)));
} FC_LOG_AND_RETHROW();

/// The relay pushes `chkcons` for a chain right after its own delivery of the current epoch once the chain is due:
/// the delivery may have decided it. Never while the chain is idle, paused, or before its genesis.
BOOST_AUTO_TEST_CASE(chkcons_follows_an_own_delivery_once_the_chain_is_due) try {
   chain_epoch_view fast = decode_both().views.at(fast_chain.value);
   BOOST_CHECK(pushes_chkcons_after_delivery(fast, fast.due_at));
   BOOST_CHECK(!pushes_chkcons_after_delivery(fast, fast.due_at - fc::milliseconds(1)));
   // An undecided epoch too: the last-read view cannot show what the delivery just decided.
   fast.decided = false;
   BOOST_CHECK(pushes_chkcons_after_delivery(fast, fast.due_at));
   // Not elected on the epoch that follows its own (late) delivery makes no difference either.
   fast.election.is_elected = false;
   BOOST_CHECK(pushes_chkcons_after_delivery(fast, fast.due_at));

   chain_epoch_view paused = fast;
   paused.is_paused = true;
   BOOST_CHECK(!pushes_chkcons_after_delivery(paused, fast.due_at));
   chain_epoch_view genesis = fast;
   genesis.epoch_index = 0;
   BOOST_CHECK(!pushes_chkcons_after_delivery(genesis, fast.due_at));
   const chain_epoch_view idle = decode_both().views.at(slow_chain.value);
   BOOST_CHECK(!pushes_chkcons_after_delivery(idle, idle.epoch_start + fc::days(30)));
} FC_LOG_AND_RETHROW();

/// The elected group pushes `chkcons` for a decided epoch once its chain is due, which advances it; never for a decided
/// epoch on an idle chain.
BOOST_AUTO_TEST_CASE(chkcons_goes_to_a_due_decided_epoch_from_the_elected_group) try {
   chain_epochs views = decode_both().views;
   // Elected on a chain it does not relay.
   chain_epoch_view unrelayed = views.at(fast_chain.value);
   unrelayed.chain_code = unconfigured_chain.value;
   views.emplace(unconfigured_chain.value, unrelayed);

   const std::set<uint64_t> relayed{fast_chain.value, slow_chain.value};
   chain_epoch_view&        fast = views.at(fast_chain.value);
   const fc::time_point     due  = fast.due_at;
   const std::vector<chkcons_push> pushed_due{{.chain_code = fast_chain.value, .reason = chkcons_reason::due}};

   BOOST_CHECK(chkcons_chains(views, relayed, due, {}, true) == pushed_due);
   BOOST_CHECK(chkcons_chains(views, relayed, due + fc::days(1), {}, false) == pushed_due);
   BOOST_CHECK(chkcons_chains(views, relayed, due - fc::milliseconds(1), {}, true).empty());
   BOOST_CHECK(chkcons_chains(views, {slow_chain.value}, due, {}, true).empty());
   BOOST_CHECK(chkcons_chains({}, relayed, due, {}, true).empty());
   // A re-tally pushed earlier does not hold back an advance.
   BOOST_CHECK(chkcons_chains(views, relayed, due, {{fast_chain.value, due}}, true) == pushed_due);

   // Decided and idle: never pushed.
   fast.traffic_since = fc::time_point();
   fast.due_at        = fc::time_point();
   BOOST_CHECK(chkcons_chains(views, relayed, due + fc::days(1), {}, true).empty());

   // Paused by a dispute, or before its genesis advance: never pushed, whatever the election says.
   fast.due_at              = due;
   fast.is_paused           = true;
   fast.election.is_elected = true;
   BOOST_CHECK(chkcons_chains(views, relayed, due, {}, true).empty());
   fast.is_paused   = false;
   fast.epoch_index = 0;
   BOOST_CHECK(chkcons_chains(views, relayed, due, {}, true).empty());
} FC_LOG_AND_RETHROW();

/// The elected group re-tallies an epoch undecided a chain minimum past its start, with or without traffic, and again
/// at most once per chain minimum after its own last re-tally.
BOOST_AUTO_TEST_CASE(chkcons_retallies_an_undecided_epoch_once_per_minimum) try {
   chain_epochs views = decode_both().views;
   const std::set<uint64_t> relayed{fast_chain.value};
   chain_epoch_view&        fast = views.at(fast_chain.value);
   fast.decided                  = false;
   const fc::time_point first    = fast.epoch_start + fast.epoch_duration;
   const std::vector<chkcons_push> retally{{.chain_code = fast_chain.value, .reason = chkcons_reason::retally}};

   BOOST_CHECK(chkcons_chains(views, relayed, first - fc::milliseconds(1), {}, true).empty());
   BOOST_CHECK(chkcons_chains(views, relayed, first, {}, true) == retally);
   // Once pushed at `first`, not again for a chain minimum.
   const retally_pushes pushed{{fast_chain.value, first}};
   BOOST_CHECK(chkcons_chains(views, relayed, first + fast.epoch_duration - fc::milliseconds(1), pushed, true).empty());
   BOOST_CHECK(chkcons_chains(views, relayed, first + fast.epoch_duration, pushed, true) == retally);
   // A re-tally pushed in an earlier epoch does not hold this one back past its own first chain minimum.
   BOOST_CHECK(chkcons_chains(views, relayed, first, {{fast_chain.value, fast.epoch_start - fc::seconds(1)}}, true) ==
               retally);
   // Idle or not makes no difference.
   fast.traffic_since = fc::time_point();
   fast.due_at        = fc::time_point();
   BOOST_CHECK(chkcons_chains(views, relayed, first, {}, true) == retally);

   // Only the elected group re-tallies, ACTIVE or not.
   fast.election.is_elected = false;
   BOOST_CHECK(chkcons_chains(views, relayed, first + fc::days(1), {}, true).empty());
} FC_LOG_AND_RETHROW();

/// The epoch tick records each re-tally it pushes, which holds this operator's next re-tally of the chain back a chain
/// minimum; an advance or a liveness push records nothing.
BOOST_AUTO_TEST_CASE(the_epoch_tick_records_its_retallies) try {
   chain_epochs views = decode_both().views;
   const std::set<uint64_t> relayed{fast_chain.value};
   chain_epoch_view&        fast = views.at(fast_chain.value);
   fast.decided                  = false;
   const fc::time_point first    = fast.epoch_start + fast.epoch_duration;
   const fc::time_point second   = first + fast.epoch_duration;
   retally_pushes       retallied;
   // One epoch tick as the plugin runs it: name the pushes, then record them.
   const auto tick = [&](fc::time_point now) {
      const std::vector<chkcons_push> pushes = chkcons_chains(views, relayed, now, retallied, true);
      record_retallies(retallied, pushes, now);
      return pushes.size();
   };

   BOOST_CHECK_EQUAL(tick(first), 1u);
   BOOST_CHECK(retallied.at(fast_chain.value) == first);
   BOOST_CHECK_EQUAL(tick(first), 0u);
   BOOST_CHECK_EQUAL(tick(second - fc::milliseconds(1)), 0u);
   BOOST_CHECK_EQUAL(tick(second), 1u);
   BOOST_CHECK(retallied.at(fast_chain.value) == second);

   retally_pushes untouched;
   record_retallies(untouched,
                    {{.chain_code = fast_chain.value, .reason = chkcons_reason::due},
                     {.chain_code = slow_chain.value, .reason = chkcons_reason::liveness}},
                    first);
   BOOST_CHECK(untouched.empty());
} FC_LOG_AND_RETHROW();

/// Once a decided epoch has stayed due past the liveness grace, every ACTIVE operator serving the chain pushes
/// `chkcons` for it, elected or not; an undecided or idle epoch, or an operator that is not ACTIVE, gets none.
BOOST_AUTO_TEST_CASE(chkcons_from_every_serving_operator_once_due_past_the_grace) try {
   chain_epochs views = decode_both().views;
   const std::set<uint64_t> relayed{fast_chain.value, slow_chain.value};
   chain_epoch_view&        fast = views.at(fast_chain.value);
   fast.election.is_elected      = false;
   const fc::time_point due      = fast.due_at;
   const fc::time_point live     = due + liveness_grace(fast);
   BOOST_CHECK_EQUAL(liveness_grace(fast).count(), fast.epoch_duration.count() * liveness_grace_minimums);
   BOOST_CHECK(chain_minimums(fast.epoch_duration, 2) == fast.epoch_duration + fast.epoch_duration);
   const std::vector<chkcons_push> liveness{{.chain_code = fast_chain.value, .reason = chkcons_reason::liveness}};

   BOOST_CHECK(chkcons_chains(views, relayed, due, {}, true).empty());
   BOOST_CHECK(chkcons_chains(views, relayed, live - fc::milliseconds(1), {}, true).empty());
   BOOST_CHECK(chkcons_chains(views, relayed, live, {}, true) == liveness);
   BOOST_CHECK(chkcons_chains(views, relayed, live + fc::days(1), {}, true) == liveness);
   // Not ACTIVE: left to the others.
   BOOST_CHECK(chkcons_chains(views, relayed, live, {}, false).empty());
   // A chain it does not serve.
   BOOST_CHECK(chkcons_chains(views, {slow_chain.value}, live, {}, true).empty());

   // Undecided: the liveness push only advances, and re-tallies stay with the elected group.
   fast.decided = false;
   BOOST_CHECK(chkcons_chains(views, relayed, live, {}, true).empty());
   fast.decided = true;
   // Paused, or idle: none.
   fast.is_paused = true;
   BOOST_CHECK(chkcons_chains(views, relayed, live, {}, true).empty());
   fast.is_paused = false;
   fast.due_at    = fc::time_point();
   BOOST_CHECK(chkcons_chains(views, relayed, live, {}, true).empty());
} FC_LOG_AND_RETHROW();

/// An operator's `qreports` row is looked up by its (chain, operator) key, and counts as reported only for the epoch it
/// was made in.
BOOST_AUTO_TEST_CASE(a_backlog_report_counts_for_the_epoch_it_was_made_in) try {
   constexpr uint32_t report_epoch = 7;
   // Both key fields, by the ABI's key names: the chain code as a number, the account by name.
   const fc::variant_object bound =
      fc::json::from_string(queue_report_find_bound(fast_chain.value, sysio::chain::name(self))).get_object();
   BOOST_CHECK_EQUAL(bound.size(), 2u);
   BOOST_CHECK_EQUAL(bound[backlog::field::chain_code].as_uint64(), fast_chain.value);
   BOOST_CHECK_EQUAL(bound[backlog::field::account].as_string(), std::string(self));
   const auto row = [](fc::slug_name chain, const char* account, uint32_t epoch_index) {
      return fc::variant(mvo()(backlog::field::chain_code, chain.value)(backlog::field::account, account)(
         backlog::field::epoch_index, epoch_index)(backlog::field::pending_count, 2)(
         "oldest_pending_at", at_sec(fast_start_sec)));
   };
   const sysio::chain::name me(self);
   BOOST_CHECK(reported_in_epoch({row(fast_chain, self, report_epoch)}, fast_chain.value, me, report_epoch));
   BOOST_CHECK(!reported_in_epoch({row(fast_chain, self, report_epoch - 1)}, fast_chain.value, me, report_epoch));
   BOOST_CHECK(!reported_in_epoch({row(slow_chain, self, report_epoch)}, fast_chain.value, me, report_epoch));
   BOOST_CHECK(!reported_in_epoch({row(fast_chain, group_peer, report_epoch)}, fast_chain.value, me, report_epoch));
   BOOST_CHECK(!reported_in_epoch({}, fast_chain.value, me, report_epoch));
   BOOST_CHECK_THROW(reported_in_epoch({fc::variant(mvo()(backlog::field::chain_code, fast_chain.value))},
                                       fast_chain.value, me, report_epoch),
                     fc::exception);
} FC_LOG_AND_RETHROW();

/// The epoch tick reads at the shortest relayed chain's cadence, and at the poll option without one; a job polls its
/// own chain's cadence.
BOOST_AUTO_TEST_CASE(the_epoch_tick_reads_at_the_shortest_relayed_cadence) try {
   const chain_epochs views = decode_both().views;
   BOOST_CHECK_EQUAL(epoch_tick_interval(views, {fast_chain.value, slow_chain.value}, max_poll).count(),
                     min_poll_interval.count());
   BOOST_CHECK_EQUAL(epoch_tick_interval(views, {slow_chain.value}, max_poll).count(), max_poll.count());
   BOOST_CHECK_EQUAL(epoch_tick_interval(views, {}, max_poll).count(), max_poll.count());
   BOOST_CHECK_EQUAL(epoch_tick_interval(views, {unconfigured_chain.value}, max_poll).count(), max_poll.count());

   BOOST_CHECK_EQUAL(poll_interval_of(views, fast_chain.value, max_poll).count(), min_poll_interval.count());
   BOOST_CHECK_EQUAL(poll_interval_of(views, slow_chain.value, max_poll).count(), max_poll.count());
   BOOST_CHECK_EQUAL(poll_interval_of(views, unconfigured_chain.value, max_poll).count(), max_poll.count());
} FC_LOG_AND_RETHROW();

/// The tick fires every second (or every poll option, when that is shorter), and a tick that fires a little before
/// the next read's mark still reads then.
BOOST_AUTO_TEST_CASE(the_next_epoch_read_tolerates_tick_jitter) try {
   const fc::microseconds step = epoch_tick_step(max_poll);
   BOOST_CHECK_EQUAL(step.count(), min_poll_interval.count());
   BOOST_CHECK_EQUAL(epoch_tick_step(fc::milliseconds(500)).count(), fc::milliseconds(500).count());

   const fc::time_point read_at = at_sec(fast_start_sec) + fc::milliseconds(7);
   const fc::time_point next    = next_epoch_read_at(read_at, max_poll, step);
   // The tick on the mark one interval later fires a few milliseconds early: it reads.
   BOOST_CHECK(read_at + max_poll - fc::milliseconds(3) >= next);
   // The tick one step earlier does not.
   BOOST_CHECK(read_at + max_poll - step < next);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
