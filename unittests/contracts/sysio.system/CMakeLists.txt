if( SYSIO_COMPILE_TEST_CONTRACTS )
   add_contract( sysio.system sysio.system sysio.system.cpp )
   target_include_directories(sysio.system
           PUBLIC
           ${CMAKE_CURRENT_SOURCE_DIR}/include
           ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.token)
else()
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/sysio.system.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.wasm COPYONLY )
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/sysio.system.abi  ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.abi  COPYONLY )
endif()
