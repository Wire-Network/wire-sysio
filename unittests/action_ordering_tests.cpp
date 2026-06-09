/**
 * @file action_ordering_tests.cpp
 *
 * @brief Regression tests pinning down EOSIO-style action_trace ordering.
 *
 * What the chain guarantees (see libraries/chain/apply_context.cpp::exec):
 *
 *   Within a single apply_context, actions run in four fixed phases:
 *
 *     phase 1: the receiver's own action body         ("self" apply)
 *     phase 2: every require_recipient() in FIFO      (notification phase)
 *     phase 3: every send_context_free_inline() FIFO  (CFA inline phase)
 *     phase 4: every send_inline() in FIFO            (regular inline phase)
 *
 *     +--------+    +---------------+    +-------------+    +-----------------+
 *     |  self  | -> | notifications | -> | CFA inlines | -> | regular inlines |
 *     |        |    |     FIFO      |    |    FIFO     |    |      FIFO       |
 *     +--------+    +---------------+    +-------------+    +-----------------+
 *      phase 1         phase 2              phase 3              phase 4
 *
 * Two distinct counters describe a trace:
 *
 *   action_ordinal   - SCHEDULE order. Assigned the moment a host call
 *                      (require_recipient / send_*_inline) enqueues a new
 *                      action. action_traces is indexed by action_ordinal,
 *                      so action_traces[0..N] is in SCHEDULE order, NEVER
 *                      execution order.
 *
 *   receipt.global_sequence - EXECUTION order. Assigned only when the
 *                             action's receipt is built, which happens
 *                             AFTER that action has actually run.
 *
 * Whenever a contract body intermixes require_recipient() and send_inline()
 * within a single action, these two counters diverge. A consumer that
 * flattens action_traces and assumes the vector order is execution order
 * will misread every such trace. This was the class of confusion that
 * the vaults.sx 2021 incident (~$13M loss) exploited - the attacker relied
 * on observers mis-attributing the order of effects produced by a single
 * transaction.
 *
 * The mitigation is mechanical, and these tests pin it down:
 *
 *   - Contract authors: do not assume your notification handler sees the
 *     post-inline state. Inlines you queue from your action body run AFTER
 *     every notification you emit, including notifications you emit AFTER
 *     the send_inline call (phase ordering, not call ordering).
 *
 *   - History / indexer authors: never iterate action_traces in index order
 *     when you mean execution order. Sort by receipt.global_sequence, or
 *     walk the (creator_action_ordinal, action_ordinal) tree explicitly.
 *
 * Six focused cases are covered here. A separate, monolithic test in
 * api_tests.cpp (action_ordinal_test) covers the same rules across a more
 * elaborate fixture; the cases below complement that with clean,
 * documented invariants and the CFA-inline coverage that test lacks.
 *
 * Each case is instantiated for every tester in `validating_testers` so the
 * trace shape produced by block production is also exercised through the
 * replay path of a validating node.
 *
 * See unittests/test-contracts/action_order_test/action_order_test.hpp for
 * an "all cases at a glance" trace-tree summary in the EOSIO PR #6897
 * nested-arrow style (one diagram per case, showing both creation-tree
 * shape and execution-order bracket labels in a single view).
 */
#include <boost/test/unit_test.hpp>

#include <fc/variant_object.hpp>

#include <sysio/chain/transaction.hpp>
#include <sysio/testing/tester.hpp>

#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using mvo = fc::mutable_variant_object;

BOOST_AUTO_TEST_SUITE(action_ordering_tests)

