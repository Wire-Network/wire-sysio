#pragma once

#include <sysio/sysio.hpp>

/**
 * @brief Test contract pinning down EOSIO-style action_trace ordering.
 *
 * Background: each call to apply_context::exec() runs actions in this order:
 *
 *     1. The receiver's own action body (the "self" apply).
 *     2. Every action queued by require_recipient(), in FIFO order
 *        (the "notification phase").
 *     3. Every action queued by send_context_free_inline(), in FIFO order
 *        (the "CFA inline phase").
 *     4. Every action queued by send_inline(), in FIFO order
 *        (the "regular inline phase").
 *
 * As a diagram:
 *
 *   +--------+    +---------------+    +-------------+    +-----------------+
 *   |  self  | -> | notifications | -> | CFA inlines | -> | regular inlines |
 *   |        |    |     FIFO      |    |    FIFO     |    |      FIFO       |
 *   +--------+    +---------------+    +-------------+    +-----------------+
 *    phase 1         phase 2              phase 3              phase 4
 *
 * Crucially, action_ordinal is the SCHEDULE order (the order in which
 * require_recipient / send_*_inline host calls were issued), while
 * global_sequence is the EXECUTION order (assigned only when an action's
 * receipt is built, after that action has actually run).
 *
 * These two orderings DIVERGE whenever a contract intermixes notifications
 * and inlines within a single action body. action_traces is indexed by
 * action_ordinal, so iterating the action_traces vector in index order
 * yields the SCHEDULE order, NOT the execution order.
 *
 * Consumers (history APIs, indexers, monitoring) that flatten action_traces
 * and assume the vector is execution-ordered will misread the trace whenever
 * a contract uses this mixed pattern. The vaults.sx incident (2021, ~$13M
 * loss) exploited exactly this confusion in a contract whose author had
 * assumed their notification handler would observe the post-inline state.
 *
 * ----------------------------------------------------------------------------
 * All six cases at a glance, in the EOSIO PR #6897 trace-tree notation:
 *
 *     (act.account::act.name -> receiver) [N]
 *        ||
 *        ||===> (child_act::child_name -> child_receiver) [M]
 *
 * Bracket numbers are EXECUTION position (matches receipt.global_sequence).
 * Tree SHAPE follows creator_action_ordinal: each node lists its children
 * in the order their parent scheduled them. action_ordinal corresponds to
 * a depth-first walk of this tree.
 *
 * Whenever the top-to-bottom bracket sequence is NOT monotonic, schedule
 * order and execution order disagree - the brackets visualize the divergence
 * directly.
 *
 *
 *   Case 1 - bare self apply (noop):
 *
 *     (alice::noop -> alice) [1]
 *
 *
 *   Case 2 - notification handler queues an inline (notiffire):
 *     alice body : require_recipient(bob);
 *     bob handler: send_inline(noop on bob);
 *
 *     (alice::notiffire -> alice) [1]
 *        ||
 *        ||===> (alice::notiffire -> bob) [2]
 *                  ||
 *                  ||===> (bob::noop -> bob) [3]
 *
 *
 *   Case 3 - schedule-vs-execution divergence (inlnotify) - HEADLINE:
 *     alice body : send_inline(noop on carol);   // scheduled 1st
 *                  require_recipient(bob);       // scheduled 2nd
 *
 *     (alice::inlnotify -> alice) [1]
 *        ||
 *        ||===> (carol::noop -> carol) [3]          <-- scheduled 1st, runs 3rd
 *        ||
 *        ||===> (alice::inlnotify -> bob) [2]       <-- scheduled 2nd, runs 2nd
 *
 *
 *   Case 4 - natural ordering, contrast with Case 3 (notifyinl):
 *     alice body : require_recipient(bob);       // scheduled 1st
 *                  send_inline(noop on carol);   // scheduled 2nd
 *
 *     (alice::notifyinl -> alice) [1]
 *        ||
 *        ||===> (alice::notifyinl -> bob) [2]
 *        ||
 *        ||===> (carol::noop -> carol) [3]
 *
 *
 *   Case 5 - nested apply_context: inline body fires its own notification
 *     (nestedcall):
 *     alice body  : require_recipient(bob);
 *                   send_inline(notifyme(bob) on carol);
 *     carol body  : require_recipient(bob);    // INSIDE inline's apply_context
 *
 *     (alice::nestedcall -> alice) [1]
 *        ||
 *        ||===> (alice::nestedcall -> bob) [2]
 *        ||
 *        ||===> (carol::notifyme -> carol) [3]
 *                  ||
 *                  ||===> (carol::notifyme -> bob) [4]
 *
 *
 *   Case 6 - CFA inline + regular inline + notification (mixedord) -
 *     TRIPLE divergence:
 *     alice body : send_inline(noop on carol);              // scheduled 1st
 *                  send_context_free_inline(noop on dave);  // scheduled 2nd
 *                  require_recipient(bob);                  // scheduled 3rd
 *
 *     (alice::mixedord -> alice) [1]
 *        ||
 *        ||===> (carol::noop -> carol) [4]      <-- scheduled 1st, runs LAST
 *        ||
 *        ||===> (dave::noop -> dave)   [3]      <-- scheduled 2nd, runs 3rd
 *        ||
 *        ||===> (alice::mixedord -> bob) [2]    <-- scheduled 3rd, runs 2nd
 *
 * ----------------------------------------------------------------------------
 *
 * The 6 actions defined below drive unittests/action_ordering_tests.cpp
 * through these permutations so the rule above remains a regression-tested
 * invariant of the chain.
 */
