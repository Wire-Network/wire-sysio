#pragma once

#include <signal.h>

namespace sysio::chain::sysvmoc {

/// Retain a previous signal action's calling convention when an OC fault belongs to another handler.
class chained_signal_handler {
public:
   chained_signal_handler() = default;

   /// Decode SA_SIGINFO once, retaining only a callable predecessor of the corresponding type.
   explicit chained_signal_handler(const struct sigaction& action) noexcept {
      if (action.sa_handler == SIG_IGN || action.sa_handler == SIG_DFL)
         return;
      if (action.sa_flags & SA_SIGINFO)
         siginfo_handler = action.sa_sigaction;
      else
         legacy_handler = action.sa_handler;
   }

   /// Invoke the predecessor with its original ABI; false leaves default fault handling to the caller.
   bool operator()(int signal, siginfo_t* info, void* context) const {
      if (siginfo_handler) {
         siginfo_handler(signal, info, context);
         return true;
      }
      if (legacy_handler) {
         legacy_handler(signal);
         return true;
      }
      return false;
   }

private:
   void (*siginfo_handler)(int, siginfo_t*, void*) = nullptr;
   void (*legacy_handler)(int) = nullptr;
};

} // namespace sysio::chain::sysvmoc
