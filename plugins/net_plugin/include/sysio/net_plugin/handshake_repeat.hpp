#pragma once

#include <sysio/chain/config.hpp>
#include <sysio/net_plugin/protocol.hpp>

#include <chrono>

namespace sysio {

/// Within this interval of the last handshake on a connection, a handshake that repeats it is not sent.
inline constexpr std::chrono::milliseconds handshake_repeat_window{chain::config::block_interval_ms};

/// The last handshake queued on a connection, with the peer's handshake as of then. Reset when the session ends
/// or the write queue is flushed, since a flushed handshake never reaches the peer.
struct handshake_send_record {
   handshake_message sent;                     ///< generation 0 until a handshake is queued in the session
   handshake_message peer_last;                ///< the peer's last handshake when `sent` was queued
   std::chrono::steady_clock::time_point time; ///< when `sent` was queued
};

/// True when @p a and @p b report the same fork database root, fork database head and chain head. The other
/// fields are fixed for a session.
inline bool same_chain_state(const handshake_message& a, const handshake_message& b) {
   return a.fork_db_head_id == b.fork_db_head_id && a.chain_head_id == b.chain_head_id &&
          a.fork_db_root_id == b.fork_db_root_id;
}

/**
 * True when @p next would repeat the last handshake sent on a connection.
 *
 * It repeats it when a handshake was queued in this session less than @p window before @p now with the same chain
 * state as @p next, and the peer's last handshake @p peer_now reports the same chain state as it did then. A change
 * on either side, or the first handshake of a session, is never a repeat.
 */
inline bool handshake_repeats_last(const handshake_message& next, const handshake_message& peer_now,
                                   const handshake_send_record& last, std::chrono::steady_clock::time_point now,
                                   std::chrono::steady_clock::duration window) {
   if (last.sent.generation == 0)
      return false;
   if (now - last.time >= window)
      return false;
   return same_chain_state(next, last.sent) && same_chain_state(peer_now, last.peer_last);
}

} // namespace sysio
