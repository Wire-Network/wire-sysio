#include <sysio/chain/webassembly/sys-vm.hpp>
#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/wasm_sysio_constraints.hpp>
//sys-vm includes
#include <sysio/vm/backend.hpp>
#include <sysio/chain/webassembly/preconditions.hpp>
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
#include <sysio/chain/webassembly/sys-vm-oc.hpp>
#endif
#include <boost/hana/string.hpp>
#include <boost/hana/equal.hpp>

namespace sysio { namespace chain { namespace webassembly { namespace sys_vm_runtime {

using namespace sysio::vm;

namespace wasm_constraints = sysio::chain::wasm_constraints;

namespace {

  struct checktime_watchdog {
     checktime_watchdog(transaction_checktime_timer& timer) : _timer(timer) {}
     template<typename F>
     struct guard {
        guard(transaction_checktime_timer& timer, F&& func)
           : _timer(timer), _func(static_cast<F&&>(func)) {
           _timer.set_expiration_callback(&callback, this);
           if(_timer.expired) {
              _func(); // it's harmless if _func is invoked twice
           }
        }
        ~guard() {
           _timer.set_expiration_callback(nullptr, nullptr);
        }
        static void callback(void* data) {
           guard* self = static_cast<guard*>(data);
           self->_func();
        }
        transaction_checktime_timer& _timer;
        F _func;
     };
     template<typename F>
     guard<F> scoped_run(F&& func) {
        return guard{_timer, static_cast<F&&>(func)};
     }
     transaction_checktime_timer& _timer;
  };
}

// Used on setcode.  Must not reject anything that WAVM accepts
// For the moment, this runs after WAVM validation, as I am not
// sure that sys-vm will replicate WAVM's parsing exactly.
struct setcode_options {
   static constexpr bool forbid_export_mutable_globals = false;
   static constexpr bool allow_code_after_function_end = true;
   static constexpr bool allow_u32_limits_flags = true;
   static constexpr bool allow_invalid_empty_local_set = true;
   static constexpr bool allow_zero_blocktype = true;
};

void validate(const bytes& code, const whitelisted_intrinsics_type& intrinsics) {
   wasm_code_ptr code_ptr((uint8_t*)code.data(), code.size());
   try {
      sys_vm_null_backend_t<setcode_options> bkend(code_ptr, code.size(), nullptr);
      // check import signatures
       sys_vm_host_functions_t::resolve(bkend.get_module());
      // check that the imports are all currently enabled
      const auto& imports = bkend.get_module().imports;
      for(std::uint32_t i = 0; i < imports.size(); ++i) {
         SYS_ASSERT(std::string_view((char*)imports[i].module_str.raw(), imports[i].module_str.size()) == "env" &&
                    is_intrinsic_whitelisted(intrinsics, std::string_view((char*)imports[i].field_str.raw(), imports[i].field_str.size())),
                    wasm_serialization_error, "${module}.${fn} unresolveable",
                    ("module", std::string((char*)imports[i].module_str.raw(), imports[i].module_str.size()))
                    ("fn", std::string((char*)imports[i].field_str.raw(), imports[i].field_str.size())));
      }
   } catch(vm::exception& e) {
      SYS_THROW(wasm_serialization_error, e.detail());
   }
}

void validate( const bytes& code, const wasm_config& cfg, const whitelisted_intrinsics_type& intrinsics ) {
   SYS_ASSERT(code.size() <= cfg.max_module_bytes, wasm_serialization_error, "Code too large");
   wasm_code_ptr code_ptr((uint8_t*)code.data(), code.size());
   try {
      sys_vm_null_backend_t<wasm_config> bkend(code_ptr, code.size(), nullptr, cfg);
      // check import signatures
      sys_vm_host_functions_t::resolve(bkend.get_module());
      // check that the imports are all currently enabled
      const auto& imports = bkend.get_module().imports;
      for(std::uint32_t i = 0; i < imports.size(); ++i) {
         SYS_ASSERT(std::string_view((char*)imports[i].module_str.raw(), imports[i].module_str.size()) == "env" &&
                    is_intrinsic_whitelisted(intrinsics, std::string_view((char*)imports[i].field_str.raw(), imports[i].field_str.size())),
                    wasm_serialization_error, "${module}.${fn} unresolveable",
                    ("module", std::string((char*)imports[i].module_str.raw(), imports[i].module_str.size()))
                    ("fn", std::string((char*)imports[i].field_str.raw(), imports[i].field_str.size())));
      }
      // check apply
      uint32_t apply_idx = bkend.get_module().get_exported_function("apply");
      SYS_ASSERT(apply_idx < std::numeric_limits<uint32_t>::max(), wasm_serialization_error, "apply not exported");
      const vm::func_type& apply_type = bkend.get_module().get_function_type(apply_idx);
      SYS_ASSERT((apply_type == vm::host_function{{vm::i64, vm::i64, vm::i64}, {}}), wasm_serialization_error, "apply has wrong type");
   } catch(vm::exception& e) {
      SYS_THROW(wasm_serialization_error, e.detail());
   }
}

// Be permissive on apply.
struct apply_options {
   std::uint32_t max_pages = wasm_constraints::maximum_linear_memory/wasm_constraints::wasm_page_size;
   std::uint32_t max_call_depth = wasm_constraints::maximum_call_depth+1;
   static constexpr bool forbid_export_mutable_globals = false;
   static constexpr bool allow_code_after_function_end = false;
   static constexpr bool allow_u32_limits_flags = true;
   static constexpr bool allow_invalid_empty_local_set = true;
   static constexpr bool allow_zero_blocktype = true;
};

template<typename Impl>
class sys_vm_instantiated_module : public wasm_instantiated_module_interface {
   using backend_t = sys_vm_backend_t<Impl>;
   public:

