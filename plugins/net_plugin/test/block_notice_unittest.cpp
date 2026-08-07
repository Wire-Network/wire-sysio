#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/net_utils.hpp>

using namespace sysio::net_utils;

BOOST_AUTO_TEST_SUITE(block_notice_handling)

// A notice for a block we already hold is the only case that records peer knowledge.
BOOST_AUTO_TEST_CASE(announced_block_already_held_records_peer) {
   BOOST_CHECK(classify_block_notice(true, true) == block_notice_action::record_peer_has_block);
   BOOST_CHECK(classify_block_notice(true, false) == block_notice_action::record_peer_has_block);
}

// Missing both the block and its parent means we are two or more behind, so ask for the branch.
BOOST_AUTO_TEST_CASE(missing_block_and_parent_requests_blocks) {
   BOOST_CHECK(classify_block_notice(false, false) == block_notice_action::request_blocks);
}

// Holding the parent but not the block leaves nothing to do in the handler itself.
BOOST_AUTO_TEST_CASE(missing_block_with_parent_held_is_ignored) {
   BOOST_CHECK(classify_block_notice(false, true) == block_notice_action::ignore);
}

// The liveness property: only a notice naming a block we already hold may refresh latest_blk_time.
// Marking either missing-block case as progress would defer the check_heartbeat handshake that
// recovers the block when no further block is produced.
BOOST_AUTO_TEST_CASE(only_a_held_block_marks_progress) {
   BOOST_CHECK_EQUAL(block_notice_marks_progress(block_notice_action::record_peer_has_block), true);
   BOOST_CHECK_EQUAL(block_notice_marks_progress(block_notice_action::request_blocks), false);
   BOOST_CHECK_EQUAL(block_notice_marks_progress(block_notice_action::ignore), false);
}

// Stated over the dispatcher inputs rather than the action, so the guarantee survives a reclassification.
BOOST_AUTO_TEST_CASE(a_notice_for_a_missing_block_never_marks_progress) {
   BOOST_CHECK_EQUAL(block_notice_marks_progress(classify_block_notice(false, true)), false);
   BOOST_CHECK_EQUAL(block_notice_marks_progress(classify_block_notice(false, false)), false);
   BOOST_CHECK_EQUAL(block_notice_marks_progress(classify_block_notice(true, true)), true);
}

BOOST_AUTO_TEST_SUITE_END()
