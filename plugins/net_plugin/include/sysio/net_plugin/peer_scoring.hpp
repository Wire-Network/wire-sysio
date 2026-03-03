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

} // namespace sysio::peer_scoring