      sys_vm_instantiated_module(sys_vm_runtime<Impl>* runtime, std::unique_ptr<backend_t> mod) :
         _runtime(runtime),
         _instantiated_module(std::move(mod)) {}

      void apply(apply_context& context) override {
         // set up backend to share the compiled mod in the instantiated
         // module of the contract
         _runtime->_bkend.share(*_instantiated_module);
         // set exec ctx's mod to instantiated module's mod
         _runtime->_exec_ctx.set_module(&(_instantiated_module->get_module()));
         // link exe ctx to backend
         _runtime->_bkend.set_context(&_runtime->_exec_ctx);
         // set max_call_depth and max_pages to original values
         _runtime->_bkend.reset_max_call_depth();
         _runtime->_bkend.reset_max_pages();
         // set wasm allocator per apply data
         _runtime->_bkend.set_wasm_allocator(&context.control.get_wasm_allocator());

         apply_options opts;
         if(context.control.is_builtin_activated(builtin_protocol_feature_t::configurable_wasm_limits)) {
            const wasm_config& config = context.control.get_global_properties().wasm_configuration;
            opts = {config.max_pages, config.max_call_depth};
         }
         auto fn = [&]() {
            sysio::chain::webassembly::interface iface(context);
            _runtime->_bkend.initialize(&iface, opts);
            _runtime->_bkend.call(
                iface, "env", "apply",
                context.get_receiver().to_uint64_t(),
                context.get_action().account.to_uint64_t(),
                context.get_action().name.to_uint64_t());
         };
         try {
            checktime_watchdog wd(context.trx_context.transaction_timer);
            _runtime->_bkend.timed_run(wd, fn);
         } catch(sysio::vm::timeout_exception&) {
            context.trx_context.checktime();
         } catch(sysio::vm::wasm_memory_exception& e) {
            FC_THROW_EXCEPTION(wasm_execution_error, "access violation");
         } catch(sysio::vm::exception& e) {
            FC_THROW_EXCEPTION(wasm_execution_error, "sys-vm system failure");
         }
      }

   private:
      sys_vm_runtime<Impl>*            _runtime;
      std::unique_ptr<backend_t> _instantiated_module;
};

#ifdef __x86_64__
class sys_vm_profiling_module : public wasm_instantiated_module_interface {
      using backend_t = sysio::vm::backend<sys_vm_host_functions_t, sysio::vm::jit_profile, webassembly::sys_vm_runtime::apply_options, vm::profile_instr_map>;
   public:
      sys_vm_profiling_module(std::unique_ptr<backend_t> mod, const char * code, std::size_t code_size) :
         _instantiated_module(std::move(mod)),
         _original_code(code, code + code_size) {}


