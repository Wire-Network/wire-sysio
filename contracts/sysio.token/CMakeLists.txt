bootstrap_contract(sysio.token sysio.token.bad.abi)
if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.token sysio.token sysio.token.cpp)
   target_include_directories(sysio.token
      PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
endif()

# Native .so for native-module runtime debugging
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES AND BUILD_SYSTEM_CONTRACTS)
   add_native_contract(
      TARGET         sysio.token_native
      SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/sysio.token.cpp
      INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}/include
      CONTRACT_CLASS "sysio::token"
      HEADERS        "sysio.token/sysio.token.hpp"
      ABI_FILE       ${CMAKE_BINARY_DIR}/contracts/sysio.token/sysio.token.abi
   )
endif()
