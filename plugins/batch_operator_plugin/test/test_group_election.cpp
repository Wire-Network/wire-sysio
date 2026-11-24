#include "../src/chain_epochs.hpp"
#include "../src/group_election.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <fc/exception/exception.hpp>
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

fc::time_point at_sec(int64_t unix_sec) { return fc::time_point(fc::seconds(unix_sec)); }

/// An `epochstate` value as the chain renders it.
fc::variant_object state_row(fc::slug_name chain, uint32_t epoch, int64_t start_sec, uint32_t duration_sec,
                             uint8_t on_duty, bool paused = false) {
   return mvo()(epoch::field::chain_code, chain.value)
               (epoch::field::current_epoch_index, epoch)
               (epoch::field::current_epoch_start, at_sec(start_sec))
               (epoch::field::next_epoch_start, at_sec(start_sec + duration_sec))
               (epoch::field::current_batch_op_group, on_duty)
               (epoch::field::batch_op_groups, sliding_window())
               (epoch::field::is_paused, paused);
}

/// An `epochcfg` value as the chain renders it.
fc::variant_object config_row(fc::slug_name chain, uint32_t duration_sec) {
   return mvo()(epoch::field::chain_code, chain.value)(epoch::field::epoch_duration_sec, duration_sec);
}

/// Both chains' rows: `self` sits in group 1, on duty on the fast chain only.
decoded_chain_epochs decode_both() {
   return decode_chain_epochs(
      fc::variants{fc::variant(state_row(fast_chain, fast_epoch, fast_start_sec, fast_duration_sec, self_group)),
                   fc::variant(state_row(slow_chain, slow_epoch, slow_start_sec, slow_duration_sec, on_duty_group))},
      fc::variants{fc::variant(config_row(fast_chain, fast_duration_sec)),
                   fc::variant(config_row(slow_chain, slow_duration_sec))},
      sysio::chain::name(self), max_poll);
}

} // namespace

BOOST_AUTO_TEST_SUITE(batch_operator_chain_epoch_tests)

/// Each chain's view carries its own epoch index, timing, duration and election, read from its own row.
BOOST_AUTO_TEST_CASE(each_chain_reads_its_own_epoch_and_election) try {
   const decoded_chain_epochs decoded = decode_both();
   BOOST_CHECK(decoded.skipped.empty());
   BOOST_REQUIRE_EQUAL(decoded.views.size(), 2u);

   const chain_epoch_view& fast = decoded.views.at(fast_chain.value);
   BOOST_CHECK_EQUAL(fast.chain_code, fast_chain.value);
   BOOST_CHECK_EQUAL(fast.epoch_index, fast_epoch);
   BOOST_CHECK(fast.epoch_start == at_sec(fast_start_sec));
   BOOST_CHECK(fast.next_epoch_start == at_sec(fast_start_sec + fast_duration_sec));
   BOOST_CHECK_EQUAL(fast.epoch_duration.count(), fc::seconds(fast_duration_sec).count());
   BOOST_CHECK(fast.election.is_elected);
   BOOST_CHECK_EQUAL(static_cast<unsigned>(fast.election.my_group), static_cast<unsigned>(self_group));

   const chain_epoch_view& slow = decoded.views.at(slow_chain.value);
   BOOST_CHECK_EQUAL(slow.epoch_index, slow_epoch);
   BOOST_CHECK(slow.epoch_start == at_sec(slow_start_sec));
   BOOST_CHECK(slow.next_epoch_start == at_sec(slow_start_sec + slow_duration_sec));
   BOOST_CHECK_EQUAL(slow.epoch_duration.count(), fc::seconds(slow_duration_sec).count());
   BOOST_CHECK(!slow.election.is_elected);
   BOOST_CHECK_EQUAL(static_cast<unsigned>(slow.election.my_group), static_cast<unsigned>(self_group));
   BOOST_CHECK_EQUAL(static_cast<unsigned>(slow.election.current_group), static_cast<unsigned>(on_duty_group));

   // Each chain's cadence follows its own duration.
   BOOST_CHECK_EQUAL(fast.cadence.poll_interval.count(), min_poll_interval.count());
   BOOST_CHECK_EQUAL(slow.cadence.poll_interval.count(), max_poll.count());
} FC_LOG_AND_RETHROW();

/// A paused chain elects nobody, though the operator's seat in its window still stands.
BOOST_AUTO_TEST_CASE(a_paused_chain_elects_nobody_but_keeps_the_seat) try {
   constexpr bool paused = true;
   const chain_epoch_view view =
      decode_chain_epoch(state_row(fast_chain, fast_epoch, fast_start_sec, fast_duration_sec, self_group, paused),
                         config_row(fast_chain, fast_duration_sec), sysio::chain::name(self), max_poll);
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
      fc::variants{fc::variant(state_row(unconfigured_chain, fast_epoch, fast_start_sec, fast_duration_sec,
                                         self_group)),
                   fc::variant(broken),
                   fc::variant(state_row(slow_chain, slow_epoch, slow_start_sec, slow_duration_sec, self_group))},
      fc::variants{fc::variant(config_row(fast_chain, fast_duration_sec)),
                   fc::variant(config_row(slow_chain, slow_duration_sec))},
      sysio::chain::name(self), max_poll);

   BOOST_REQUIRE_EQUAL(decoded.views.size(), 1u);
   BOOST_CHECK(decoded.views.contains(slow_chain.value));
   BOOST_CHECK(decoded.views.at(slow_chain.value).election.is_elected);
   BOOST_REQUIRE_EQUAL(decoded.skipped.size(), 2u);
   BOOST_CHECK(decoded.skipped[0].find("BASE") != std::string::npos);
   BOOST_CHECK(decoded.skipped[1].find("did not decode") != std::string::npos);
} FC_LOG_AND_RETHROW();