      void apply(apply_context& context) override {
         _instantiated_module->set_wasm_allocator(&context.control.get_wasm_allocator());
         apply_options opts;
         if(context.control.is_builtin_activated(builtin_protocol_feature_t::configurable_wasm_limits)) {
            const wasm_config& config = context.control.get_global_properties().wasm_configuration;
            opts = {config.max_pages, config.max_call_depth};
         }
         auto fn = [&]() {
            sysio::chain::webassembly::interface iface(context);
            _instantiated_module->initialize(&iface, opts);
            _instantiated_module->call(
                iface, "env", "apply",
                context.get_receiver().to_uint64_t(),
                context.get_action().account.to_uint64_t(),
                context.get_action().name.to_uint64_t());
         };
         profile_data* prof = start(context);
         try {
            scoped_profile profile_runner(prof);
            checktime_watchdog wd(context.trx_context.transaction_timer);
            _instantiated_module->timed_run(wd, fn);
         } catch(sysio::vm::timeout_exception&) {
            context.trx_context.checktime();
         } catch(sysio::vm::wasm_memory_exception& e) {
            FC_THROW_EXCEPTION(wasm_execution_error, "access violation");
         } catch(sysio::vm::exception& e) {
            FC_THROW_EXCEPTION(wasm_execution_error, "sys-vm system failure");
         }
      }

      profile_data* start(apply_context& context) {
         name account = context.get_receiver();
         if(!context.control.is_profiling(account)) return nullptr;
         if(auto it = _prof.find(account); it != _prof.end()) {
            return it->second.get();
         } else {
            auto code_sequence = context.control.db().get<account_metadata_object, by_name>(account).code_sequence;
            std::string basename = account.to_string() + "." + std::to_string(code_sequence);
            auto prof = std::make_unique<profile_data>(basename + ".profile", *_instantiated_module);
            auto [pos,_] = _prof.insert(std::pair{ account, std::move(prof)});
            std::ofstream outfile(basename + ".wasm");
            outfile.write(_original_code.data(), _original_code.size());
            return pos->second.get();
         }
         return nullptr;
      }

   private:

