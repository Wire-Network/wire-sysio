#pragma once
/**
 * @file sync_gate.hpp
 * @brief Pure building blocks of the chain_plugin sync gate — the predicate behind
 *        `chain_plugin::is_synced()` and the edge detector behind the
 *        `chain_plugin::synced()` signal — lifted out of the `.cpp`-private impl so
 *        they are unit-testable without standing up a chain.
 *
 * Plugins whose startup validates on-chain state via LOCAL table reads (the operator
 * daemons: batch_operator_plugin, underwriter_plugin) must not start until the node is
 * synced. On a cold-booting operator node those reads see mid-sync (possibly genesis)
 * state and fail spuriously. "Synced" here means the LAST IRREVERSIBLE block's time is
 * within a small window of wall-clock now — the irreversible state is what those
 * plugins' table reads actually serve (operator daemons run read-mode = irreversible).
 */

#include <sysio/chain/controller.hpp>

#include <fc/time.hpp>

namespace sysio::chain_apis {

   /// Behind-now gap (ms) under which the node counts as synced. Measured against the
   /// LAST IRREVERSIBLE block's time — the state operator-daemon table reads actually
   /// serve (read-mode = irreversible); see {@link lib_time_is_recent}. Must exceed
   /// steady-state finality lag (a few blocks) plus scheduling slack, and stay well
   /// under one epoch so gated consumers never start against stale state. Not an
   /// operator tunable.
   inline constexpr uint32_t default_sync_recency_ms = 5000;

   /**
    * @brief True when a block time is recent enough to treat the node as synced.
    *
    * While a cold-booting node syncs blocks the tested time trails `now` by the
    * catch-up gap; once caught up, it tracks `now` to within finality-lag jitter. The
    * window must exceed that jitter (plus scheduling slack) but stay well under one
    * epoch so gated consumers never start against stale state.
    *
    * @param block_time     The block timestamp to test — the caller's sync criterion
    *                       (the sync gate feeds the LAST IRREVERSIBLE block's time,
    *                       since that is the state operator-daemon reads serve).
    * @param now            Wall-clock now.
    * @param recency_window Maximum behind-now gap still considered synced.
    * @return True when `block_time >= now - recency_window`.
    */
   inline bool block_time_is_recent(fc::time_point block_time, fc::time_point now,
                                    fc::microseconds recency_window) {
      return block_time >= now - recency_window;
   }

   /**
    * @brief The "node is synced" predicate: the LAST IRREVERSIBLE block's time within
    *        `recency_window` of `now`.
    *
    * LIB recency, not head recency, deliberately: operator daemons run read-mode =
    * irreversible, so their table reads serve the IRREVERSIBLE state. Head-time
    * recency armed the underwriter's gate while the local LIB still trailed the rows
    * its preflight needed (observed live: a registration in block N was readable only
    * 50ms after a preflight read at LIB N−3). False while no irreversible root exists.
    *
    * @param chain          The controller to read the irreversible root from.
    * @param now            Wall-clock now.
    * @param recency_window Maximum behind-now gap still considered synced.
    * @return True when an irreversible root exists and its block time is recent.
    */
   inline bool lib_time_is_recent(const chain::controller& chain, fc::time_point now,
                                  fc::microseconds recency_window) {
      return chain.fork_db_has_root() &&
             block_time_is_recent(chain.fork_db_root().block_time(), now, recency_window);
   }

   /**
    * @brief Stale→synced edge detector behind the `chain_plugin::synced()` signal.
    *
    * Fed the {@link lib_time_is_recent} predicate once per accepted block, it reports
    * exactly the transitions from stale to synced — the emission points of the signal.
    * Deliberately re-arming rather than a once-per-process latch: a latch that fires
    * with no consumer connected while the chain then stalls leaves a later subscriber
    * waiting on a signal that can never re-fire; with re-arm semantics the next
    * stale→synced transition reaches it. Initial state is stale.
    *
    * Not thread-safe by contract — updated only from the main thread (block
    * application), like the controller signals it derives from.
    */
   class sync_transition {
   public:
      /**
       * @brief Record the current sync state and report a stale→synced edge.
       *
       * @param now_synced The current value of the sync predicate.
       * @return True exactly when `now_synced` is true and the previously recorded
       *         state was stale.
       */
      bool update(bool now_synced) {
         const bool fired = now_synced && !was_synced;
         was_synced = now_synced;
         return fired;
      }

   private:
      /// The last recorded sync state; starts stale.
      bool was_synced = false;
   };

} // namespace sysio::chain_apis
