#pragma once

#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/exceptions.hpp>

namespace sysio::chain::webassembly::native_module {

// Thread-local context stack for native module intrinsic dispatch.
// When native_instantiated_module::apply() runs, it pushes its interface*
// onto this stack. The INTRINSIC_EXPORT functions dispatch through it.
struct native_context_stack {
   static void push(interface* iface) {
      stack_ = iface;
   }
   static void pop() {
      stack_ = nullptr;
   }
   static interface* current() {
      SYS_ASSERT(stack_, wasm_execution_error, "native intrinsic called outside of contract execution context");
      return stack_;
   }
   static inline thread_local interface* stack_ = nullptr;
};

} // sysio::chain::webassembly::native_module