      std::unique_ptr<backend_t> _instantiated_module;
      boost::container::flat_map<name, std::unique_ptr<profile_data>> _prof;
      std::vector<char> _original_code;
};
#endif

template<typename Impl>
sys_vm_runtime<Impl>::sys_vm_runtime() {}

template<typename Impl>
std::unique_ptr<wasm_instantiated_module_interface> sys_vm_runtime<Impl>::instantiate_module(const char* code_bytes, size_t code_size,
                                                                                             const digest_type&, const uint8_t&, const uint8_t&) {

   using backend_t = sys_vm_backend_t<Impl>;
   try {
      wasm_code_ptr code((uint8_t*)code_bytes, code_size);
      apply_options options = { .max_pages = 65536,
                                .max_call_depth = 0 };
      std::unique_ptr<backend_t> bkend = nullptr;
#ifdef SYSIO_SYS_VM_JIT_RUNTIME_ENABLED
      if constexpr (std::is_same_v<Impl, sysio::vm::jit>)
         bkend = std::make_unique<backend_t>(code, code_size, nullptr, options, true, false); // true, false <--> single parsing, backend does not own execution context (execution context is reused per thread)
      else
#endif
         bkend = std::make_unique<backend_t>(code, code_size, nullptr, options, false, false); // false, false <--> 2-passes parsing, backend does not own execution context (execution context is reused per thread)
      sys_vm_host_functions_t::resolve(bkend->get_module());
      return std::make_unique<sys_vm_instantiated_module<Impl>>(this, std::move(bkend));
   } catch(sysio::vm::exception& e) {
      FC_THROW_EXCEPTION(wasm_execution_error, "Error building sys-vm interp: ${e}", ("e", e.what()));
   }
}

template class sys_vm_runtime<sysio::vm::interpreter>;
#ifdef __x86_64__
template class sys_vm_runtime<sysio::vm::jit>;

sys_vm_profile_runtime::sys_vm_profile_runtime() {}

std::unique_ptr<wasm_instantiated_module_interface> sys_vm_profile_runtime::instantiate_module(const char* code_bytes, size_t code_size,
                                                                                               const digest_type&, const uint8_t&, const uint8_t&) {

   using backend_t = sysio::vm::backend<sys_vm_host_functions_t, sysio::vm::jit_profile, webassembly::sys_vm_runtime::apply_options, vm::profile_instr_map>;
   try {
      wasm_code_ptr code((uint8_t*)code_bytes, code_size);
      apply_options options = { .max_pages = 65536,
                                .max_call_depth = 0 };
      std::unique_ptr<backend_t> bkend = std::make_unique<backend_t>(code, code_size, nullptr, options, true, false); // true, false <--> single parsing, backend does not own execution context (execution context is reused per thread)
      sys_vm_host_functions_t::resolve(bkend->get_module());
      return std::make_unique<sys_vm_profiling_module>(std::move(bkend), code_bytes, code_size);
   } catch(sysio::vm::exception& e) {
      FC_THROW_EXCEPTION(wasm_execution_error, "Error building sys-vm interp: ${e}", ("e", e.what()));
   }
}
#endif

template<typename Impl>
thread_local typename sys_vm_runtime<Impl>::context_t sys_vm_runtime<Impl>::_exec_ctx;
template<typename Impl>
thread_local sys_vm_backend_t<Impl> sys_vm_runtime<Impl>::_bkend;
}

template <auto HostFunction, typename... Preconditions>
struct host_function_registrator {
   template <typename Mod, typename Name>
   constexpr host_function_registrator(Mod mod_name, Name fn_name) {
      using rhf_t = sys_vm_host_functions_t;
      rhf_t::add<HostFunction, Preconditions...>(mod_name.c_str(), fn_name.c_str());
#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
      constexpr bool is_injected = (Mod() == BOOST_HANA_STRING(SYSIO_INJECTED_MODULE_NAME));
      sysvmoc::register_sysvm_oc<HostFunction, is_injected, std::tuple<Preconditions...>>(
          mod_name + BOOST_HANA_STRING(".") + fn_name);
#endif
   }
};

#define REGISTER_INJECTED_HOST_FUNCTION(NAME, ...)                                                                     \
   static host_function_registrator<&interface::NAME, ##__VA_ARGS__> NAME##_registrator_impl() {                       \
      return {BOOST_HANA_STRING(SYSIO_INJECTED_MODULE_NAME), BOOST_HANA_STRING(#NAME)};                                \
   }                                                                                                                   \
   inline static auto NAME##_registrator = NAME##_registrator_impl();

#define REGISTER_HOST_FUNCTION(NAME, ...)                                                                              \
   static host_function_registrator<&interface::NAME, core_precondition, context_aware_check, ##__VA_ARGS__>           \
       NAME##_registrator_impl() {                                                                                     \
      return {BOOST_HANA_STRING("env"), BOOST_HANA_STRING(#NAME)};                                                     \
   }                                                                                                                   \
   inline static auto NAME##_registrator = NAME##_registrator_impl();

#define REGISTER_CF_HOST_FUNCTION(NAME, ...)                                                                           \
   static host_function_registrator<&interface::NAME, core_precondition, ##__VA_ARGS__> NAME##_registrator_impl() {    \
      return {BOOST_HANA_STRING("env"), BOOST_HANA_STRING(#NAME)};                                                     \
   }                                                                                                                   \
   inline static auto NAME##_registrator = NAME##_registrator_impl();

#define REGISTER_LEGACY_HOST_FUNCTION(NAME, ...)                                                                       \
   static host_function_registrator<&interface::NAME, legacy_static_check_wl_args, context_aware_check, ##__VA_ARGS__> \
       NAME##_registrator_impl() {                                                                                     \
      return {BOOST_HANA_STRING("env"), BOOST_HANA_STRING(#NAME)};                                                     \
   }                                                                                                                   \
   inline static auto NAME##_registrator = NAME##_registrator_impl();

#define REGISTER_LEGACY_CF_HOST_FUNCTION(NAME, ...)                                                                    \
   static host_function_registrator<&interface::NAME, legacy_static_check_wl_args, ##__VA_ARGS__>                      \
       NAME##_registrator_impl() {                                                                                     \
      return {BOOST_HANA_STRING("env"), BOOST_HANA_STRING(#NAME)};                                                     \
   }                                                                                                                   \
   inline static auto NAME##_registrator = NAME##_registrator_impl();

#define REGISTER_LEGACY_CF_ONLY_HOST_FUNCTION(NAME, ...)                                                               \
   static host_function_registrator<&interface::NAME, legacy_static_check_wl_args, context_free_check, ##__VA_ARGS__>  \
       NAME##_registrator_impl() {                                                                                     \
      return {BOOST_HANA_STRING("env"), BOOST_HANA_STRING(#NAME)};                                                     \
   }                                                                                                                   \
   inline static auto NAME##_registrator = NAME##_registrator_impl();

// context free api
REGISTER_LEGACY_CF_ONLY_HOST_FUNCTION(get_context_free_data)

// privileged api
REGISTER_HOST_FUNCTION(is_feature_active, privileged_check);
REGISTER_HOST_FUNCTION(activate_feature, privileged_check);
REGISTER_LEGACY_HOST_FUNCTION(preactivate_feature, privileged_check);
REGISTER_HOST_FUNCTION(set_resource_limits, privileged_check);
REGISTER_LEGACY_HOST_FUNCTION(get_resource_limits, privileged_check);
REGISTER_HOST_FUNCTION(get_parameters_packed, privileged_check);
REGISTER_HOST_FUNCTION(set_parameters_packed, privileged_check);
REGISTER_HOST_FUNCTION(get_wasm_parameters_packed, privileged_check);
REGISTER_HOST_FUNCTION(set_wasm_parameters_packed, privileged_check);
REGISTER_LEGACY_HOST_FUNCTION(set_proposed_producers, privileged_check);
REGISTER_LEGACY_HOST_FUNCTION(set_proposed_producers_ex, privileged_check);
REGISTER_LEGACY_HOST_FUNCTION(get_blockchain_parameters_packed, privileged_check);
REGISTER_LEGACY_HOST_FUNCTION(set_blockchain_parameters_packed, privileged_check);
REGISTER_HOST_FUNCTION(is_privileged, privileged_check);
REGISTER_HOST_FUNCTION(set_privileged, privileged_check);

// softfloat api
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_add);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_sub);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_div);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_mul);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_min);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_max);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_copysign);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_abs);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_neg);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_sqrt);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_ceil);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_floor);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_trunc);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_nearest);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_eq);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_ne);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_lt);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_le);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_gt);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_ge);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_add);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_sub);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_div);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_mul);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_min);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_max);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_copysign);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_abs);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_neg);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_sqrt);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_ceil);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_floor);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_trunc);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_nearest);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_eq);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_ne);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_lt);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_le);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_gt);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_ge);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_promote);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_demote);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_trunc_i32s);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_trunc_i32s);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_trunc_i32u);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_trunc_i32u);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_trunc_i64s);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_trunc_i64s);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f32_trunc_i64u);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_f64_trunc_i64u);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_i32_to_f32);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_i64_to_f32);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_ui32_to_f32);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_ui64_to_f32);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_i32_to_f64);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_i64_to_f64);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_ui32_to_f64);
REGISTER_INJECTED_HOST_FUNCTION(_sysio_ui64_to_f64);

