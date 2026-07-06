#include <sysio/chain/webassembly/sys-vm-oc.hpp>
#include <sysio/chain/wasm_sysio_constraints.hpp>
#include <sysio/chain/wasm_sysio_injection.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/global_property_object.hpp>

#include <vector>
#include <iterator>

namespace sysio { namespace chain { namespace webassembly { namespace sysvmoc {

class sysvmoc_instantiated_module : public wasm_instantiated_module_interface {
   public:
      sysvmoc_instantiated_module(const digest_type& code_hash, const uint8_t& vm_version, sysvmoc_runtime& wr) :
         _code_hash(code_hash),
         _vm_version(vm_version),
         _sysvmoc_runtime(wr)
      {

      }

      ~sysvmoc_instantiated_module() {
         _sysvmoc_runtime.cc.free_code(_code_hash, _vm_version);
      }

      void apply(apply_context& context) override {
         sysio::chain::sysvmoc::code_cache_sync::mode m;
         m.whitelisted = context.is_sys_vm_oc_whitelisted();
         m.write_window = context.control.is_write_window();
         const code_descriptor* const cd = _sysvmoc_runtime.cc.get_descriptor_for_code_sync(m, context.get_receiver(), _code_hash, _vm_version);
         SYS_ASSERT(cd, wasm_execution_error, "SYS VM OC instantiation failed");

         _sysvmoc_runtime.exec_mem.get_executor().execute(*cd, _sysvmoc_runtime.exec_mem.get_memory(), context);
      }

      const digest_type              _code_hash;
      const uint8_t                  _vm_version;
      sysvmoc_runtime&               _sysvmoc_runtime;
};

sysvmoc_runtime::sysvmoc_runtime(const std::filesystem::path data_dir, const sysvmoc::config& sysvmoc_config, const chainbase::database& db)
   : cc(data_dir, sysvmoc_config, db), exec_mem(cc) {
}

sysvmoc_runtime::~sysvmoc_runtime() {
}

std::unique_ptr<wasm_instantiated_module_interface> sysvmoc_runtime::instantiate_module(const char* code_bytes, size_t code_size,
                                                                                        const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) {
   return std::make_unique<sysvmoc_instantiated_module>(code_hash, vm_type, *this);
}

void sysvmoc_runtime::init_thread_local_data() {
   exec_mem.init_thread_local_data();
}

}}}}
