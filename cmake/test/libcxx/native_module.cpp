#include "native_probe.hpp"

namespace {
/// Report stack unwinding through the loaded module back to its host.
struct unwind_guard {
   bool* destroyed;
   ~unwind_guard() { *destroyed = true; }
};
}

/// Throw locally, unwind a host callback, or return a module-owned allocation.
extern "C" std::string* native_probe(native_callback callback, bool* destroyed) {
   if (!destroyed)
      throw native_probe_error();
   unwind_guard guard{destroyed};
   if (callback)
      callback();
   return new std::string(native_probe_message);
}