// producer api
REGISTER_LEGACY_HOST_FUNCTION(get_active_producers);

// crypto api
REGISTER_LEGACY_CF_HOST_FUNCTION(assert_recover_key);
REGISTER_LEGACY_CF_HOST_FUNCTION(recover_key);
REGISTER_LEGACY_CF_HOST_FUNCTION(assert_sha256);
REGISTER_LEGACY_CF_HOST_FUNCTION(assert_sha1);
REGISTER_LEGACY_CF_HOST_FUNCTION(assert_sha512);
REGISTER_LEGACY_CF_HOST_FUNCTION(assert_ripemd160);
REGISTER_LEGACY_CF_HOST_FUNCTION(sha256);
REGISTER_LEGACY_CF_HOST_FUNCTION(sha1);
REGISTER_LEGACY_CF_HOST_FUNCTION(sha512);
REGISTER_LEGACY_CF_HOST_FUNCTION(ripemd160);

// permission api
REGISTER_LEGACY_HOST_FUNCTION(check_transaction_authorization);
REGISTER_LEGACY_HOST_FUNCTION(check_permission_authorization);
REGISTER_HOST_FUNCTION(get_permission_last_used);
REGISTER_HOST_FUNCTION(get_account_creation_time);

// authorization api
REGISTER_HOST_FUNCTION(require_auth);
REGISTER_HOST_FUNCTION(require_auth2);
REGISTER_HOST_FUNCTION(has_auth);
REGISTER_HOST_FUNCTION(require_recipient);
REGISTER_HOST_FUNCTION(is_account);
REGISTER_HOST_FUNCTION(get_code_hash);

