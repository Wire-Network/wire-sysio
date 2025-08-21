#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/webassembly/sys-vm.hpp>
#include <sysio/chain/wasm_interface.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/producer_schedule.hpp>
#include <sysio/chain/exceptions.hpp>
#include <boost/core/ignore_unused.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/wasm_interface_private.hpp>
#include <sysio/chain/wasm_sysio_validation.hpp>
#include <sysio/chain/wasm_sysio_injection.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/protocol_state_object.hpp>
#include <sysio/chain/account_object.hpp>
#include <fc/exception/exception.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/sha1.hpp>
#include <fc/io/raw.hpp>

#include <softfloat.hpp>
#include <compiler_builtins.hpp>
#include <boost/asio.hpp>
#include <fstream>
#include <string.h>

#if defined(SYSIO_SYS_VM_RUNTIME_ENABLED) || defined(SYSIO_SYS_VM_JIT_RUNTIME_ENABLED)
#include <sysio/vm/allocator.hpp>
#endif

namespace sysio { namespace chain {

   wasm_interface::wasm_interface(vm_type vm, vm_oc_enable sysvmoc_tierup, const chainbase::database& d, const std::filesystem::path data_dir, const sysvmoc::config& sysvmoc_config, bool profile)
     : sysvmoc_tierup(sysvmoc_tierup), my( new wasm_interface_impl(vm, sysvmoc_tierup, d, data_dir, sysvmoc_config, profile) ) {}

   wasm_interface::~wasm_interface() {}

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   void wasm_interface::init_thread_local_data() {
      // OC tierup and OC runtime are mutually exclusive
      if (my->sysvmoc) {
         my->sysvmoc->init_thread_local_data();
      } else if (my->wasm_runtime_time == wasm_interface::vm_type::sys_vm_oc && my->runtime_interface) {
         my->runtime_interface->init_thread_local_data();
      }
   }
#endif

   void wasm_interface::validate(const controller& control, const bytes& code) {
      const auto& pso = control.db().get<protocol_state_object>();

      const auto& gpo = control.get_global_properties();
      webassembly::sys_vm_runtime::validate( code, gpo.wasm_configuration, pso.whitelisted_intrinsics );

      //there are a couple opportunties for improvement here--
      //Easy: Cache the Module created here so it can be reused for instantiaion
      //Hard: Kick off instantiation in a separate thread at this location
   }

   void wasm_interface::code_block_num_last_used(const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version, const uint32_t& block_num) {
      my->code_block_num_last_used(code_hash, vm_type, vm_version, block_num);
   }

   void wasm_interface::current_lib(const uint32_t lib) {
      my->current_lib(lib);
   }

   void wasm_interface::apply( const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version, apply_context& context ) {
      if (substitute_apply && substitute_apply(code_hash, vm_type, vm_version, context))
         return;
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
      if (my->sysvmoc && (sysvmoc_tierup == wasm_interface::vm_oc_enable::oc_all || context.should_use_sys_vm_oc())) {
         const chain::sysvmoc::code_descriptor* cd = nullptr;
         chain::sysvmoc::code_cache_base::get_cd_failure failure = chain::sysvmoc::code_cache_base::get_cd_failure::temporary;
         try {
            const bool high_priority = context.get_receiver().prefix() == chain::config::system_account_name;
            cd = my->sysvmoc->cc.get_descriptor_for_code(high_priority, code_hash, vm_version, context.control.is_write_window(), failure);
            if (test_disable_tierup)
               cd = nullptr;
         } catch (...) {
            // swallow errors here, if SYS VM OC has gone in to the weeds we shouldn't bail: continue to try and run baseline
            // In the future, consider moving bits of SYS VM that can fire exceptions and such out of this call path
            static bool once_is_enough;
            if (!once_is_enough)
               elog("SYS VM OC has encountered an unexpected failure");
            once_is_enough = true;
         }
         if (cd) {
            if (!context.is_applying_block()) // read_only_trx_test.py looks for this log statement
               tlog("${a} speculatively executing ${h} with sys vm oc", ("a", context.get_receiver())("h", code_hash));
            my->sysvmoc->exec->execute(*cd, *my->sysvmoc->mem, context);
            return;
         }
      }
#endif

      my->get_instantiated_module(code_hash, vm_type, vm_version, context.trx_context)->apply(context);
   }

   bool wasm_interface::is_code_cached(const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) const {
      return my->is_code_cached(code_hash, vm_type, vm_version);
   }

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   bool wasm_interface::is_sys_vm_oc_enabled() const {
      return my->is_sys_vm_oc_enabled();
   }
#endif

   wasm_instantiated_module_interface::~wasm_instantiated_module_interface() = default;
   wasm_runtime_interface::~wasm_runtime_interface() = default;

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
   thread_local std::unique_ptr<sysvmoc::executor> wasm_interface_impl::sysvmoc_tier::exec{};
   thread_local std::unique_ptr<sysvmoc::memory>   wasm_interface_impl::sysvmoc_tier::mem{};
#endif

std::istream& operator>>(std::istream& in, wasm_interface::vm_type& runtime) {
   std::string s;
   in >> s;
   if (s == "sys-vm")
      runtime = sysio::chain::wasm_interface::vm_type::sys_vm;
   else if (s == "sys-vm-jit")
      runtime = sysio::chain::wasm_interface::vm_type::sys_vm_jit;
   else if (s == "sys-vm-oc")
      runtime = sysio::chain::wasm_interface::vm_type::sys_vm_oc;
   else
      in.setstate(std::ios_base::failbit);
   return in;
}

} } /// sysio::chain
