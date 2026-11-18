#include <sysio/net_plugin/handshake_repeat.hpp>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <string>
#include <string_view>

using namespace sysio;

namespace {
using steady_time = std::chrono::steady_clock::time_point;

constexpr int16_t first_generation = 1;
constexpr int16_t later_generation = 7;
constexpr std::chrono::milliseconds window{chain::config::block_interval_ms};
constexpr std::chrono::milliseconds within_window = window / 2;
constexpr std::chrono::microseconds just_inside = window - std::chrono::microseconds{1};
constexpr std::chrono::seconds long_after{10};

/// Builds a deterministic block id distinct per block number and label.
block_id_type make_block_id(uint32_t block_num, std::string_view label) {
   const std::string seed = std::string(label) + "-" + std::to_string(block_num);
   return block_id_type::hash(seed.data(), seed.size());
}

/// A handshake as populate_handshake fills it, for a node at the given root and head.
handshake_message make_handshake(uint32_t root_num, uint32_t head_num, std::string_view node) {
   handshake_message hs;
   hs.network_version = wire_protocol_base_version;
   hs.chain_id = chain_id_type::empty_chain_id();
   hs.node_id = fc::sha256::hash(node.data(), node.size());
   hs.fork_db_root_id = make_block_id(root_num, node);
   hs.fork_db_head_id = make_block_id(head_num, node);
   hs.chain_head_id = make_block_id(head_num, node);
   hs.p2p_address = std::string(node) + ":9876";
   hs.agent = "test agent";
   return hs;
}

/// The record send_handshake keeps after sending `sent` while the peer's last handshake was `peer`.
handshake_send_record make_record(handshake_message sent, const handshake_message& peer, steady_time time) {
   sent.generation = first_generation;
   return handshake_send_record{.sent = std::move(sent), .peer_last = peer, .time = time};
}
} // namespace

BOOST_AUTO_TEST_SUITE(handshake_repeat_tests)

// The window is one block interval.
BOOST_AUTO_TEST_CASE(window_is_one_block_interval) {
   BOOST_CHECK(handshake_repeat_window == std::chrono::milliseconds{chain::config::block_interval_ms});
}

// Only the three block ids are compared; the generation and the fields fixed for a session are not.
BOOST_AUTO_TEST_CASE(only_the_block_ids_are_compared) {
   const handshake_message base = make_handshake(10, 20, "me");
   const handshake_message other = make_handshake(11, 21, "other");
   auto differs = [&](auto&& mutate) {
      handshake_message changed = base;
      mutate(changed);
      BOOST_CHECK_EQUAL(same_chain_state(changed, base), same_chain_state(base, changed));
      return !same_chain_state(changed, base);
   };
   BOOST_CHECK(differs([&](handshake_message& h) { h.fork_db_root_id = other.fork_db_root_id; }));
   BOOST_CHECK(differs([&](handshake_message& h) { h.fork_db_head_id = other.fork_db_head_id; }));
   BOOST_CHECK(differs([&](handshake_message& h) { h.chain_head_id = other.chain_head_id; }));

   BOOST_CHECK(!differs([](handshake_message& h) { h.generation = later_generation; }));
   BOOST_CHECK(!differs([](handshake_message& h) { ++h.network_version; }));
   BOOST_CHECK(!differs([&](handshake_message& h) { h.chain_id = chain_id_type(other.node_id.str()); }));
   BOOST_CHECK(!differs([&](handshake_message& h) { h.node_id = other.node_id; }));
   BOOST_CHECK(!differs([&](handshake_message& h) { h.p2p_address = other.p2p_address; }));
   BOOST_CHECK(!differs([&](handshake_message& h) { h.agent = "other agent"; }));
}

// The same state on both sides, sent less than one block interval ago, is a repeat.
BOOST_AUTO_TEST_CASE(unchanged_state_within_window_repeats) {
   const steady_time t0 = std::chrono::steady_clock::now();
   const handshake_message mine = make_handshake(10, 20, "me");
   const handshake_message peer = make_handshake(30, 40, "peer");
   const handshake_send_record last = make_record(mine, peer, t0);

   BOOST_CHECK(handshake_repeats_last(mine, peer, last, t0, window));
   BOOST_CHECK(handshake_repeats_last(mine, peer, last, t0 + within_window, window));
   BOOST_CHECK(handshake_repeats_last(mine, peer, last, t0 + just_inside, window));
}

