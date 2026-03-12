#pragma once

#include <sysio/chain/webassembly/native-module/dynamic_loaded_function.hpp>
#include <sysio/chain/webassembly/native-module/native_context_stack.hpp>
#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/types.hpp>

#include <filesystem>
#include <map>

namespace sysio::chain::webassembly::native_module {

/// Overlay that routes specific contracts through native .so files
/// while letting all other contracts execute via the normal WASM runtime.
/// Used by both --native-contract in nodeop and unit tests.
///
/// Install via wasm_interface::substitute_apply:
///   overlay.load(code_hash, "/path/to/contract.so");
///   wasmif.substitute_apply = [&](auto&&... args) { return overlay(args...); };
class native_module_overlay {
public:
   /// Load a native .so for a given code_hash.  Throws on dlopen/dlsym failure.
   void load(const digest_type& code_hash, const std::filesystem::path& so_path) {
      modules_.emplace(code_hash, dynamic_loaded_function(so_path, "apply"));
   }

   /// substitute_apply-compatible callback.
   /// Returns true if the contract was handled natively, false to fall through to WASM.
   bool operator()(const digest_type& code_hash, uint8_t vm_type, uint8_t vm_version, apply_context& context) {
      auto it = modules_.find(code_hash);
      if (it == modules_.end())
         return false;

      interface iface(context);
      native_context_stack::guard ctx_guard(&iface);
      it->second.exec(context.get_receiver().to_uint64_t(),
                      context.get_action().account.to_uint64_t(),
                      context.get_action().name.to_uint64_t());
      return true;
   }

   bool empty() const { return modules_.empty(); }
   size_t size() const { return modules_.size(); }

private:
   std::map<digest_type, dynamic_loaded_function> modules_;
};

} // sysio::chain::webassembly::native_module
