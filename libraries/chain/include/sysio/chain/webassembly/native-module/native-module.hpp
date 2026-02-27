#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/webassembly/runtime_interface.hpp>
#include <sysio/chain/webassembly/native-module/dynamic_loaded_function.hpp>

#include <filesystem>

namespace sysio::chain::webassembly::native_module {

/// Instantiated module for the native-module WASM runtime. Wraps a dlopen'd
/// contract .so and invokes its native `apply(receiver, code, action)` function.
/// The native .so resolves blockchain intrinsics (db_store_i64, require_auth, etc.)
/// at load time against symbols exported from the host test executable.
class native_instantiated_module : public wasm_instantiated_module_interface {
public:
   explicit native_instantiated_module(dynamic_loaded_function apply_fn)
      : apply_fun_(std::move(apply_fn)) {}

   void apply(apply_context& context) override;

private:
   dynamic_loaded_function apply_fun_;
};

class native_runtime : public wasm_runtime_interface {
public:
   // Well-known directory for native contract .so files (symlinks).
   // Both the runtime and the tester use this path.
   static const std::filesystem::path& code_dir() {
      static const std::filesystem::path dir = std::filesystem::temp_directory_path() / "wire-sysio-native-contracts";
      return dir;
   }

   // fallback_runtime is used for contracts without a native .so
   explicit native_runtime(std::unique_ptr<wasm_runtime_interface> fallback_runtime = nullptr)
      : fallback_(std::move(fallback_runtime)) {}

   std::unique_ptr<wasm_instantiated_module_interface>
   instantiate_module(const char* code_bytes, size_t code_size,
                      const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) override;

private:
   std::unique_ptr<wasm_runtime_interface> fallback_;
};

} // sysio::chain::webassembly::native_module
