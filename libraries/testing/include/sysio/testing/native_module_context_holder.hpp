#pragma once

#include <sysio/chain/types.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace sysio::testing {

// Native module helpers for the testing framework.
// Manages symlinks in native_runtime::code_dir() and the registry
// mapping WASM code hashes to native .so file paths.
struct native_module_context_holder {
   // Register a native .so file for a given code hash.
   // Creates a symlink: <code_dir>/<code_hash>.so -> so_path
   static void register_native_so(const chain::digest_type& code_hash, const std::filesystem::path& so_path);

   // Register a native .so for a contract by providing its WASM bytes and .so path.
   // Hashes the WASM and stores the mapping for later auto-registration in set_code.
   static void register_native_contract(const std::vector<uint8_t>& wasm, const std::filesystem::path& so_path);

   // Called from set_code() to auto-register a .so if the WASM hash is in the registry.
   static void try_register_from_wasm(const std::vector<uint8_t>& wasm);

   // Register all known native contracts (called once at startup).
   static void init_native_contracts();
};

} // sysio::testing
