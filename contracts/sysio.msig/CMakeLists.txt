if(BUILD_SYSTEM_CONTRACTS)
  add_contract( sysio.msig sysio.msig sysio.msig.cpp )
endif()

if(NOT BUILD_SYSTEM_CONTRACTS OR NOT CDT_BUILD)
  configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/sysio.msig.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.msig.wasm COPYONLY )
  configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/sysio.msig.abi  ${CMAKE_CURRENT_BINARY_DIR}/sysio.msig.abi  COPYONLY )
endif()
