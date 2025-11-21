#pragma once
#include <sysio/chain/code_object.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain/whitelisted_intrinsics.hpp>
#include <sysio/chain/exceptions.hpp>
#include <functional>

namespace sysio { namespace chain {

   struct platform_timer;
   class apply_context;
   class wasm_runtime_interface;
   class controller;
   namespace sysvmoc { struct config; }

   struct wasm_exit {
      int32_t code = 0;
   };

   /**
    * @class wasm_interface
    *
    */
   class wasm_interface {
      public:
         enum class vm_type {
            sys_vm,
            sys_vm_jit,
            sys_vm_oc
         };

         //return string description of vm_type
         static std::string vm_type_string(vm_type vmtype) {
             switch (vmtype) {
             case vm_type::sys_vm:
                return "sys-vm";
             case vm_type::sys_vm_oc:
                return "sys-vm-oc";
             default:
                 return "sys-vm-jit";
             }
         }

         enum class vm_oc_enable {
            oc_auto,
            oc_all,
            oc_none
         };

         wasm_interface(vm_type vm, vm_oc_enable sysvmoc_tierup, const chainbase::database& d, platform_timer& main_thread_timer, const std::filesystem::path data_dir, const sysvmoc::config& sysvmoc_config, bool profile);
         ~wasm_interface();

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
         // initialize exec per thread
         void init_thread_local_data();

         // returns true if SYS VM OC is enabled
         bool is_sys_vm_oc_enabled() const;

         // return number of wasm execution interrupted by sys vm oc compile completing, used for testing
         uint64_t get_sys_vm_oc_compile_interrupt_count() const;
#endif

         //call before dtor to skip what can be minutes of dtor overhead with some runtimes; can cause leaks
         void indicate_shutting_down();

         //validates code -- does a WASM validation pass and checks the wasm against SYSIO specific constraints
         static void validate(const controller& control, const bytes& code);

         //indicate that a particular code probably won't be used after given block_num
         void code_block_num_last_used(const digest_type& code_hash, uint8_t vm_type, uint8_t vm_version,
                                       block_num_type first_used_block_num, block_num_type last_used_block_num);

         //indicate the current LIB. evicts old cache entries
         void current_lib(const uint32_t lib);

         //Calls apply or error on a given code
         void apply(const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version, apply_context& context);

         //Returns true if the code is cached
         bool is_code_cached(const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) const;

         // If substitute_apply is set, then apply calls it before doing anything else. If substitute_apply returns true,
         // then apply returns immediately. Provided function must be multi-thread safe.
         std::function<bool(const digest_type& code_hash, uint8_t vm_type, uint8_t vm_version, apply_context& context)> substitute_apply;

      private:
         unique_ptr<struct wasm_interface_impl> my;
   };

} } // sysio::chain

namespace sysio{ namespace chain {
   std::istream& operator>>(std::istream& in, wasm_interface::vm_type& runtime);
   inline std::ostream& operator<<(std::ostream& os, wasm_interface::vm_oc_enable t) {
      if (t == wasm_interface::vm_oc_enable::oc_auto) {
         os << "auto";
      } else if (t == wasm_interface::vm_oc_enable::oc_all) {
         os << "all";
      } else if (t == wasm_interface::vm_oc_enable::oc_none) {
         os << "none";
      }
      return os;
   }
}}

FC_REFLECT_ENUM( sysio::chain::wasm_interface::vm_type, (sys_vm)(sys_vm_jit)(sys_vm_oc) )
