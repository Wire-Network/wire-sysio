bootstrap_contract(sysio.roa)
if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.roa sysio.roa sysio.roa.cpp)

   target_include_directories(sysio.roa
      PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include)
endif()

# Native .so for native-module runtime debugging
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
   add_native_contract(
      TARGET         sysio.roa_native
      SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.cpp
      INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}
                     ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include
      CONTRACT_CLASS "sysio::roa"
      HEADERS        "sysio.roa.hpp"
      ABI_FILE       ${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.abi
   )
endif()
