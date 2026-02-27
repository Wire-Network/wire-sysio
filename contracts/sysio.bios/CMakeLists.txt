bootstrap_contract(sysio.bios)
if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.bios sysio.bios sysio.bios.cpp)
endif()

# Native .so for native-module runtime debugging
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES AND NATIVE_CDT_DIR)
   native_contract_target(
      TARGET         sysio.bios_native
      SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/sysio.bios.cpp
      INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}
      CONTRACT_CLASS "sysiobios::bios"
      HEADERS        "sysio.bios.hpp"
      ABI_TARGET     sysio.bios
   )
endif()
