# Register committed artifacts in host builds and compile with CDT in contract builds.
set(contract_name sysio.kicker)
bootstrap_contract(${contract_name})
if(BUILD_SYSTEM_CONTRACTS)
   find_cdt_magic_enum()
   set(SOURCES src/sysio.kicker.cpp)
   add_contract(${contract_name} ${contract_name} ${SOURCES})
   set(targets ${contract_name})
   if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
      list(APPEND targets ${contract_name}_native)
      add_native_contract(
         TARGET ${contract_name}_native
         SOURCES ${SOURCES}
         INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
         CONTRACT_CLASS "sysio::kicker"
         HEADERS "sysio.kicker/sysio.kicker.hpp"
         ABI_FILE ${CMAKE_BINARY_DIR}/contracts/${contract_name}/${contract_name}.abi)
   endif()
   foreach(target ${targets})
      target_include_directories(${target} PUBLIC
         ${CMAKE_CURRENT_SOURCE_DIR}/include
         ${CMAKE_CURRENT_BINARY_DIR}/../../libraries/opp/generated-cdt
         ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.opp.common/include
         ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.andon/include
         ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.liq/include
         ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.token/include
         ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include)
      target_link_libraries(${target} INTERFACE magic_enum::magic_enum)
   endforeach()
endif()