class [[sysio::contract]] action_order_test : public sysio::contract {
public:
   using sysio::contract::contract;

   /**
    * @brief Silent stub. Used as the target of inline / CFA inline calls.
    *
    * Deliberately does nothing so the resulting action_trace records only
    * the apply itself (no further notifications or inlines), keeping
    * fixtures small and the global_sequence math easy to follow.
    */
   [[sysio::action]] void noop();

   /**
    * @brief Issues a single require_recipient(obs). Used standalone.
    *
    * Trace shape: 2 traces total. action_traces[0] is the self apply on
    * the source; action_traces[1] is the notification with receiver=obs
    * and act.account=source. action_ordinal and global_sequence agree.
    */
   [[sysio::action]] void notifyme(sysio::name obs);

   /**
    * @brief Drives the "notification handler queues an inline" pattern.
    *
    * Body: require_recipient(obs).
    *
    * The companion on_notify("*::notiffire") handler runs when obs is
    * notified, and inside that handler it calls send_inline(noop) targeting
    * get_self() - i.e. the notification recipient (obs), NOT the source.
    * The host enqueues that inline in the SAME apply_context's
    * _inline_actions queue as the original action's body, so the inline
    * runs AFTER all sibling notifications - it does not jump the line just
    * because it was scheduled from inside a handler.
    *
    * Trace shape: 3 traces - source self, source's notification to obs,
    * inline noop on obs. global_sequence == action_ordinal for this case.
    */
   [[sysio::action]] void notiffire(sysio::name obs);

   /**
    * @brief THE HEADLINE TEST: schedule order diverges from execution order.
    *
    * Body, in order:
    *     1. send_inline(noop on inlt)       // scheduled FIRST  (ordinal +1)
    *     2. require_recipient(obs)          // scheduled SECOND (ordinal +2)
    *
    * Despite that scheduling, the chain executes the notification BEFORE
    * the inline. The action_traces vector therefore looks like:
    *
    *     index 0 : self apply on source         ordinal=1 gseq=g0
    *     index 1 : noop inline (target inlt)    ordinal=2 gseq=g0+2
    *     index 2 : notification on obs          ordinal=3 gseq=g0+1
    *
    *                            source body
    *                                 |
    *                                 v
    *     +---------------------+ phase 1
    *     | self  (ord 1)       |  gseq=g0
    *     +---------------------+
    *                                 |
    *                                 v
    *     +---------------------+ phase 2 (notifications)
    *     | obs notify  (ord 3) |  gseq=g0+1   <-- 2nd to run, ordinal 3
    *     +---------------------+
    *                                 |
    *                                 v
    *     +---------------------+ phase 4 (regular inlines)
    *     | noop inline (ord 2) |  gseq=g0+2   <-- 3rd to run, ordinal 2
    *     +---------------------+
    *
    *     action_traces VECTOR order (by ordinal):  [ self, inline, notify ]
    *     action_traces EXEC   order (by gseq):     [ self, notify, inline ]
    *
    * A consumer iterating action_traces[0..] in vector order sees
    * "self -> inline -> notification", but the chain actually ran
    * "self -> notification -> inline".
    *
    * History APIs MUST sort by receipt.global_sequence (or otherwise reorder
    * relative to action_ordinal) before exposing execution-ordered output.
    * Failing to do so reproduces the trap that lets a malicious actor
    * confuse downstream tooling about the true sequence of effects.
    */
   [[sysio::action]] void inlnotify(sysio::name obs, sysio::name inlt);

   /**
    * @brief Sanity check: scheduling notification first AGREES with execution.
    *
    * Body, in order:
    *     1. require_recipient(obs)          // scheduled FIRST  (ordinal +1)
    *     2. send_inline(noop on inlt)       // scheduled SECOND (ordinal +2)
    *
    * Here the notification runs first by virtue of being scheduled first,
    * AND because notifications phase runs before inlines phase, so the
    * effect is unambiguous. Trace shape: ordinal==global_sequence ordering
    * agrees throughout. Pairs with inlnotify so the test makes the contrast
    * obvious.
    */
   [[sysio::action]] void notifyinl(sysio::name obs, sysio::name inlt);

