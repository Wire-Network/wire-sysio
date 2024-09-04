#pragma once

#include <sysio/chain/webassembly/common.hpp>
#include <sysio/chain/webassembly/runtime_interface.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/wasm_config.hpp>
#include <sysio/chain/whitelisted_intrinsics.hpp>
#include <softfloat_types.h>

//eos-vm includes
#include <sysio/vm/backend.hpp>
#include <sysio/vm/profile.hpp>

namespace sysio { namespace chain { namespace webassembly { namespace eos_vm_runtime {

struct apply_options;

}}

template <typename Impl>
using eos_vm_backend_t = sysio::vm::backend<eos_vm_host_functions_t, Impl, webassembly::eos_vm_runtime::apply_options, vm::profile_instr_map>;

template <typename Options>
using eos_vm_null_backend_t = sysio::vm::backend<eos_vm_host_functions_t, sysio::vm::null_backend, Options>;

namespace webassembly { namespace eos_vm_runtime {

using namespace fc;
using namespace sysio::vm;

void validate(const bytes& code, const whitelisted_intrinsics_type& intrinsics );

void validate(const bytes& code, const wasm_config& cfg, const whitelisted_intrinsics_type& intrinsics );

struct apply_options;

struct profile_config {
   boost::container::flat_set<name> accounts_to_profile;
};

template<typename Backend>
class eos_vm_runtime : public sysio::chain::wasm_runtime_interface {
   public:
      eos_vm_runtime();
      std::unique_ptr<wasm_instantiated_module_interface> instantiate_module(const char* code_bytes, size_t code_size, std::vector<uint8_t>,
                                                                             const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) override;

      void immediately_exit_currently_running_module() override;

   private:
      // todo: managing this will get more complicated with sync calls;
      //       immediately_exit_currently_running_module() should probably
      //       move from wasm_runtime_interface to wasm_instantiated_module_interface.
      eos_vm_backend_t<Backend>* _bkend = nullptr;  // non owning pointer to allow for immediate exit

   template<typename Impl>
   friend class eos_vm_instantiated_module;
};

class eos_vm_profile_runtime : public sysio::chain::wasm_runtime_interface {
   public:
      eos_vm_profile_runtime();
      std::unique_ptr<wasm_instantiated_module_interface> instantiate_module(const char* code_bytes, size_t code_size, std::vector<uint8_t>,
                                                                             const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version) override;

      void immediately_exit_currently_running_module() override;
};

}}}}// sysio::chain::webassembly::eos_vm_runtime