// system api
REGISTER_HOST_FUNCTION(current_time);
REGISTER_HOST_FUNCTION(publication_time);
REGISTER_LEGACY_HOST_FUNCTION(is_feature_activated);
REGISTER_HOST_FUNCTION(get_sender);

// context-free system api
REGISTER_CF_HOST_FUNCTION(abort)
REGISTER_LEGACY_CF_HOST_FUNCTION(sysio_assert)
REGISTER_LEGACY_CF_HOST_FUNCTION(sysio_assert_message)
REGISTER_CF_HOST_FUNCTION(sysio_assert_code)
REGISTER_CF_HOST_FUNCTION(sysio_exit)

// action api
REGISTER_LEGACY_CF_HOST_FUNCTION(read_action_data);
REGISTER_CF_HOST_FUNCTION(action_data_size);
REGISTER_CF_HOST_FUNCTION(current_receiver);
REGISTER_HOST_FUNCTION(set_action_return_value);

// console api
REGISTER_LEGACY_CF_HOST_FUNCTION(prints);
REGISTER_LEGACY_CF_HOST_FUNCTION(prints_l);
REGISTER_CF_HOST_FUNCTION(printi);
REGISTER_CF_HOST_FUNCTION(printui);
REGISTER_LEGACY_CF_HOST_FUNCTION(printi128);
REGISTER_LEGACY_CF_HOST_FUNCTION(printui128);
REGISTER_CF_HOST_FUNCTION(printsf);
REGISTER_CF_HOST_FUNCTION(printdf);
REGISTER_LEGACY_CF_HOST_FUNCTION(printqf);
REGISTER_CF_HOST_FUNCTION(printn);
REGISTER_LEGACY_CF_HOST_FUNCTION(printhex);

// database api
// primary index api
REGISTER_LEGACY_HOST_FUNCTION(db_store_i64);
REGISTER_LEGACY_HOST_FUNCTION(db_update_i64);
REGISTER_HOST_FUNCTION(db_remove_i64);
REGISTER_LEGACY_HOST_FUNCTION(db_get_i64);
REGISTER_LEGACY_HOST_FUNCTION(db_next_i64);
REGISTER_LEGACY_HOST_FUNCTION(db_previous_i64);
REGISTER_HOST_FUNCTION(db_find_i64);
REGISTER_HOST_FUNCTION(db_lowerbound_i64);
REGISTER_HOST_FUNCTION(db_upperbound_i64);
REGISTER_HOST_FUNCTION(db_end_i64);

// uint64_t secondary index api
REGISTER_LEGACY_HOST_FUNCTION(db_idx64_store);
REGISTER_LEGACY_HOST_FUNCTION(db_idx64_update);
REGISTER_HOST_FUNCTION(db_idx64_remove);
REGISTER_LEGACY_HOST_FUNCTION(db_idx64_find_secondary);
REGISTER_LEGACY_HOST_FUNCTION(db_idx64_find_primary);
REGISTER_LEGACY_HOST_FUNCTION(db_idx64_lowerbound);
REGISTER_LEGACY_HOST_FUNCTION(db_idx64_upperbound);
REGISTER_HOST_FUNCTION(db_idx64_end);
REGISTER_LEGACY_HOST_FUNCTION(db_idx64_next);
REGISTER_LEGACY_HOST_FUNCTION(db_idx64_previous);

