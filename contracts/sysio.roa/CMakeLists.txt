if(BUILD_SYSTEM_CONTRACTS)
   # When building from source, copy pre-built contracts only if they don't exist (bootstrap)
   if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.wasm)
      configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.wasm COPYONLY)
   endif()
   if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.abi)
      configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.abi ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.abi COPYONLY)
   endif()
   add_contract(sysio.roa sysio.roa sysio.roa.cpp)

   target_include_directories(sysio.roa
           PUBLIC
           ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include)
else()
   # When not building from source, always copy pre-built contracts (ensures fresh copy on branch switch)
   configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.wasm COPYONLY)
   configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.abi ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.abi COPYONLY)
endif()

