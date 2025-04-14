#pragma once

#include <sysio/chain/wasm_interface.hpp>
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
#include <sysio/chain/webassembly/sys-vm-oc.hpp>
#else
#define _REGISTER_SYSVMOC_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)
#endif
#include <sysio/chain/webassembly/runtime_interface.hpp>
#include <sysio/chain/wasm_sysio_injection.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/code_object.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/exceptions.hpp>
#include <fc/scoped_exit.hpp>

#include "IR/Module.h"
#include "Platform/Platform.h"
#include "WAST/WAST.h"
#include "IR/Validate.h"

#include <sysio/chain/webassembly/sys-vm.hpp>
#include <sysio/vm/allocator.hpp>

#include <mutex>

using namespace fc;
using namespace sysio::chain::webassembly;
using namespace IR;

using boost::multi_index_container;

namespace sysio { namespace chain {

   namespace sysvmoc { struct config; }

   struct wasm_interface_impl {
      struct wasm_cache_entry {
         digest_type                                          code_hash;
         uint32_t                                             last_block_num_used;
         std::unique_ptr<wasm_instantiated_module_interface>  module;
         uint8_t                                              vm_type = 0;
         uint8_t                                              vm_version = 0;
      };
      struct by_hash;
      struct by_last_block_num;

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
struct sysvmoc_tier {
   // Called from main thread
   sysvmoc_tier(const std::filesystem::path& d, const sysvmoc::config& c, const chainbase::database& db)
      : cc(d, c, db) {
      // Construct exec and mem for the main thread
      exec = std::make_unique<sysvmoc::executor>(cc);
      mem  = std::make_unique<sysvmoc::memory>(wasm_constraints::maximum_linear_memory/wasm_constraints::wasm_page_size);
   }

   // Called from read-only threads
   void init_thread_local_data() {
      exec = std::make_unique<sysvmoc::executor>(cc);
      mem  = std::make_unique<sysvmoc::memory>(sysvmoc::memory::sliced_pages_for_ro_thread);
   }

   sysvmoc::code_cache_async cc;

   // Each thread requires its own exec and mem. Defined in wasm_interface.cpp
   thread_local static std::unique_ptr<sysvmoc::executor> exec;
   thread_local static std::unique_ptr<sysvmoc::memory>   mem;
};
#endif

      wasm_interface_impl(wasm_interface::vm_type vm, wasm_interface::vm_oc_enable sysvmoc_tierup, const chainbase::database& d,
                          const std::filesystem::path data_dir, const sysvmoc::config& sysvmoc_config, bool profile)
         : db(d)
         , wasm_runtime_time(vm)
      {
#ifdef SYSIO_SYS_VM_RUNTIME_ENABLED
         if(vm == wasm_interface::vm_type::sys_vm)
            runtime_interface = std::make_unique<webassembly::sys_vm_runtime::sys_vm_runtime<sysio::vm::interpreter>>();
#endif
#ifdef SYSIO_SYS_VM_JIT_RUNTIME_ENABLED
         if(vm == wasm_interface::vm_type::sys_vm_jit && profile) {
            sysio::vm::set_profile_interval_us(200);
            runtime_interface = std::make_unique<webassembly::sys_vm_runtime::sys_vm_profile_runtime>();
         }
         if(vm == wasm_interface::vm_type::sys_vm_jit && !profile)
            runtime_interface = std::make_unique<webassembly::sys_vm_runtime::sys_vm_runtime<sysio::vm::jit>>();
#endif
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
         if(vm == wasm_interface::vm_type::sys_vm_oc)
            runtime_interface = std::make_unique<webassembly::sysvmoc::sysvmoc_runtime>(data_dir, sysvmoc_config, d);
#endif
         if(!runtime_interface)
            SYS_THROW(wasm_exception, "${r} wasm runtime not supported on this platform and/or configuration", ("r", vm));

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
         if(sysvmoc_tierup != wasm_interface::vm_oc_enable::oc_none) {
            SYS_ASSERT(vm != wasm_interface::vm_type::sys_vm_oc, wasm_exception, "You can't use SYS VM OC as the base runtime when tier up is activated");
            sysvmoc = std::make_unique<sysvmoc_tier>(data_dir, sysvmoc_config, d);
         }
#endif
      }

      ~wasm_interface_impl() = default;

      bool is_code_cached(const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) const {
         // This method is only called from tests; performance is not critical.
         // No need for an additional check if we should lock or not.
         std::lock_guard g(instantiation_cache_mutex);
         wasm_cache_index::iterator it = wasm_instantiation_cache.find( boost::make_tuple(code_hash, vm_type, vm_version) );
         return it != wasm_instantiation_cache.end();
      }

