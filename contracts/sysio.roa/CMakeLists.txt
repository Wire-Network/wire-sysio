if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.wasm)
   configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.wasm COPYONLY)
   configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.abi ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.abi COPYONLY)
endif()

if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.roa sysio.roa sysio.roa.cpp)

   configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.wasm COPYONLY)
   configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa.abi ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa.abi COPYONLY)
endif()

