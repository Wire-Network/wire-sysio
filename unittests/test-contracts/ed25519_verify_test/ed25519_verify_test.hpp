#pragma once
#include <sysio/sysio.hpp>
#include <vector>

using bytes = std::vector<char>;

/// Action serialization only: the intrinsic receives three independent byte spans.
struct ed25519_input {
   bytes signature;
   bytes public_key;
   bytes message;
};

/// Exercises the raw Ed25519 import through real WASM actions, without CDT crypto wrappers.
class [[sysio::contract]] ed25519_verify_test : public sysio::contract {
public:
   using sysio::contract::contract;

   /// Check one result, optionally using unaligned signature, key, and message buffers.
   [[sysio::action]] void verify(const ed25519_input& entry, int32_t expected, bool unaligned);
   /// Verify independent entries in WASM, stopping at the first failed signature.
   [[sysio::action]] void batch(const std::vector<ed25519_input>& entries, int32_t expected);
   /// Place the selected buffer at the memory boundary, optionally extending its length past the end.
   [[sysio::action]] void boundary(const ed25519_input& entry, uint32_t buffer, bool overrun);
   /// Pass a wrapping address range for each selected input.
   [[sysio::action]] void badptr(uint32_t buffer);
   /// Write a marker and repeatedly verify in WASM to exercise runtime timeout and rollback.
   [[sysio::action]] void stress(const ed25519_input& entry);
   /// Assert the marker written by stress was rolled back.
   [[sysio::action]] void clean();
};
