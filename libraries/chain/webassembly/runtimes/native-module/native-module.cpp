#include <sysio/chain/webassembly/native-module/native-module.hpp>
#include <sysio/chain/webassembly/native-module/native_context_stack.hpp>
#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/exceptions.hpp>
#include <fc/log/logger.hpp>

namespace sysio::chain::webassembly::native_module {

void native_instantiated_module::apply(apply_context& context) {
   interface iface(context);
   native_context_stack::guard ctx_guard(&iface);

   apply_fun_.exec(context.get_receiver().to_uint64_t(),
                   context.get_action().account.to_uint64_t(),
                   context.get_action().name.to_uint64_t());
}

std::unique_ptr<wasm_instantiated_module_interface>
native_runtime::instantiate_module(const char* code_bytes, size_t code_size,
                                   const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version)
{
   auto so_path = code_dir() / (code_hash.str() + ".so");

   if (std::filesystem::exists(so_path)) {
      auto apply_fn = dynamic_loaded_function(so_path, "apply");
      return std::make_unique<native_instantiated_module>(std::move(apply_fn));
   }

   // No native .so for this contract — fall back to WASM runtime
   if (fallback_) {
      ilog("No native .so for code hash {}, falling back to WASM runtime", code_hash.str());
      return fallback_->instantiate_module(code_bytes, code_size, code_hash, vm_type, vm_version);
   }

   SYS_ASSERT(false, wasm_exception,
              "native module not found: {} (and no fallback runtime configured)", so_path.string());
}

} // sysio::chain::webassembly::native_module
