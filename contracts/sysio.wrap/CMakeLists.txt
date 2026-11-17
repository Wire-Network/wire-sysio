bootstrap_contract(sysio.wrap)
if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.wrap sysio.wrap sysio.wrap.cpp)

   # canonical_transaction.hpp: the packed-transaction decode sysio.wrap shares with sysio.msig.
   target_include_directories(sysio.wrap
      PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include)
endif()

# Native .so for native-module runtime debugging
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES AND BUILD_SYSTEM_CONTRACTS)
   add_native_contract(
      TARGET         sysio.wrap_native
      SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/sysio.wrap.cpp
      INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}
                     ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include
      CONTRACT_CLASS "sysio::wrap"
      HEADERS        "sysio.wrap.hpp"
      ABI_FILE       ${CMAKE_BINARY_DIR}/contracts/sysio.wrap/sysio.wrap.abi
   )
endif()