namespace {

/**
 * @brief Common fixture: four accounts wired up for ordering tests.
 *
 *   source - originates the action; has the contract.
 *   obs    - notification recipient; has the contract so its on_notify
 *            handler (used by Case 2) can dispatch.
 *   inlt   - inline recipient; has the contract so nested-case inlines
 *            can run real contract logic (notifyme) on it.
 *   cfat   - CFA-inline recipient; intentionally has NO contract. The
 *            CFA noop trace is still recorded; no wasm runs.
 *
 * Parameterised on the tester type so each test case can be instantiated
 * via BOOST_AUTO_TEST_CASE_TEMPLATE against validating_testers, picking up
 * replay-validation coverage as a side effect.
 */
template <typename TesterT>
struct ordering_fixture {
   TesterT chain;
   account_name source = "alice"_n;
   account_name obs    = "bob"_n;
   account_name inlt   = "carol"_n;
   account_name cfat   = "dave"_n;

   ordering_fixture() {
      chain.create_accounts({source, obs, inlt, cfat});
      chain.produce_block();
      chain.set_code(source, test_contracts::action_order_test_wasm());
      chain.set_abi(source, test_contracts::action_order_test_abi());
      chain.set_code(obs, test_contracts::action_order_test_wasm());
      chain.set_abi(obs, test_contracts::action_order_test_abi());
      chain.set_code(inlt, test_contracts::action_order_test_wasm());
      chain.set_abi(inlt, test_contracts::action_order_test_abi());
      chain.produce_block();
   }
};

/**
 * @brief Tuple-of-interest extracted from each action_trace for clean asserts.
 *
 * The five fields here are the ones a history consumer typically wants.
 * Bundling them this way makes the BOOST_CHECK lines below read like the
 * doc-comment tables in action_order_test.hpp.
 */
struct trace_row {
   uint32_t      ordinal;       // schedule order
   uint64_t      gseq;          // execution order (from receipt)
   account_name  receiver;
   account_name  act_account;
   action_name   act_name;
};

std::vector<trace_row> tabulate(const transaction_trace_ptr& trace) {
   std::vector<trace_row> rows;
   rows.reserve(trace->action_traces.size());
   for (const auto& at : trace->action_traces) {
      BOOST_REQUIRE(at.receipt.has_value());
      rows.push_back({
         static_cast<uint32_t>(at.action_ordinal),
         at.receipt->global_sequence,
         at.receiver,
         at.act.account,
         at.act.name
      });
   }
   return rows;
}

} // namespace

// ---------------------------------------------------------------------------
// Case 1: a single self-contained action produces a single action_trace.
// Baseline - if this fails, everything else below is suspect.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE_TEMPLATE(simple_self_only, T, validating_testers) try {
   ordering_fixture<T> f;
   auto trace = f.chain.push_action(f.source, "noop"_n, f.source, mvo());
   BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);

   const auto& at = trace->action_traces[0];
   BOOST_CHECK_EQUAL(static_cast<int>(at.action_ordinal), 1);
   BOOST_CHECK_EQUAL(at.receiver, f.source);
   BOOST_CHECK_EQUAL(at.act.account, f.source);
   BOOST_CHECK_EQUAL(at.act.name, "noop"_n);
   BOOST_REQUIRE(at.receipt.has_value());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Case 2: notification-fires-inline.
