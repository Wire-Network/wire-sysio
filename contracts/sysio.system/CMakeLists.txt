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
   ${CMAKE_CURRENT_SOURCE_DIR}/src/expandauth.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/block_info.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/snapshot_attest.cpp)

set(SYSIO_SYSTEM_HEADERS
   "sysio.system/sysio.system.hpp"
   "sysio.system/peer_keys.hpp"
   "sysio.system/trx_priority.hpp"
   "sysio.system/snapshot_attest.hpp")

set(SYSIO_SYSTEM_INCLUDE_DIRS
   ${CMAKE_CURRENT_SOURCE_DIR}/include
   ${CMAKE_CURRENT_SOURCE_DIR}/../)

if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.system sysio.system ${SYSIO_SYSTEM_SOURCES})

   target_include_directories(sysio.system
      PUBLIC ${SYSIO_SYSTEM_INCLUDE_DIRS})
endif()

# Native .so for native-module runtime debugging
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES AND BUILD_SYSTEM_CONTRACTS)
   add_native_contract(
      TARGET         sysio.system_native
      SOURCES        ${SYSIO_SYSTEM_SOURCES}
      INCLUDE_DIRS   ${SYSIO_SYSTEM_INCLUDE_DIRS}
      CONTRACT_CLASS "sysiosystem::system_contract"
      HEADERS        ${SYSIO_SYSTEM_HEADERS}
      ABI_FILE       ${CMAKE_BINARY_DIR}/contracts/sysio.system/sysio.system.abi
      EXTRA_DISPATCH "sysiosystem::peer_keys:regpeerkey,delpeerkey,getpeerkeys"
                     "sysiosystem::trx_priority:addtrxp,deltrxp"
                     "sysiosystem::snapshot_attest:regsnapprov,delsnapprov,votesnaphash,setsnpcfg,getsnaphash"
   )
endif()
