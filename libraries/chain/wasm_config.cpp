#include <sysio/chain/wasm_config.hpp>
#include <sysio/chain/exceptions.hpp>

using namespace sysio::chain;

void wasm_config::validate() const {
   SYS_ASSERT(max_section_elements >= 4, action_validate_exception, "max_section_elements cannot be less than 4");
   SYS_ASSERT(max_func_local_bytes >= 8, action_validate_exception, "max_func_local_bytes cannot be less than 8");
   SYS_ASSERT(max_nested_structures >= 1, action_validate_exception, "max_nested_structures cannot be less than 1");
   SYS_ASSERT(max_symbol_bytes >= 32, action_validate_exception, "max_symbol_bytes cannot be less than 32");
   SYS_ASSERT(max_module_bytes >= 256, action_validate_exception, "max_module_bytes cannot be less than 256");
   SYS_ASSERT(max_code_bytes >= 32, action_validate_exception, "max_code_bytes cannot be less than 32");
   SYS_ASSERT(max_pages >= 1, action_validate_exception, "max_pages cannot be less than 1");
   SYS_ASSERT(max_call_depth >= 2, action_validate_exception, "max_call_depth cannot be less than 2");
}
