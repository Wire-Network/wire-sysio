bootstrap_contract(sysio.system)

set(SYSIO_SYSTEM_SOURCES
   ${CMAKE_CURRENT_SOURCE_DIR}/src/sysio.system.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/native.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/producer_pay.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/ranking.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/limit_auth_changes.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/finalizer_key.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/peer_keys.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/trx_priority.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/block_info.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/emissions.cpp)

set(SYSIO_SYSTEM_HEADERS
   "sysio.system/sysio.system.hpp"
   "sysio.system/peer_keys.hpp"
   "sysio.system/trx_priority.hpp")

set(SYSIO_SYSTEM_INCLUDE_DIRS
   ${CMAKE_CURRENT_SOURCE_DIR}/include
   ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.token/include)

# OPP integration: emissions reads opreg/epoch state via generated protobuf types + shared DataStream operators.
# generated-cdt is reached via CMAKE_CURRENT_BINARY_DIR since contracts_project is a nested ExternalProject.
set(SYSIO_SYSTEM_OPP_INCLUDE_DIRS
   ${CMAKE_CURRENT_BINARY_DIR}/../../libraries/opp/generated-cdt
   ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.opp.common/include
   ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.opreg/include
   ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.epoch/include)

if(BUILD_SYSTEM_CONTRACTS)
   # emissions.cpp pulls in OPP-generated types.pb.hpp which uses magic_enum.
   find_cdt_magic_enum()

   add_contract(sysio.system sysio.system ${SYSIO_SYSTEM_SOURCES})

   target_include_directories(sysio.system
      PUBLIC ${SYSIO_SYSTEM_INCLUDE_DIRS}
             ${SYSIO_SYSTEM_OPP_INCLUDE_DIRS})

   target_link_libraries(sysio.system
      INTERFACE magic_enum::magic_enum)
endif()

# Native .so for native-module runtime debugging
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES AND BUILD_SYSTEM_CONTRACTS)
   add_native_contract(
      TARGET         sysio.system_native
      SOURCES        ${SYSIO_SYSTEM_SOURCES}
      INCLUDE_DIRS   ${SYSIO_SYSTEM_INCLUDE_DIRS} ${SYSIO_SYSTEM_OPP_INCLUDE_DIRS}
      CONTRACT_CLASS "sysiosystem::system_contract"
      HEADERS        ${SYSIO_SYSTEM_HEADERS}
      ABI_FILE       ${CMAKE_BINARY_DIR}/contracts/sysio.system/sysio.system.abi
      EXTRA_DISPATCH "sysiosystem::peer_keys:regpeerkey,delpeerkey,getpeerkeys"
                     "sysiosystem::trx_priority:addtrxp,deltrxp"
   )

   target_link_libraries(sysio.system_native
      INTERFACE magic_enum::magic_enum)
endif()