   /**
    * @brief Nested: an inline action that itself notifies a third party.
    *
    * Body, in order:
    *     1. require_recipient(obs)                       // ordinal +1
    *     2. send_inline(notifyme(obs) on inlt)           // ordinal +2
    *
    * When the inline runs, it creates a NEW apply_context with its own
    * _notified / _inline_actions queues. The inline's own
    * require_recipient(obs) is local to that nested context and produces
    * its own notification action_trace.
    *
    * Trace shape:
    *     index 0: source self                            ord=1 gseq=g0
    *     index 1: source's notification to obs           ord=2 gseq=g0+1
    *     index 2: inline notifyme on inlt                ord=3 gseq=g0+2
    *     index 3: inline's own notification to obs       ord=4 gseq=g0+3
    *
    *     outer apply_context (source's nestedcall)
    *       |
    *       +-- self    (ord 1)             gseq=g0
    *       +-- notify  obs    (ord 2)      gseq=g0+1
    *       +-- inline notifyme  (ord 3) --+
    *                                      v
    *                       inner apply_context (inlt's notifyme)
    *                            |
    *                            +-- self     (ord 3)    gseq=g0+2
    *                            +-- notify obs (ord 4)  gseq=g0+3
    *
    * Here schedule order matches execution order because the source body
    * scheduled its notification BEFORE its inline. The point of this case
    * is to verify that nested apply_contexts behave independently: an inner
    * notification does NOT leak into the outer queue.
    */
   [[sysio::action]] void nestedcall(sysio::name obs, sysio::name inlt);

   /**
    * @brief CFA-inline runs BEFORE regular inline, regardless of schedule.
    *
    * Body, in order:
    *     1. send_inline(noop on inlt)              // ordinal +1
    *     2. send_context_free_inline(noop on cfat) // ordinal +2 (CFA)
    *     3. require_recipient(obs)                 // ordinal +3
    *
    * Execution order, per apply_context::exec():
    *     self -> notifications -> CFA inlines -> regular inlines.
    *
    * So the chain runs: source self, obs notification, CFA noop on cfat,
    * regular noop on inlt. Trace shape:
    *
    *     index 0: source self          ord=1 gseq=g0
    *     index 1: regular noop on inlt ord=2 gseq=g0+3   (LAST executed)
    *     index 2: CFA noop on cfat     ord=3 gseq=g0+2
    *     index 3: notification on obs  ord=4 gseq=g0+1   (FIRST after self)
    *
    *                            source body (mixedord)
    *                                 |
    *                                 v
    *     +-----------------------+ phase 1
    *     | self  (ord 1)         |  gseq=g0
    *     +-----------------------+
    *                                 |
    *                                 v
    *     +-----------------------+ phase 2 (notifications)
    *     | obs notify  (ord 4)   |  gseq=g0+1   <-- scheduled LAST,  ran 2nd
    *     +-----------------------+
    *                                 |
    *                                 v
    *     +-----------------------+ phase 3 (CFA inlines)
    *     | cfa noop    (ord 3)   |  gseq=g0+2
    *     +-----------------------+
    *                                 |
    *                                 v
    *     +-----------------------+ phase 4 (regular inlines)
    *     | inline noop (ord 2)   |  gseq=g0+3   <-- scheduled FIRST, ran LAST
    *     +-----------------------+
    *
    *     action_traces VECTOR order:  [ self, inline, cfa, notify ]
    *     action_traces EXEC   order:  [ self, notify, cfa, inline ]  (tail reversed)
    *
    * This is a TRIPLE divergence: every non-self trace's gseq disagrees
    * with its ordinal. It also encodes the (often forgotten) invariant
    * that CFA inlines run BEFORE regular inlines, never alongside them.
    */
   [[sysio::action]] void mixedord(sysio::name obs, sysio::name inlt, sysio::name cfat);

   /**
    * @brief Notification handler for notiffire.
    *
    * When the source action notiffire calls require_recipient(obs),
    * the chain dispatches THIS handler on obs. From here, send_inline(noop)
    * is queued back onto the SHARED apply_context's _inline_actions vector,
    * which is processed only after every sibling notification has run.
    */
   [[sysio::on_notify("*::notiffire")]] void on_notiffire(sysio::name obs);

   using noop_action     = sysio::action_wrapper<"noop"_n,     &action_order_test::noop>;
   using notifyme_action = sysio::action_wrapper<"notifyme"_n, &action_order_test::notifyme>;
};
