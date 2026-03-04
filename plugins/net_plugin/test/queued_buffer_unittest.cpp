#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/queued_buffer.hpp>

using namespace sysio;

namespace {

send_buffer_type make_buffer(std::size_t sz) {
   return std::make_shared<std::vector<char>>(sz, 'x');
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(queued_buffer_tests)

// ---- Basic enqueue / drain ----

BOOST_AUTO_TEST_CASE(enqueue_ctrl_and_drain) {
   queued_buffer qb;
   auto buf = make_buffer(100);
   auto r = qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                                buf, 1, go_away_reason::no_reason, std::nullopt);
   BOOST_CHECK(r.ok);
   BOOST_CHECK(r.needs_drain);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 100u);

   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   BOOST_CHECK_EQUAL(bufs.size(), 1u);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 0u);
}

BOOST_AUTO_TEST_CASE(enqueue_sync_and_drain) {
   queued_buffer qb;
   auto buf = make_buffer(200);
   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::block_sync,
                       buf, 1, go_away_reason::no_reason, block_num_type{5});
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 200u);

   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   BOOST_CHECK_EQUAL(bufs.size(), 1u);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 0u);
}

BOOST_AUTO_TEST_CASE(enqueue_general_block_and_drain) {
   queued_buffer qb;
   auto buf = make_buffer(300);
   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::general,
                       buf, 1, go_away_reason::no_reason, block_num_type{10});
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 300u);

   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   BOOST_CHECK_EQUAL(bufs.size(), 1u);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 0u);
}

BOOST_AUTO_TEST_CASE(enqueue_trx_and_drain) {
   queued_buffer qb;
   auto buf = make_buffer(150);
   qb.add_write_queue(msg_type_t::transaction_message, queued_buffer::queue_t::general,
                       buf, 1, go_away_reason::no_reason, std::nullopt);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 150u);

   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   BOOST_CHECK_EQUAL(bufs.size(), 1u);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 0u);
}

BOOST_AUTO_TEST_CASE(trx_notice_routes_to_trx_queue) {
   queued_buffer qb;
   auto buf = make_buffer(50);
   qb.add_write_queue(msg_type_t::transaction_notice_message, queued_buffer::queue_t::general,
                       buf, 1, go_away_reason::no_reason, std::nullopt);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 50u);

   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   BOOST_CHECK_EQUAL(bufs.size(), 1u);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 0u);
}

// ---- Priority ordering ----

BOOST_AUTO_TEST_CASE(priority_ctrl_before_sync) {
   queued_buffer qb;
   // Enqueue sync first, then ctrl.
   auto sync_buf = make_buffer(10);
   auto ctrl_buf = make_buffer(20);

   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::block_sync,
                       sync_buf, 1, go_away_reason::no_reason, block_num_type{1});
   qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                       ctrl_buf, 1, go_away_reason::no_reason, std::nullopt);

   BOOST_CHECK_EQUAL(qb.write_queue_size(), 30u);

   // First drain should pick ctrl (vote_message) — size 20.
   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   BOOST_CHECK_EQUAL(bufs.size(), 1u);
   BOOST_CHECK_EQUAL(bufs[0].size(), 20u); // ctrl_buf
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 10u);

   // Simulate clear_out_queue by calling reset on out_queue via fill again.
   // We need to clear_write_queue + reset to simulate, but that would lose the sync entry.
   // Instead, just verify the second drain picks sync.
   // NOTE: fill_out_buffer moves items to _out_queue. Subsequent calls won't re-drain _out_queue.
   // But since we can't call clear_out_queue without a connection*, we just verify the first
   // drain chose ctrl over sync. The remaining 10 bytes are in sync.
}

BOOST_AUTO_TEST_CASE(priority_sync_before_general) {
   queued_buffer qb;
   auto gen_buf  = make_buffer(10);
   auto sync_buf = make_buffer(20);

   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::general,
                       gen_buf, 1, go_away_reason::no_reason, block_num_type{5});
   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::block_sync,
                       sync_buf, 1, go_away_reason::no_reason, block_num_type{1});

   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   // With no ctrl messages, sync should drain first.
   BOOST_CHECK_EQUAL(bufs.size(), 1u);
   BOOST_CHECK_EQUAL(bufs[0].size(), 20u); // sync_buf
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 10u);
}