      void code_block_num_last_used(const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version, const uint32_t& block_num) {
         // The caller of this method apply_sysio_setcode has asserted that
         // the transaction is not read-only, implying we are
         // in write window. Read-only threads are not running.
         // Safe to update the cache without locking.
         wasm_cache_index::iterator it = wasm_instantiation_cache.find(boost::make_tuple(code_hash, vm_type, vm_version));
         if(it != wasm_instantiation_cache.end())
            wasm_instantiation_cache.modify(it, [block_num](wasm_cache_entry& e) {
               e.last_block_num_used = block_num;
            });
      }

      // reports each code_hash and vm_version that will be erased to callback
      void current_lib(uint32_t lib) {
         // producer_plugin has asserted irreversible_block signal is called
         // in write window. Read-only threads are not running.
         // Safe to update the cache without locking.
         // Anything last used before or on the LIB can be evicted.
         const auto first_it = wasm_instantiation_cache.get<by_last_block_num>().begin();
         const auto last_it  = wasm_instantiation_cache.get<by_last_block_num>().upper_bound(lib);
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
         if(sysvmoc) for(auto it = first_it; it != last_it; it++)
            sysvmoc->cc.free_code(it->code_hash, it->vm_version);
#endif
         wasm_instantiation_cache.get<by_last_block_num>().erase(first_it, last_it);
      }

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
      bool is_sys_vm_oc_enabled() const {
         return (sysvmoc || wasm_runtime_time == wasm_interface::vm_type::sys_vm_oc);
      }
#endif

      const std::unique_ptr<wasm_instantiated_module_interface>& get_instantiated_module(
         const digest_type&   code_hash,
         const uint8_t&       vm_type,
         const uint8_t&       vm_version,
         transaction_context& trx_context)
      {
         if (trx_context.control.is_write_window()) {
            // When in write window (either read only threads are not enabled or
            // they are not schedued to run), only main thread is processing
            // transactions. No need to lock.
            return get_or_build_instantiated_module(code_hash, vm_type, vm_version, trx_context);
         } else {
            std::lock_guard g(instantiation_cache_mutex);
            return get_or_build_instantiated_module(code_hash, vm_type, vm_version, trx_context);
         }
      }

      // Locked by the caller if required.
      const std::unique_ptr<wasm_instantiated_module_interface>& get_or_build_instantiated_module(
         const digest_type&   code_hash,
         const uint8_t&       vm_type,
         const uint8_t&       vm_version,
         transaction_context& trx_context )
      {
         wasm_cache_index::iterator it = wasm_instantiation_cache.find( boost::make_tuple(code_hash, vm_type, vm_version) );
         if (it != wasm_instantiation_cache.end()) {
            // An instantiated module's module should never be null.
            assert(it->module);
            return it->module;
         }

         const code_object* codeobject = &db.get<code_object,by_code_hash>(boost::make_tuple(code_hash, vm_type, vm_version));
         it = wasm_instantiation_cache.emplace( wasm_interface_impl::wasm_cache_entry {
            .code_hash = code_hash,
            .last_block_num_used = UINT32_MAX,
            .module = nullptr,
            .vm_type = vm_type,
            .vm_version = vm_version
         } ).first;
         auto timer_pause = fc::make_scoped_exit([&](){
            trx_context.resume_billing_timer();
         });
         trx_context.pause_billing_timer();
         wasm_instantiation_cache.modify(it, [&](auto& c) {
            c.module = runtime_interface->instantiate_module(codeobject->code.data(), codeobject->code.size(), code_hash, vm_type, vm_version);
         });
         return it->module;
      }

      std::unique_ptr<wasm_runtime_interface> runtime_interface;

      typedef boost::multi_index_container<
         wasm_cache_entry,
         indexed_by<
            ordered_unique<tag<by_hash>,
               composite_key< wasm_cache_entry,
                  member<wasm_cache_entry, digest_type, &wasm_cache_entry::code_hash>,
                  member<wasm_cache_entry, uint8_t,     &wasm_cache_entry::vm_type>,
                  member<wasm_cache_entry, uint8_t,     &wasm_cache_entry::vm_version>
               >
            >,
            ordered_non_unique<tag<by_last_block_num>, member<wasm_cache_entry, uint32_t, &wasm_cache_entry::last_block_num_used>>
         >
      > wasm_cache_index;
      mutable std::mutex instantiation_cache_mutex;
      wasm_cache_index wasm_instantiation_cache;

      const chainbase::database& db;
      const wasm_interface::vm_type wasm_runtime_time;

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
      std::unique_ptr<struct sysvmoc_tier> sysvmoc{nullptr}; // used by all threads
#endif
   };

} } // sysio::chain
