bootstrap_contract(sysio.msig)
if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.msig sysio.msig sysio.msig.cpp)
endif()

# Native .so for native-module runtime debugging
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES AND NATIVE_CDT_DIR)
   native_contract_target(
      TARGET         sysio.msig_native
      SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/sysio.msig.cpp
      INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}
      CONTRACT_CLASS "sysio::multisig"
      HEADERS        "sysio.msig.hpp"
      ABI_TARGET     sysio.msig
   )
endif()
