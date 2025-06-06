#pragma once
#include <vector>
#include <memory>

namespace IR {
  struct Module;
}

namespace sysio { namespace chain {

class apply_context;

class wasm_instantiated_module_interface {
   public:
      virtual void apply(apply_context& context) = 0;

      virtual ~wasm_instantiated_module_interface();
};

class wasm_runtime_interface {
   public:
      virtual std::unique_ptr<wasm_instantiated_module_interface> instantiate_module(const char* code_bytes, size_t code_size,
                                                                                     const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) = 0;

      virtual ~wasm_runtime_interface();

      // sysvmoc_runtime needs this
      virtual void init_thread_local_data() {};
};

}}