BOOST_AUTO_TEST_CASE(priority_general_before_trx) {
   queued_buffer qb;
   auto trx_buf = make_buffer(10);
   auto gen_buf = make_buffer(20);

   qb.add_write_queue(msg_type_t::transaction_message, queued_buffer::queue_t::general,
                       trx_buf, 1, go_away_reason::no_reason, std::nullopt);
   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::general,
                       gen_buf, 1, go_away_reason::no_reason, block_num_type{5});

   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   // general should drain before trx.
   BOOST_CHECK_EQUAL(bufs.size(), 1u);
   BOOST_CHECK_EQUAL(bufs[0].size(), 20u); // gen_buf (signed_block)
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 10u);
}

BOOST_AUTO_TEST_CASE(full_4tier_priority_ordering) {
   queued_buffer qb;
   auto trx_buf  = make_buffer(10);
   auto gen_buf  = make_buffer(20);
   auto sync_buf = make_buffer(30);
   auto ctrl_buf = make_buffer(40);

   // Enqueue in reverse priority order.
   qb.add_write_queue(msg_type_t::transaction_message, queued_buffer::queue_t::general,
                       trx_buf, 1, go_away_reason::no_reason, std::nullopt);
   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::general,
                       gen_buf, 1, go_away_reason::no_reason, block_num_type{5});
   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::block_sync,
                       sync_buf, 1, go_away_reason::no_reason, block_num_type{1});
   qb.add_write_queue(msg_type_t::handshake_message, queued_buffer::queue_t::general,
                       ctrl_buf, 1, go_away_reason::no_reason, std::nullopt);

   BOOST_CHECK_EQUAL(qb.write_queue_size(), 100u);

   // Drain 1: ctrl (40 bytes)
   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   BOOST_CHECK_EQUAL(bufs.size(), 1u);
   BOOST_CHECK_EQUAL(bufs[0].size(), 40u);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 60u);

   // Can't drain further without clearing _out_queue (needs connection*).
   // Verify remaining size is correct.
}

// ---- Control message routing ----

BOOST_AUTO_TEST_CASE(control_message_routing) {
   // Iterate all msg_type_t values; skip the non-ctrl types.
   // This mirrors how msg_type_t is derived from the net_message variant,
   // so new message types are automatically covered.
   for (uint32_t i = 0; i < to_index(msg_type_t::unknown); ++i) {
      auto mt = static_cast<msg_type_t>(i);
      // signed_block routes to block queue; trx types route to trx queue.
      if (mt == msg_type_t::signed_block ||
          mt == msg_type_t::transaction_message ||
          mt == msg_type_t::transaction_notice_message)
         continue;
      queued_buffer qb;
      auto ctrl_buf = make_buffer(10);
      auto trx_buf  = make_buffer(20);

      // Add trx first, then the control message.
      qb.add_write_queue(msg_type_t::transaction_message, queued_buffer::queue_t::general,
                          trx_buf, 1, go_away_reason::no_reason, std::nullopt);
      qb.add_write_queue(mt, queued_buffer::queue_t::general,
                          ctrl_buf, 1, go_away_reason::no_reason, std::nullopt);

      // Drain should pick ctrl (10 bytes) not trx (20 bytes).
      small_buf_vector bufs;
      qb.fill_out_buffer(bufs);
      BOOST_CHECK_EQUAL(bufs[0].size(), 10u);
   }
}

// ---- Size tracking ----

BOOST_AUTO_TEST_CASE(size_tracking) {
   queued_buffer qb;
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 0u);

   auto buf1 = make_buffer(100);
   auto buf2 = make_buffer(200);
   auto buf3 = make_buffer(300);

   qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                       buf1, 1, go_away_reason::no_reason, std::nullopt);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 100u);

   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::block_sync,
                       buf2, 1, go_away_reason::no_reason, block_num_type{1});
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 300u);

   qb.add_write_queue(msg_type_t::transaction_message, queued_buffer::queue_t::general,
                       buf3, 1, go_away_reason::no_reason, std::nullopt);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 600u);

   // Drain ctrl (100 bytes)
   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 500u);

   // clear_write_queue should reset to 0
   qb.clear_write_queue();
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 0u);
}

// ---- Drain claim CAS ----

BOOST_AUTO_TEST_CASE(drain_claim_cas) {
   queued_buffer qb;

   // First claim succeeds.
   BOOST_CHECK(qb.try_claim_drain());
   // Second claim fails (drain already pending).
   BOOST_CHECK(!qb.try_claim_drain());
   // Release.
   qb.release_drain();
   // Now claim succeeds again.
   BOOST_CHECK(qb.try_claim_drain());
   qb.release_drain();
}

// ---- ready_to_send ----

BOOST_AUTO_TEST_CASE(ready_to_send_empty) {
   queued_buffer qb;
   // Empty buffer is not ready.
   BOOST_CHECK(!qb.ready_to_send(1));
}