// uint128_t secondary index api
REGISTER_LEGACY_HOST_FUNCTION(db_idx128_store);
REGISTER_LEGACY_HOST_FUNCTION(db_idx128_update);
REGISTER_HOST_FUNCTION(db_idx128_remove);
REGISTER_LEGACY_HOST_FUNCTION(db_idx128_find_secondary);
REGISTER_LEGACY_HOST_FUNCTION(db_idx128_find_primary);
REGISTER_LEGACY_HOST_FUNCTION(db_idx128_lowerbound);
REGISTER_LEGACY_HOST_FUNCTION(db_idx128_upperbound);
REGISTER_HOST_FUNCTION(db_idx128_end);
REGISTER_LEGACY_HOST_FUNCTION(db_idx128_next);
REGISTER_LEGACY_HOST_FUNCTION(db_idx128_previous);

// 256-bit secondary index api
REGISTER_LEGACY_HOST_FUNCTION(db_idx256_store);
REGISTER_LEGACY_HOST_FUNCTION(db_idx256_update);
REGISTER_HOST_FUNCTION(db_idx256_remove);
REGISTER_LEGACY_HOST_FUNCTION(db_idx256_find_secondary);
REGISTER_LEGACY_HOST_FUNCTION(db_idx256_find_primary);
REGISTER_LEGACY_HOST_FUNCTION(db_idx256_lowerbound);
REGISTER_LEGACY_HOST_FUNCTION(db_idx256_upperbound);
REGISTER_HOST_FUNCTION(db_idx256_end);
REGISTER_LEGACY_HOST_FUNCTION(db_idx256_next);
REGISTER_LEGACY_HOST_FUNCTION(db_idx256_previous);

// double secondary index api
REGISTER_LEGACY_HOST_FUNCTION(db_idx_double_store, is_nan_check);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_double_update, is_nan_check);
REGISTER_HOST_FUNCTION(db_idx_double_remove);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_double_find_secondary, is_nan_check);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_double_find_primary);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_double_lowerbound, is_nan_check);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_double_upperbound, is_nan_check);
REGISTER_HOST_FUNCTION(db_idx_double_end);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_double_next);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_double_previous);

// long double secondary index api
REGISTER_LEGACY_HOST_FUNCTION(db_idx_long_double_store, is_nan_check);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_long_double_update, is_nan_check);
REGISTER_HOST_FUNCTION(db_idx_long_double_remove);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_long_double_find_secondary, is_nan_check);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_long_double_find_primary);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_long_double_lowerbound, is_nan_check);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_long_double_upperbound, is_nan_check);
REGISTER_HOST_FUNCTION(db_idx_long_double_end);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_long_double_next);
REGISTER_LEGACY_HOST_FUNCTION(db_idx_long_double_previous);

// memory api
REGISTER_LEGACY_CF_HOST_FUNCTION(memcpy);
REGISTER_LEGACY_CF_HOST_FUNCTION(memmove);
REGISTER_LEGACY_CF_HOST_FUNCTION(memcmp);
REGISTER_LEGACY_CF_HOST_FUNCTION(memset);

// transaction api
REGISTER_LEGACY_HOST_FUNCTION(send_inline);
REGISTER_LEGACY_HOST_FUNCTION(send_context_free_inline);
REGISTER_LEGACY_HOST_FUNCTION(send_deferred);
REGISTER_LEGACY_HOST_FUNCTION(cancel_deferred);

// context-free transaction api
REGISTER_LEGACY_CF_HOST_FUNCTION(read_transaction);
REGISTER_CF_HOST_FUNCTION(transaction_size);
REGISTER_CF_HOST_FUNCTION(expiration);
REGISTER_CF_HOST_FUNCTION(tapos_block_num);
REGISTER_CF_HOST_FUNCTION(tapos_block_prefix);
REGISTER_LEGACY_CF_HOST_FUNCTION(get_action);

