#pragma once

#include <sysio/chain/webassembly/common.hpp>
#include <sysio/chain/webassembly/runtime_interface.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/wasm_config.hpp>
#include <sysio/chain/whitelisted_intrinsics.hpp>
#include <softfloat_types.h>

//sys-vm includes
#include <sysio/vm/backend.hpp>
#include <sysio/vm/profile.hpp>

namespace sysio { namespace chain { namespace webassembly { namespace sys_vm_runtime {

struct apply_options;

}}

template <typename Impl>
using sys_vm_backend_t = sysio::vm::backend<sys_vm_host_functions_t, Impl, webassembly::sys_vm_runtime::apply_options, vm::profile_instr_map>;

template <typename Options>
using sys_vm_null_backend_t = sysio::vm::backend<sys_vm_host_functions_t, sysio::vm::null_backend, Options>;

namespace webassembly { namespace sys_vm_runtime {

using namespace fc;
using namespace sysio::vm;

void validate(const bytes& code, const whitelisted_intrinsics_type& intrinsics );

void validate(const bytes& code, const wasm_config& cfg, const whitelisted_intrinsics_type& intrinsics );

struct apply_options;

struct profile_config {
   boost::container::flat_set<name> accounts_to_profile;
};

template<typename Backend>
class sys_vm_runtime : public sysio::chain::wasm_runtime_interface {
   using context_t = typename Backend::template context<sys_vm_host_functions_t>;
   public:
      sys_vm_runtime();
      std::unique_ptr<wasm_instantiated_module_interface> instantiate_module(const char* code_bytes, size_t code_size,
                                                                             const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) override;

   private:
      // todo: managing this will get more complicated with sync calls;
      // Each thread uses its own backend and exec context.
      // Their constructors do not take any arguments; therefore their life time
      // do not rely on others. Safe to be thread_local.
      thread_local static sys_vm_backend_t<Backend> _bkend;
      thread_local static context_t                 _exec_ctx;

   template<typename Impl>
   friend class sys_vm_instantiated_module;
};

class sys_vm_profile_runtime : public sysio::chain::wasm_runtime_interface {
   public:
      sys_vm_profile_runtime();
      std::unique_ptr<wasm_instantiated_module_interface> instantiate_module(const char* code_bytes, size_t code_size,
                                                                             const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) override;
};

}}}}// sysio::chain::webassembly::sys_vm_runtime
