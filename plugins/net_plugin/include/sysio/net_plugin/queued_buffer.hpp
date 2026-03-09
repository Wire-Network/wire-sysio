#pragma once

#include <sysio/net_plugin/protocol.hpp>
#include <sysio/net_plugin/buffer_factory.hpp>
#include <sysio/net_plugin/net_logger.hpp>
#include <sysio/chain/thread_utils.hpp>
#include <fc/mutex.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/noncopyable.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <deque>
#include <optional>

namespace sysio {

   class connection; // forward declaration for clear_out_queue

   using small_buf_vector = boost::container::small_vector<boost::asio::const_buffer, 32>;

   constexpr auto     def_send_buffer_size_mb = 8;
   constexpr auto     def_send_buffer_size = 1024*1024*def_send_buffer_size_mb;
   constexpr auto     def_max_write_queue_size = def_send_buffer_size*10;

   // thread safe
   class queued_buffer : boost::noncopyable {
   public:
      void reset() {
         clear_write_queue();
         fc::lock_guard g( _mtx );
         _out_queue.clear();
      }

      void clear_write_queue() {
         fc::lock_guard g( _mtx );
         _ctrl_write_queue.clear();
         _block_write_queue.clear();
         _sync_write_queue.clear();
         _trx_write_queue.clear();
         _write_queue_size.store(0, std::memory_order_relaxed);
         _write_drain_pending.store(false, std::memory_order_relaxed);
      }

      // Declared here, defined out-of-line in net_plugin.cpp (depends on connection*).
      void clear_out_queue(connection* conn, boost::system::error_code ec, std::size_t number_of_bytes_written);

      uint32_t write_queue_size() const {
         return _write_queue_size.load(std::memory_order_relaxed);
      }

      // Returns true if caller wins the drain race; false if a drain is already pending.
      bool try_claim_drain() {
         return !_write_drain_pending.exchange(true, std::memory_order_acq_rel);
      }
      void release_drain() {
         _write_drain_pending.store(false, std::memory_order_release);
      }

      // called from connection strand
      bool ready_to_send(connection_id_t connection_id) const {
         fc::unique_lock g( _mtx );
         // if out_queue is not empty then async_write is in progress
         const bool async_write_in_progress = !_out_queue.empty();
         const bool ready = !async_write_in_progress && _write_queue_size.load(std::memory_order_relaxed) != 0;
         g.unlock();
         if (async_write_in_progress) {
            fc_dlog(p2p_conn_log, "Connection - {} not ready to send data, async write in progress", connection_id);
         }
         return ready;
      }

      enum class queue_t { block_sync, general };
      struct add_result { bool ok; bool needs_drain; };
      add_result add_write_queue(msg_type_t net_msg,
                                 queue_t queue,
                                 const send_buffer_type& buff,
                                 connection_id_t conn_id,
                                 go_away_reason close_after_send,
                                 std::optional<block_num_type> block_num) {
         fc::lock_guard g( _mtx );
         if( net_msg == msg_type_t::transaction_message || net_msg == msg_type_t::transaction_notice_message ) {
            _trx_write_queue.push_back( {buff, conn_id, close_after_send, net_msg, block_num} );
         } else if (queue == queue_t::block_sync) {
            _sync_write_queue.push_back( {buff, conn_id, close_after_send, net_msg, block_num} );
         } else if (net_msg == msg_type_t::signed_block) {
            _block_write_queue.push_back( {buff, conn_id, close_after_send, net_msg, block_num} );
         } else {
            _ctrl_write_queue.push_back( {buff, conn_id, close_after_send, net_msg, block_num} );
         }
         auto new_size = _write_queue_size.fetch_add(buff->size(), std::memory_order_relaxed) + buff->size();
         return { new_size <= 2 * def_max_write_queue_size, _out_queue.empty() };
      }

      void fill_out_buffer( small_buf_vector& bufs ) {
         fc::lock_guard g( _mtx );
         if (!_ctrl_write_queue.empty()) { // always send ctrl/consensus msgs first (votes, handshakes)
            fill_out_buffer( bufs, _ctrl_write_queue );
         } else if (!_sync_write_queue.empty()) { // then sync blocks
            fill_out_buffer( bufs, _sync_write_queue );
         } else if (!_block_write_queue.empty()) { // then head blocks
            fill_out_buffer( bufs, _block_write_queue );
         } else {
            fill_out_buffer( bufs, _trx_write_queue );
            assert(_trx_write_queue.empty() && _block_write_queue.empty() && _sync_write_queue.empty() &&
                   _ctrl_write_queue.empty() && _write_queue_size.load(std::memory_order_relaxed) == 0);
         }
      }

   private:
      struct queued_write {
         send_buffer_type             buff;
         connection_id_t              connection_id{0};
         go_away_reason               close_after_send{go_away_reason::no_reason};
         msg_type_t                   net_msg{};
         std::optional<block_num_type> block_num;
      };

      void fill_out_buffer( small_buf_vector& bufs,
                            std::deque<queued_write>& w_queue ) REQUIRES(_mtx) {
         while ( !w_queue.empty() ) {
            auto& m = w_queue.front();
            bufs.emplace_back( m.buff->data(), m.buff->size() );
            _write_queue_size.fetch_sub(m.buff->size(), std::memory_order_relaxed);
            _out_queue.emplace_back( m );
            w_queue.pop_front();
         }
      }

      alignas(hardware_destructive_interference_sz)
      std::atomic<uint32_t> _write_queue_size{0}; // total size of all 4 write queues
      std::atomic<bool>     _write_drain_pending{false}; // coalesces queue_write_mt drain posts

      alignas(hardware_destructive_interference_sz)
      mutable fc::mutex          _mtx;
      std::deque<queued_write>   _ctrl_write_queue  GUARDED_BY(_mtx); // consensus/control msgs (votes, handshakes, go_away, etc.)
      std::deque<queued_write>   _sync_write_queue  GUARDED_BY(_mtx); // sync blocks
      std::deque<queued_write>   _block_write_queue GUARDED_BY(_mtx); // head blocks (signed_block via queue_t::general)
      std::deque<queued_write>   _trx_write_queue   GUARDED_BY(_mtx); // transactions (lowest priority)
      std::deque<queued_write>   _out_queue         GUARDED_BY(_mtx); // currently being async_written

   }; // queued_buffer

} // namespace sysio
