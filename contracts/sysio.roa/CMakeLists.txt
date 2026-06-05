bootstrap_contract(sysio.roa)
if(BUILD_SYSTEM_CONTRACTS)
   find_cdt_magic_enum()
   add_contract(sysio.roa sysio.roa sysio.roa.cpp)

   target_include_directories(sysio.roa
      PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.authex/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.opp.common/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/libfc-lite/include
      ${CMAKE_CURRENT_BINARY_DIR}/../../libraries/opp/generated-cdt)

   target_link_libraries(sysio.roa
      INTERFACE
      magic_enum::magic_enum)
endif()

# Native .so for native-module runtime debugging
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES AND BUILD_SYSTEM_CONTRACTS)
   add_native_contract(
      TARGET         sysio.roa_native
      SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.cpp
      INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}
                     ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include
                     ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.authex/include
                     ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.opp.common/include
                     ${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/libfc-lite/include
                     ${CMAKE_CURRENT_BINARY_DIR}/../../libraries/opp/generated-cdt
      CONTRACT_CLASS "sysio::roa"
      HEADERS        "sysio.roa.hpp"
      ABI_FILE       ${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.abi
   )
endif()
