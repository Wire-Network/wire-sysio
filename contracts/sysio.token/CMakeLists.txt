if( BUILD_SYSTEM_CONTRACTS AND CDT_BUILD)
   add_contract( sysio.token sysio.token sysio.token.cpp )
else()
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/sysio.token.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.token.wasm COPYONLY )
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/sysio.token.abi  ${CMAKE_CURRENT_BINARY_DIR}/sysio.token.abi  COPYONLY )
endif()
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/sysio.token.bad.abi  ${CMAKE_CURRENT_BINARY_DIR}/sysio.token.bad.abi  COPYONLY )