BOOST_AUTO_TEST_CASE(ready_to_send_with_data) {
   queued_buffer qb;
   auto buf = make_buffer(100);
   qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                       buf, 1, go_away_reason::no_reason, std::nullopt);
   // Data in write queue, out_queue empty → ready.
   BOOST_CHECK(qb.ready_to_send(1));
}

BOOST_AUTO_TEST_CASE(ready_to_send_during_async_write) {
   queued_buffer qb;
   auto buf1 = make_buffer(100);
   auto buf2 = make_buffer(200);

   qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                       buf1, 1, go_away_reason::no_reason, std::nullopt);

   // Drain to move buf1 into _out_queue.
   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);

   // Now _out_queue is non-empty (async write in progress).
   // Add more data — should NOT be ready because async write in progress.
   qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                       buf2, 1, go_away_reason::no_reason, std::nullopt);
   BOOST_CHECK(!qb.ready_to_send(1));
}

// ---- Queue size limit ----

BOOST_AUTO_TEST_CASE(queue_size_limit) {
   queued_buffer qb;
   // def_max_write_queue_size = 8MB * 10 = 80MB. Limit is 2x = 160MB.
   // Add a buffer that pushes past the limit.
   auto small_buf = make_buffer(1);
   auto r1 = qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                                  small_buf, 1, go_away_reason::no_reason, std::nullopt);
   BOOST_CHECK(r1.ok); // 1 byte is well under limit

   // Create a buffer just over the 2x limit.
   auto huge_buf = make_buffer(2 * def_max_write_queue_size + 1);
   auto r2 = qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                                  huge_buf, 1, go_away_reason::no_reason, std::nullopt);
   BOOST_CHECK(!r2.ok); // exceeds 2 * def_max_write_queue_size
}

// ---- needs_drain flag ----

BOOST_AUTO_TEST_CASE(needs_drain_when_out_queue_empty) {
   queued_buffer qb;
   auto buf = make_buffer(100);
   auto r = qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                                buf, 1, go_away_reason::no_reason, std::nullopt);
   // out_queue is empty → needs_drain should be true
   BOOST_CHECK(r.needs_drain);
}

BOOST_AUTO_TEST_CASE(needs_drain_false_during_async_write) {
   queued_buffer qb;
   auto buf1 = make_buffer(100);
   qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                       buf1, 1, go_away_reason::no_reason, std::nullopt);

   // Drain to populate _out_queue.
   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);

   // Now _out_queue is non-empty. Adding more data should set needs_drain = false.
   auto buf2 = make_buffer(100);
   auto r = qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                                buf2, 1, go_away_reason::no_reason, std::nullopt);
   BOOST_CHECK(!r.needs_drain);
}

// ---- reset / clear ----

BOOST_AUTO_TEST_CASE(reset_clears_everything) {
   queued_buffer qb;
   auto buf = make_buffer(100);
   qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                       buf, 1, go_away_reason::no_reason, std::nullopt);

   // Drain to populate _out_queue.
   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);

   // Add more data.
   auto buf2 = make_buffer(200);
   qb.add_write_queue(msg_type_t::signed_block, queued_buffer::queue_t::block_sync,
                       buf2, 1, go_away_reason::no_reason, block_num_type{1});

   qb.reset();
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 0u);
   BOOST_CHECK(!qb.ready_to_send(1));
}

// ---- Multiple items in same queue drain together ----

BOOST_AUTO_TEST_CASE(multiple_items_same_queue_drain_together) {
   queued_buffer qb;
   auto buf1 = make_buffer(10);
   auto buf2 = make_buffer(20);
   auto buf3 = make_buffer(30);

   // All go to ctrl queue.
   qb.add_write_queue(msg_type_t::vote_message, queued_buffer::queue_t::general,
                       buf1, 1, go_away_reason::no_reason, std::nullopt);
   qb.add_write_queue(msg_type_t::handshake_message, queued_buffer::queue_t::general,
                       buf2, 2, go_away_reason::no_reason, std::nullopt);
   qb.add_write_queue(msg_type_t::time_message, queued_buffer::queue_t::general,
                       buf3, 3, go_away_reason::no_reason, std::nullopt);

   small_buf_vector bufs;
   qb.fill_out_buffer(bufs);
   // All 3 should drain together since they're in the same (ctrl) queue.
   BOOST_CHECK_EQUAL(bufs.size(), 3u);
   BOOST_CHECK_EQUAL(bufs[0].size(), 10u);
   BOOST_CHECK_EQUAL(bufs[1].size(), 20u);
   BOOST_CHECK_EQUAL(bufs[2].size(), 30u);
   BOOST_CHECK_EQUAL(qb.write_queue_size(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
