#include "../src/group_election.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>

#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>

using namespace sysio::batch_operator_detail;

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

   BOOST_CHECK_EQUAL(not_elected_message(epoch_index, election),
                     std::string("batch_operator: not elected for epoch 1 (my_group=1, active_group=0)"));
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
   BOOST_CHECK_EQUAL(not_elected_message(epoch_index, election),
                     std::string("batch_operator: not elected for epoch 1 (my_group=1, active_group=2)"));
} FC_LOG_AND_RETHROW();

/// An operator seated in no group keeps the sentinel and is never elected.
BOOST_AUTO_TEST_CASE(unseated_operator_keeps_the_group_sentinel) try {
   const auto election =
      evaluate_group_election(on_duty_group, sliding_window(), sysio::chain::name("batchop.z"));

   BOOST_CHECK_EQUAL(static_cast<unsigned>(election.my_group), static_cast<unsigned>(GROUP_NONE));
   BOOST_CHECK(!election.is_elected);
   BOOST_CHECK_EQUAL(not_elected_message(epoch_index, election),
                     std::string("batch_operator: not elected for epoch 1 (my_group=255, active_group=0)"));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
