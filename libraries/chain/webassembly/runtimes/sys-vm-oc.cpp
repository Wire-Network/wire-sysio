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
         _sysvmoc_runtime(wr),
         _main_thread_id(std::this_thread::get_id())
      {

      }

      ~sysvmoc_instantiated_module() {
         _sysvmoc_runtime.cc.free_code(_code_hash, _vm_version);
      }

      bool is_main_thread() { return _main_thread_id == std::this_thread::get_id(); };

      void apply(apply_context& context) override {
         const code_descriptor* const cd = _sysvmoc_runtime.cc.get_descriptor_for_code_sync(_code_hash, _vm_version, context.control.is_write_window());
         SYS_ASSERT(cd, wasm_execution_error, "SYS VM OC instantiation failed");

         if ( is_main_thread() )
            _sysvmoc_runtime.exec.execute(*cd, _sysvmoc_runtime.mem, context);
         else
            _sysvmoc_runtime.exec_thread_local->execute(*cd, *_sysvmoc_runtime.mem_thread_local, context);
      }

      const digest_type              _code_hash;
      const uint8_t                  _vm_version;
      sysvmoc_runtime&               _sysvmoc_runtime;
      std::thread::id                _main_thread_id;
};

sysvmoc_runtime::sysvmoc_runtime(const std::filesystem::path data_dir, const sysvmoc::config& sysvmoc_config, const chainbase::database& db)
   : cc(data_dir, sysvmoc_config, db), exec(cc), mem(wasm_constraints::maximum_linear_memory/wasm_constraints::wasm_page_size) {
}

sysvmoc_runtime::~sysvmoc_runtime() {
}

std::unique_ptr<wasm_instantiated_module_interface> sysvmoc_runtime::instantiate_module(const char* code_bytes, size_t code_size,
                                                                                        const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) {
   return std::make_unique<sysvmoc_instantiated_module>(code_hash, vm_type, *this);
}

void sysvmoc_runtime::init_thread_local_data() {
   exec_thread_local = std::make_unique<sysvmoc::executor>(cc);
   mem_thread_local  = std::make_unique<sysvmoc::memory>(sysvmoc::memory::sliced_pages_for_ro_thread);
}

thread_local std::unique_ptr<sysvmoc::executor> sysvmoc_runtime::exec_thread_local{};
thread_local std::unique_ptr<sysvmoc::memory> sysvmoc_runtime::mem_thread_local{};

}}}}
