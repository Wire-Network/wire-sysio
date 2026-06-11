#include "action_order_test.hpp"

using namespace sysio;

namespace {
   /**
    * @brief Build an inline action_wrapper instance with no authorizations.
    *
    * Inline actions normally carry the sender's permission_level. For these
    * ordering tests we only care about the trace shape, not auth checks, so
    * we pass an empty permission vector - the inline-action code path
    * accepts empty authorization (check_authorization has nothing to verify)
    * and the resulting trace is identical in shape.
    */
   template <typename Wrapper>
   Wrapper make_unauthed_wrapper(name code) {
      return Wrapper(code, std::vector<permission_level>{});
   }
} // namespace

void action_order_test::noop() {
   // Intentionally empty. The trace's action_receipt is still produced.
}

void action_order_test::notifyme(name obs) {
   require_recipient(obs);
}

void action_order_test::notiffire(name obs) {
   // Single host call: enqueue obs as a notification recipient. The
   // matching on_notify handler below queues a follow-on inline.
   require_recipient(obs);
}

void action_order_test::on_notiffire([[maybe_unused]] name obs) {
   // We are running inside the notification phase of the SHARED
   // apply_context belonging to the originating notiffire action.
   // send_inline(noop) lands in that shared _inline_actions queue and
   // will execute only AFTER every sibling notification finishes. The
   // inline targets get_self() (the notified receiver), so this trace
   // shows the inline executing on the notification recipient, not the
   // originator - matching the "B observes A and then acts" pattern.
   auto act = make_unauthed_wrapper<noop_action>(get_self());
   act.send();
}

void action_order_test::inlnotify(name obs, name inlt) {
   // HEADLINE: schedule the inline FIRST, then the notification.
   // Inline is queued in _inline_actions (regular inline phase, phase 4).
   // Notification is queued in _notified (notification phase, phase 2).
   // Result: notification runs first at execution time, but the action_trace
   // for the inline appears EARLIER in the action_traces vector because it
   // was scheduled (assigned its ordinal) first.
   auto inline_act = make_unauthed_wrapper<noop_action>(inlt);
   inline_act.send();
   require_recipient(obs);
}

void action_order_test::notifyinl(name obs, name inlt) {
   // Natural ordering: notification scheduled first, inline second.
   // Here ordinal order and global_sequence order agree, providing the
   // contrast case for the inlnotify divergence test.
   require_recipient(obs);
   auto inline_act = make_unauthed_wrapper<noop_action>(inlt);
   inline_act.send();
}

void action_order_test::nestedcall(name obs, name inlt) {
   // Outer scheduling: notification, then inline.
   // The inline itself does require_recipient(obs) inside notifyme; that
   // notification belongs to the INLINE'S apply_context, not this one,
   // so it shows up as an additional trace inside the inline's subtree.
   require_recipient(obs);
   auto inline_act = make_unauthed_wrapper<notifyme_action>(inlt);
   inline_act.send(obs);
}

void action_order_test::mixedord(name obs, name inlt, name cfat) {
   // Three host calls in this scheduling order:
   //   1. regular inline noop on inlt
   //   2. CFA inline    noop on cfat
   //   3. require_recipient(obs)
   //
   // Despite that, execution runs phases in fixed order:
   //   self -> notifications -> CFA inlines -> regular inlines.
   // So the obs notification (last scheduled) runs FIRST after self,
   // the CFA noop runs second, the regular noop runs LAST.
   auto regular_inline = make_unauthed_wrapper<noop_action>(inlt);
   regular_inline.send();
   auto cfa_inline = make_unauthed_wrapper<noop_action>(cfat);
   cfa_inline.send_context_free();
   require_recipient(obs);
}
