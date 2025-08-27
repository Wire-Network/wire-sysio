#include "restrict_action_test.hpp"

using namespace sysio;

void restrict_action_test::noop( ) {

}

void restrict_action_test::sendinline( name authorizer ) {
   action(
      permission_level{authorizer,"active"_n},
      get_self(),
      "noop"_n,
      std::make_tuple()
   ).send();
}

void restrict_action_test::notifyinline( name acctonotify, name authorizer ) {
   require_recipient(acctonotify);
}

void restrict_action_test::on_notify_inline( name acctonotify, name authorizer ) {
   sendinline(authorizer);
}