// Once one block interval has passed, the same handshake is sent again.
BOOST_AUTO_TEST_CASE(unchanged_state_after_window_is_sent) {
   const steady_time t0 = std::chrono::steady_clock::now();
   const handshake_message mine = make_handshake(10, 20, "me");
   const handshake_message peer = make_handshake(30, 40, "peer");
   const handshake_send_record last = make_record(mine, peer, t0);

   BOOST_CHECK(!handshake_repeats_last(mine, peer, last, t0 + window, window));
   BOOST_CHECK(!handshake_repeats_last(mine, peer, last, t0 + long_after, window));
}

// The first handshake after any change in our state goes out at once.
BOOST_AUTO_TEST_CASE(our_state_change_is_sent) {
   const steady_time t0 = std::chrono::steady_clock::now();
   const handshake_message mine = make_handshake(10, 20, "me");
   const handshake_message peer = make_handshake(30, 40, "peer");
   const handshake_send_record last = make_record(mine, peer, t0);

   handshake_message new_head = mine;
   new_head.fork_db_head_id = make_block_id(21, "me");
   BOOST_CHECK(!handshake_repeats_last(new_head, peer, last, t0, window));

   handshake_message new_root = mine;
   new_root.fork_db_root_id = make_block_id(11, "me");
   BOOST_CHECK(!handshake_repeats_last(new_root, peer, last, t0, window));

   handshake_message new_chain_head = mine;
   new_chain_head.chain_head_id = make_block_id(21, "me");
   BOOST_CHECK(!handshake_repeats_last(new_chain_head, peer, last, t0, window));
}

// A changed handshake from the peer since ours went out means the peer may evaluate ours differently now, so ours
// goes out again even when it is unchanged.
BOOST_AUTO_TEST_CASE(peer_state_change_is_answered) {
   const steady_time t0 = std::chrono::steady_clock::now();
   const handshake_message mine = make_handshake(10, 20, "me");
   const handshake_message peer = make_handshake(30, 40, "peer");
   const handshake_send_record last = make_record(mine, peer, t0);

   const handshake_message peer_advanced = make_handshake(31, 41, "peer");
   BOOST_CHECK(!handshake_repeats_last(mine, peer_advanced, last, t0, window));
}

// A peer handshake with a new generation but the same state does not by itself make ours a new one.
BOOST_AUTO_TEST_CASE(peer_generation_alone_is_not_a_change) {
   const steady_time t0 = std::chrono::steady_clock::now();
   const handshake_message mine = make_handshake(10, 20, "me");
   const handshake_message peer = make_handshake(30, 40, "peer");
   const handshake_send_record last = make_record(mine, peer, t0);

   handshake_message peer_again = peer;
   peer_again.generation = later_generation;
   BOOST_CHECK(handshake_repeats_last(mine, peer_again, last, t0, window));
}

// No handshake sent in the session yet (a new or reset record) is never a repeat, so the handshake that opens a
// session, including after a reconnect, always goes out.
BOOST_AUTO_TEST_CASE(first_handshake_of_session_is_sent) {
   const steady_time t0 = std::chrono::steady_clock::now();
   // value-initialized, as connection::_close resets it
   const handshake_send_record reset_record = handshake_send_record();
   const handshake_message empty = handshake_message();
   BOOST_CHECK(!handshake_repeats_last(empty, empty, reset_record, t0, window));
   BOOST_CHECK(!handshake_repeats_last(empty, empty, reset_record, reset_record.time, window));

   const handshake_message mine = make_handshake(10, 20, "me");
   handshake_send_record unsent{.sent = mine, .peer_last = empty, .time = t0};
   unsent.sent.generation = 0;
   BOOST_CHECK(!handshake_repeats_last(mine, empty, unsent, t0, window));
}

BOOST_AUTO_TEST_SUITE_END()
