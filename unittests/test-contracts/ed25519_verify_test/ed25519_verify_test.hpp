#pragma once
#include <sysio/sysio.hpp>

#include <vector>

using bytes = std::vector<char>;

/// Exercises the raw Ed25519 import through real WASM actions, without CDT crypto wrappers.
class [[sysio::contract]] ed25519_verify_test : public sysio::contract {
public:
   using sysio::contract::contract;

   /// Check the intrinsic result and optionally pass deliberately unaligned record bytes.
   [[sysio::action]]
   void verify(const bytes& entries, uint32_t count, bool expected, bool unaligned);

   /// Pass a record at the exact end of WASM memory, optionally extending the span past the end.
   [[sysio::action]]
   void boundary(const bytes& entry, bool overrun);

   /// Pass a wrapping address range to verify that runtime memory errors remain traps.
   [[sysio::action]]
   void badptr();

   /// Build a large batch, write a marker, and invoke the intrinsic once to exercise rollback on timeout.
   [[sysio::action]]
   void stress(const bytes& empty_record);

   /// Assert the marker written by stress was rolled back.
   [[sysio::action]]
   void clean();
};
