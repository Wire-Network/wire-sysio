bootstrap_contract(sysio.wrap)
if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.wrap sysio.wrap sysio.wrap.cpp)
endif()

# Native .so for native-module runtime debugging
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES AND NATIVE_CDT_DIR)
   native_contract_target(
      TARGET         sysio.wrap_native
      SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/sysio.wrap.cpp
      INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}
      CONTRACT_CLASS "sysio::wrap"
      HEADERS        "sysio.wrap.hpp"
      ABI_TARGET     sysio.wrap
   )
endif()