/// The poll is 1/24 of the epoch and the edge buffer 1/144, so the default 360 s epoch keeps today's 15 s poll and
/// 2.5 s buffer; the poll is floored at a second and capped at `--batch-epoch-poll-ms`, the buffer floored at a block.
BOOST_AUTO_TEST_CASE(the_cadence_follows_the_epoch_duration) try {
   struct expectation {
      uint32_t         duration_sec;
      fc::microseconds max_poll_interval;
      fc::microseconds poll_interval;
      fc::microseconds edge_buffer;
   };
   const std::vector<expectation> expectations{
      {slow_duration_sec, max_poll, fc::seconds(15), fc::milliseconds(2500)},
      {fast_duration_sec, max_poll, min_poll_interval, min_edge_buffer},
      {60, max_poll, fc::milliseconds(2500), min_edge_buffer},
      {120, max_poll, fc::seconds(5), fc::microseconds(833'333)},
      {3600, max_poll, max_poll, fc::seconds(25)},
      // A poll option below the floor is honoured.
      {slow_duration_sec, fc::milliseconds(500), fc::milliseconds(500), fc::milliseconds(2500)},
   };
   for (const expectation& e : expectations) {
      const epoch_cadence cadence = derive_cadence(fc::seconds(e.duration_sec), e.max_poll_interval);
      BOOST_TEST_CONTEXT("duration " << e.duration_sec << " s, max poll " << e.max_poll_interval.count() << " us") {
         BOOST_CHECK_EQUAL(cadence.poll_interval.count(), e.poll_interval.count());
         BOOST_CHECK_EQUAL(cadence.edge_buffer.count(), e.edge_buffer.count());
      }
   }
   BOOST_CHECK_EQUAL(min_edge_buffer.count(), fc::milliseconds(sysio::chain::config::block_interval_ms).count());
} FC_LOG_AND_RETHROW();

/// The safe window and the boundary are each chain's own: the same instant can be inside the fast chain's window
/// and still inside the slow chain's start buffer.
BOOST_AUTO_TEST_CASE(the_epoch_window_uses_the_chains_own_buffer) try {
   const decoded_chain_epochs decoded = decode_both();
   chain_epoch_view fast = decoded.views.at(fast_chain.value);
   chain_epoch_view slow = decoded.views.at(slow_chain.value);
   // Line both epochs up on the same start so only their buffers differ.
   slow.epoch_start      = fast.epoch_start;
   slow.next_epoch_start = fast.epoch_start + slow.epoch_duration;
   const fc::time_point start = fast.epoch_start;

   const fc::time_point inside_fast_buffer = start + fc::milliseconds(400);
   const fc::time_point past_fast_buffer   = start + fc::milliseconds(600);
   BOOST_CHECK(!within_epoch_window(fast, inside_fast_buffer));
   BOOST_CHECK(within_epoch_window(fast, past_fast_buffer));
   BOOST_CHECK(!within_epoch_window(slow, past_fast_buffer));
   BOOST_CHECK(within_epoch_window(slow, start + fc::seconds(3)));

   // The buffer before the fast chain's next epoch, then the overdue epoch.
   BOOST_CHECK(!within_epoch_window(fast, fast.next_epoch_start - fc::milliseconds(400)));
   BOOST_CHECK(!is_epoch_boundary_past(fast, fast.next_epoch_start - fc::milliseconds(1)));
   BOOST_CHECK(within_epoch_window(fast, fast.next_epoch_start));
   BOOST_CHECK(is_epoch_boundary_past(fast, fast.next_epoch_start));
   BOOST_CHECK(!is_epoch_boundary_past(slow, fast.next_epoch_start));

   // A chain without a next epoch start has no boundary to pass.
   chain_epoch_view genesis = fast;
   genesis.next_epoch_start = fc::time_point();
   BOOST_CHECK(!is_epoch_boundary_past(genesis, past_fast_buffer));
   BOOST_CHECK(within_epoch_window(genesis, past_fast_buffer));
} FC_LOG_AND_RETHROW();

/// `chkcons` goes to each relayed chain the operator is elected on, and only once that chain's boundary has passed.
BOOST_AUTO_TEST_CASE(chkcons_goes_only_to_relayed_elected_chains_past_their_boundary) try {
   chain_epochs views = decode_both().views;
   // Elected on a chain it does not relay.
   chain_epoch_view unrelayed = views.at(fast_chain.value);
   unrelayed.chain_code = unconfigured_chain.value;
   views.emplace(unconfigured_chain.value, unrelayed);

   const std::set<uint64_t> relayed{fast_chain.value, slow_chain.value};
   const fc::time_point     boundary = views.at(fast_chain.value).next_epoch_start;
   BOOST_CHECK(chkcons_chains(views, relayed, boundary) == std::vector<uint64_t>{fast_chain.value});
   BOOST_CHECK(chkcons_chains(views, relayed, boundary - fc::milliseconds(1)).empty());
   BOOST_CHECK(chkcons_chains(views, {slow_chain.value}, boundary).empty());
   BOOST_CHECK(chkcons_chains({}, relayed, boundary).empty());

   // A chain without a next epoch start has no boundary to pass.
   views.at(fast_chain.value).next_epoch_start = fc::time_point();
   BOOST_CHECK(chkcons_chains(views, relayed, boundary).empty());
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
