#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace sysio::peer_scoring {

   // score range and baseline
   constexpr int32_t baseline           = 100;
   constexpr int32_t min_score          =   0;
   constexpr int32_t max_score          = 200;
   constexpr int32_t eviction_threshold =  50;

   // score deltas
   constexpr int32_t unique_block       =   3;
   constexpr int32_t block_accepted     =   1;
   constexpr int32_t block_rejected     =  -5;
   constexpr int32_t max_violated       = -30;
   constexpr int32_t fatal_violation    = -50;
   constexpr int32_t heartbeat_timeout  = -10;
   constexpr int32_t benign_close       =  -5;
   constexpr int32_t block_nack         =  -2;

   /// Thread-safe peer score with CAS-based adjustment and clamping.
   /// Intended to be embedded as a member of a connection object.
   class peer_score {
   public:
      int32_t get() const { return score_.load(std::memory_order_relaxed); }

      void adjust(int32_t delta) {
         int32_t old_val = score_.load(std::memory_order_relaxed);
         int32_t new_val;
         do {
            new_val = std::clamp(old_val + delta, min_score, max_score);
         } while (!score_.compare_exchange_weak(old_val, new_val, std::memory_order_relaxed));
      }

      void decay() {
         int32_t old_val = score_.load(std::memory_order_relaxed);
         if (old_val == baseline)
            return;
         int32_t new_val = old_val > baseline ? old_val - 1 : old_val + 1;
         score_.compare_exchange_strong(old_val, new_val, std::memory_order_relaxed);
      }

   private:
      std::atomic<int32_t> score_{baseline};
   };

   /// Select the peer to evict so that a new connection can be admitted over a connection limit: the evictable peer
   /// with the lowest score, when that score is below eviction_threshold. No other peer is evicted, so a node full of
   /// peers in good standing refuses the new connection instead. A peer already closing is never chosen, since its
   /// slot is being freed anyway.
   ///
   /// @tparam PeerPtr   shared pointer to a peer that provides get_peer_score() and closed()
   /// @param  peers     provides for_each_connection(), which visits every peer
   /// @param  evictable true for a peer that counts against the limit and may be evicted
   /// @return the peer to evict, or a null pointer when no peer qualifies
   template <typename PeerPtr, typename Peers, typename Evictable>
   PeerPtr select_eviction_victim(const Peers& peers, Evictable&& evictable) {
      PeerPtr victim;
      int32_t lowest_score = eviction_threshold;
      peers.for_each_connection([&](const PeerPtr& peer) {
         if (!peer->closed() && evictable(peer)) {
            if (const int32_t score = peer->get_peer_score(); score < lowest_score) {
               lowest_score = score;
               victim = peer;
            }
         }
         return true;
      });
      return victim;
   }

} // namespace sysio::peer_scoring