//
// Source's body does require_recipient(obs). When obs runs its on_notify
// handler, that handler calls send_inline(noop) targeting itself. The
// resulting inline lands in the SHARED _inline_actions queue belonging to
// source's apply_context, NOT in some inner queue local to the handler.
// It therefore runs after the notification phase, with ordinal == 3.
//
// Trace shape:
//   [0] ordinal=1 gseq=g0   self apply on source (notiffire)
//   [1] ordinal=2 gseq=g0+1 notification on obs  (notiffire)
//   [2] ordinal=3 gseq=g0+2 inline noop on obs   (handler-scheduled inline)
//
// schedule order == execution order here, but the LESSON is that scheduling
// from inside a handler does NOT give you priority over sibling work.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE_TEMPLATE(notification_handler_inline_queues_after_siblings,
                              T, validating_testers) try {
   ordering_fixture<T> f;
   auto trace = f.chain.push_action(f.source, "notiffire"_n, f.source,
                                    mvo()("obs", f.obs.to_string()));
   auto rows = tabulate(trace);
   BOOST_REQUIRE_EQUAL(rows.size(), 3u);

   BOOST_CHECK_EQUAL(rows[0].ordinal, 1u);
   BOOST_CHECK_EQUAL(rows[0].receiver, f.source);
   BOOST_CHECK_EQUAL(rows[0].act_name, "notiffire"_n);

   BOOST_CHECK_EQUAL(rows[1].ordinal, 2u);
   BOOST_CHECK_EQUAL(rows[1].receiver, f.obs);
   BOOST_CHECK_EQUAL(rows[1].act_account, f.source);
   BOOST_CHECK_EQUAL(rows[1].act_name, "notiffire"_n);
   BOOST_CHECK_EQUAL(rows[1].gseq, rows[0].gseq + 1);

   BOOST_CHECK_EQUAL(rows[2].ordinal, 3u);
   BOOST_CHECK_EQUAL(rows[2].receiver, f.obs);
   BOOST_CHECK_EQUAL(rows[2].act_account, f.obs);
   BOOST_CHECK_EQUAL(rows[2].act_name, "noop"_n);
   BOOST_CHECK_EQUAL(rows[2].gseq, rows[0].gseq + 2);
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Case 3 (HEADLINE): schedule order diverges from execution order.
//
// Source's body schedules an inline FIRST (ordinal 2), then a notification
// (ordinal 3). The chain runs the notification before the inline. Therefore
// action_traces[1] (the inline, vector position 2) has a HIGHER
// global_sequence than action_traces[2] (the notification, vector position 3).
//
// Concretely:
//
//     action_traces vector     receipt.global_sequence
//     --------------------     -----------------------
//     [0] self    ord=1        g0          (1st to run)
//     [1] inline  ord=2        g0 + 2      (3rd to run)
//     [2] notify  ord=3        g0 + 1      (2nd to run)
//
// rows[1].gseq > rows[2].gseq - vector order and execution order swap.
// Anyone iterating action_traces in vector order and assuming "earlier index
// means ran earlier" reads the wrong story here. This is the trap class that
// the vaults.sx 2021 attack relied on.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE_TEMPLATE(inline_before_notification_diverges,
                              T, validating_testers) try {
   ordering_fixture<T> f;
   auto trace = f.chain.push_action(f.source, "inlnotify"_n, f.source,
                                    mvo()("obs",  f.obs.to_string())
                                         ("inlt", f.inlt.to_string()));
   auto rows = tabulate(trace);
   BOOST_REQUIRE_EQUAL(rows.size(), 3u);

   // [0] self
   BOOST_CHECK_EQUAL(rows[0].ordinal, 1u);
   BOOST_CHECK_EQUAL(rows[0].receiver, f.source);
   BOOST_CHECK_EQUAL(rows[0].act_name, "inlnotify"_n);

   // [1] inline - scheduled first (ordinal 2), but executed LAST (gseq +2)
   BOOST_CHECK_EQUAL(rows[1].ordinal, 2u);
   BOOST_CHECK_EQUAL(rows[1].receiver, f.inlt);
   BOOST_CHECK_EQUAL(rows[1].act_account, f.inlt);
   BOOST_CHECK_EQUAL(rows[1].act_name, "noop"_n);
   BOOST_CHECK_EQUAL(rows[1].gseq, rows[0].gseq + 2);

   // [2] notification - scheduled second (ordinal 3), but executed first (gseq +1)
   BOOST_CHECK_EQUAL(rows[2].ordinal, 3u);
   BOOST_CHECK_EQUAL(rows[2].receiver, f.obs);
   BOOST_CHECK_EQUAL(rows[2].act_account, f.source);
   BOOST_CHECK_EQUAL(rows[2].act_name, "inlnotify"_n);
   BOOST_CHECK_EQUAL(rows[2].gseq, rows[0].gseq + 1);

   // The divergence assertion in one line: the inline's global_sequence is
   // GREATER than the notification's, even though the inline appears EARLIER
   // in the action_traces vector.
   BOOST_CHECK_GT(rows[1].gseq, rows[2].gseq);
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Case 4 (sanity contrast): same actions, opposite scheduling.
// Notification scheduled first => ordinal order and gseq order agree.
// Pairs with Case 3 so the divergence stands out by comparison.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE_TEMPLATE(notification_before_inline_agrees,
                              T, validating_testers) try {
   ordering_fixture<T> f;
   auto trace = f.chain.push_action(f.source, "notifyinl"_n, f.source,
                                    mvo()("obs",  f.obs.to_string())
                                         ("inlt", f.inlt.to_string()));
   auto rows = tabulate(trace);
   BOOST_REQUIRE_EQUAL(rows.size(), 3u);

   BOOST_CHECK_EQUAL(rows[0].ordinal, 1u);
   BOOST_CHECK_EQUAL(rows[0].receiver, f.source);

   BOOST_CHECK_EQUAL(rows[1].ordinal, 2u);
   BOOST_CHECK_EQUAL(rows[1].receiver, f.obs);
   BOOST_CHECK_EQUAL(rows[1].act_name, "notifyinl"_n);
   BOOST_CHECK_EQUAL(rows[1].gseq, rows[0].gseq + 1);

   BOOST_CHECK_EQUAL(rows[2].ordinal, 3u);
   BOOST_CHECK_EQUAL(rows[2].receiver, f.inlt);
   BOOST_CHECK_EQUAL(rows[2].act_name, "noop"_n);
   BOOST_CHECK_EQUAL(rows[2].gseq, rows[0].gseq + 2);
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Case 5 (nested): notification + inline, where the inline body itself
// triggers a notification at its OWN apply_context depth.
//
// The inner notification belongs to the inline's apply_context, not the
// outer one - so it appears in the trace as a child of the inline, not a
// sibling of the outer notification.
//
// Trace shape:
//   [0] ord=1 gseq=g0   source self (nestedcall)
//   [1] ord=2 gseq=g0+1 outer notification on obs (nestedcall)
//   [2] ord=3 gseq=g0+2 inline notifyme on inlt
//   [3] ord=4 gseq=g0+3 inner notification on obs (notifyme, from inlt)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE_TEMPLATE(nested_inline_fires_inner_notification,
                              T, validating_testers) try {
   ordering_fixture<T> f;
   auto trace = f.chain.push_action(f.source, "nestedcall"_n, f.source,
                                    mvo()("obs",  f.obs.to_string())
                                         ("inlt", f.inlt.to_string()));
   auto rows = tabulate(trace);
   BOOST_REQUIRE_EQUAL(rows.size(), 4u);

   // [0] outer self
   BOOST_CHECK_EQUAL(rows[0].ordinal, 1u);
   BOOST_CHECK_EQUAL(rows[0].receiver, f.source);
   BOOST_CHECK_EQUAL(rows[0].act_name, "nestedcall"_n);

   // [1] outer notification: source -> obs, action = nestedcall
   BOOST_CHECK_EQUAL(rows[1].ordinal, 2u);
   BOOST_CHECK_EQUAL(rows[1].receiver, f.obs);
   BOOST_CHECK_EQUAL(rows[1].act_account, f.source);
   BOOST_CHECK_EQUAL(rows[1].act_name, "nestedcall"_n);
   BOOST_CHECK_EQUAL(rows[1].gseq, rows[0].gseq + 1);

   // [2] inline self: notifyme runs on inlt with code=inlt
   BOOST_CHECK_EQUAL(rows[2].ordinal, 3u);
   BOOST_CHECK_EQUAL(rows[2].receiver, f.inlt);
   BOOST_CHECK_EQUAL(rows[2].act_account, f.inlt);
   BOOST_CHECK_EQUAL(rows[2].act_name, "notifyme"_n);
   BOOST_CHECK_EQUAL(rows[2].gseq, rows[0].gseq + 2);

   // [3] inner notification: inlt -> obs, action = notifyme
   BOOST_CHECK_EQUAL(rows[3].ordinal, 4u);
   BOOST_CHECK_EQUAL(rows[3].receiver, f.obs);
   BOOST_CHECK_EQUAL(rows[3].act_account, f.inlt);
   BOOST_CHECK_EQUAL(rows[3].act_name, "notifyme"_n);
   BOOST_CHECK_EQUAL(rows[3].gseq, rows[0].gseq + 3);
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Case 6 (CFA): CFA inlines run BEFORE regular inlines, regardless of the
// order in which they were scheduled.
//
// Body schedules: regular_inline -> CFA_inline -> require_recipient.
// Chain runs phases in fixed order:
//   self -> notifications -> CFA inlines -> regular inlines.
//
// All three non-self traces have global_sequence values that DISAGREE with
// their action_ordinal. This is the strongest divergence in the suite.
//
// Trace shape:
//   [0] ord=1 gseq=g0   source self (mixedord)
//   [1] ord=2 gseq=g0+3 regular noop on inlt   (LAST executed)
//   [2] ord=3 gseq=g0+2 CFA noop on cfat
//   [3] ord=4 gseq=g0+1 notification on obs    (FIRST executed after self)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE_TEMPLATE(cfa_runs_before_regular_inline,
                              T, validating_testers) try {
   ordering_fixture<T> f;
   auto trace = f.chain.push_action(f.source, "mixedord"_n, f.source,
                                    mvo()("obs",  f.obs.to_string())
                                         ("inlt", f.inlt.to_string())
                                         ("cfat", f.cfat.to_string()));
   auto rows = tabulate(trace);
   BOOST_REQUIRE_EQUAL(rows.size(), 4u);

   // [0] self
   BOOST_CHECK_EQUAL(rows[0].ordinal, 1u);
   BOOST_CHECK_EQUAL(rows[0].receiver, f.source);
   BOOST_CHECK_EQUAL(rows[0].act_name, "mixedord"_n);

   // [1] regular inline (scheduled first, executed last)
   BOOST_CHECK_EQUAL(rows[1].ordinal, 2u);
   BOOST_CHECK_EQUAL(rows[1].receiver, f.inlt);
   BOOST_CHECK_EQUAL(rows[1].act_account, f.inlt);
   BOOST_CHECK_EQUAL(rows[1].act_name, "noop"_n);
   BOOST_CHECK_EQUAL(rows[1].gseq, rows[0].gseq + 3);

   // [2] CFA inline
   BOOST_CHECK_EQUAL(rows[2].ordinal, 3u);
   BOOST_CHECK_EQUAL(rows[2].receiver, f.cfat);
   BOOST_CHECK_EQUAL(rows[2].act_account, f.cfat);
   BOOST_CHECK_EQUAL(rows[2].act_name, "noop"_n);
   BOOST_CHECK_EQUAL(rows[2].gseq, rows[0].gseq + 2);

   // [3] notification (scheduled last, executed first after self)
   BOOST_CHECK_EQUAL(rows[3].ordinal, 4u);
   BOOST_CHECK_EQUAL(rows[3].receiver, f.obs);
   BOOST_CHECK_EQUAL(rows[3].act_account, f.source);
   BOOST_CHECK_EQUAL(rows[3].act_name, "mixedord"_n);
   BOOST_CHECK_EQUAL(rows[3].gseq, rows[0].gseq + 1);

   // Phase-order invariants as plain inequalities.
   // (notification phase) < (CFA phase) < (regular inline phase).
   BOOST_CHECK_LT(rows[3].gseq, rows[2].gseq);
   BOOST_CHECK_LT(rows[2].gseq, rows[1].gseq);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