// compiler builtins api
REGISTER_LEGACY_CF_HOST_FUNCTION(__ashlti3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__ashrti3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__lshlti3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__lshrti3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__divti3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__udivti3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__multi3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__modti3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__umodti3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__addtf3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__subtf3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__multf3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__divtf3);
REGISTER_LEGACY_CF_HOST_FUNCTION(__negtf2);
REGISTER_LEGACY_CF_HOST_FUNCTION(__extendsftf2);
REGISTER_LEGACY_CF_HOST_FUNCTION(__extenddftf2);
REGISTER_CF_HOST_FUNCTION(__trunctfdf2);
REGISTER_CF_HOST_FUNCTION(__trunctfsf2);
REGISTER_CF_HOST_FUNCTION(__fixtfsi);
REGISTER_CF_HOST_FUNCTION(__fixtfdi);
REGISTER_LEGACY_CF_HOST_FUNCTION(__fixtfti);
REGISTER_CF_HOST_FUNCTION(__fixunstfsi);
REGISTER_CF_HOST_FUNCTION(__fixunstfdi);
REGISTER_LEGACY_CF_HOST_FUNCTION(__fixunstfti);
REGISTER_LEGACY_CF_HOST_FUNCTION(__fixsfti);
REGISTER_LEGACY_CF_HOST_FUNCTION(__fixdfti);
REGISTER_LEGACY_CF_HOST_FUNCTION(__fixunssfti);
REGISTER_LEGACY_CF_HOST_FUNCTION(__fixunsdfti);
REGISTER_CF_HOST_FUNCTION(__floatsidf);
REGISTER_LEGACY_CF_HOST_FUNCTION(__floatsitf);
REGISTER_LEGACY_CF_HOST_FUNCTION(__floatditf);
REGISTER_LEGACY_CF_HOST_FUNCTION(__floatunsitf);
REGISTER_LEGACY_CF_HOST_FUNCTION(__floatunditf);
REGISTER_CF_HOST_FUNCTION(__floattidf);
REGISTER_CF_HOST_FUNCTION(__floatuntidf);
REGISTER_CF_HOST_FUNCTION(__cmptf2);
REGISTER_CF_HOST_FUNCTION(__eqtf2);
REGISTER_CF_HOST_FUNCTION(__netf2);
REGISTER_CF_HOST_FUNCTION(__getf2);
REGISTER_CF_HOST_FUNCTION(__gttf2);
REGISTER_CF_HOST_FUNCTION(__letf2);
REGISTER_CF_HOST_FUNCTION(__lttf2);
REGISTER_CF_HOST_FUNCTION(__unordtf2);

// get_block_num protocol feature
REGISTER_CF_HOST_FUNCTION( get_block_num );

// crypto_primitives protocol feature
REGISTER_CF_HOST_FUNCTION( alt_bn128_add );
REGISTER_CF_HOST_FUNCTION( alt_bn128_mul );
REGISTER_CF_HOST_FUNCTION( alt_bn128_pair );
REGISTER_CF_HOST_FUNCTION( mod_exp );
REGISTER_CF_HOST_FUNCTION( blake2_f );
REGISTER_CF_HOST_FUNCTION( sha3 );
REGISTER_CF_HOST_FUNCTION( k1_recover );
REGISTER_CF_HOST_FUNCTION( blake2b_256 );

// bls_primitives protocol feature
REGISTER_CF_HOST_FUNCTION( bls_g1_add );
REGISTER_CF_HOST_FUNCTION( bls_g2_add );
REGISTER_CF_HOST_FUNCTION( bls_g1_weighted_sum );
REGISTER_CF_HOST_FUNCTION( bls_g2_weighted_sum );
REGISTER_CF_HOST_FUNCTION( bls_pairing );
REGISTER_CF_HOST_FUNCTION( bls_g1_map );
REGISTER_CF_HOST_FUNCTION( bls_g2_map );
REGISTER_CF_HOST_FUNCTION( bls_fp_mod );
REGISTER_CF_HOST_FUNCTION( bls_fp_mul );
REGISTER_CF_HOST_FUNCTION( bls_fp_exp ); 

} // namespace webassembly
} // namespace chain
} // namespace sysio
