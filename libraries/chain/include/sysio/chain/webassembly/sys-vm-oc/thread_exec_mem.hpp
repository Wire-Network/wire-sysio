#pragma once

#include <sysio/chain/webassembly/sys-vm-oc/executor.hpp>
#include <sysio/chain/webassembly/sys-vm-oc/memory.hpp>
#include <sysio/chain/wasm_sysio_constraints.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace sysio { namespace chain { namespace sysvmoc {

class code_cache_base;

/**
 * Provides the sys-vm-oc executor and memory for whichever thread is executing, always paired
 * with the one code cache this instance was constructed against.
 *
 * An executor mmaps its code cache's backing file, and the code descriptors handed out by that
 * cache hold offsets that are only meaningful within that same file. A process can contain
 * multiple wasm_interface instances -- e.g. a validating tester runs two controllers, each with
 * its own sys-vm-oc runtime or tier-up -- so executor/memory state shared loosely across
 * instances (such as a bare `thread_local static`) can end up executing one cache's descriptor
 * offsets against another cache's mapping, running garbage. Each runtime/tier-up instance
 * therefore owns one of these providers:
 *
 *  - The main thread's executor/memory are direct members, created with the instance, so their
 *    pairing with the instance's code cache is guaranteed by construction.
 *  - Read-only threads share one `thread_local` executor/memory slot per thread. Each slot is
 *    tagged with the id of the owning provider and is rebuilt from the calling provider's code
 *    cache whenever a different provider executes on that thread.
 *
 * Owner ids are minted from a monotonic counter and never reused, so a destroyed provider's id
 * cannot collide with a later provider the way a recycled `this` pointer could. A read-only
 * thread's slot built by a since-destroyed provider is dormant (never executed from) until some
 * other provider's ensure_ro_thread_state() rebuilds it.
 */
class thread_exec_mem {
   public:
      /// Construct the main thread's executor (mapping cc's cache file) and full-sized memory.
      /// Must be constructed on the main thread, before any read-only thread executes.
      explicit thread_exec_mem(const code_cache_base& cc)
         : cc(cc)
         , main_thread_exec(cc)
         , main_thread_mem(wasm_constraints::maximum_linear_memory/wasm_constraints::wasm_page_size) {}

      /// Executor for the calling thread, built from this instance's code cache.
      executor& get_executor() {
         if(std::this_thread::get_id() == main_thread_id)
            return main_thread_exec;
         ensure_ro_thread_state();
         return *ro_thread_state.exec;
      }

      /// Memory for the calling thread (full-sized on the main thread, read-only-thread-sized
      /// elsewhere).
      memory& get_memory() {
         if(std::this_thread::get_id() == main_thread_id)
            return main_thread_mem;
         ensure_ro_thread_state();
         return *ro_thread_state.mem;
      }

      /// Eagerly build the calling read-only thread's executor/memory; called from the
      /// read-only thread pool's thread-start hook.
      void init_thread_local_data() { ensure_ro_thread_state(); }

   private:
      /// Rebuild the calling thread's thread_local executor/memory from this instance's code
      /// cache if they were built by a different instance (or not yet built).
      void ensure_ro_thread_state() {
         if(ro_thread_state.owner_id != id) {
            ro_thread_state.exec     = std::make_unique<executor>(cc);
            ro_thread_state.mem      = std::make_unique<memory>(memory::sliced_pages_for_ro_thread);
            ro_thread_state.owner_id = id;
         }
      }

      /// Process-unique id; monotonic, never reused.
      static uint64_t next_owner_id() {
         static std::atomic<uint64_t> counter{0};
         return ++counter;
      }

      const code_cache_base& cc;               ///< cache this instance's executors are built from
      executor               main_thread_exec; ///< main thread executor; maps cc's cache file
      memory                 main_thread_mem;  ///< main thread memory
      const std::thread::id  main_thread_id = std::this_thread::get_id();
      const uint64_t         id = next_owner_id();

      /// One executor/memory slot per read-only thread, shared by all providers in the process
      /// and tagged with the id of the provider that built it. Defined in executor.cpp.
      struct ro_thread_exec_mem {
         uint64_t                  owner_id = 0; ///< id of the provider that built exec/mem; 0 = none
         std::unique_ptr<executor> exec;
         std::unique_ptr<memory>   mem;
      };
      thread_local static ro_thread_exec_mem ro_thread_state;
};

}}} // namespace sysio::chain::sysvmoc
