if(BUILD_SYSTEM_CONTRACTS)
  add_contract(sysio.wrap sysio.wrap sysio.wrap.cpp)
  if(NOT CDT_BUILD)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.wrap.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.wrap.wasm COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.wrap.abi ${CMAKE_CURRENT_BINARY_DIR}/sysio.wrap.abi COPYONLY)
  endif()
endif()

