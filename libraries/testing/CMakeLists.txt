file(GLOB HEADERS "include/sysio/testing/*.hpp")

configure_file(contracts.hpp.in include/testing_contracts/contracts.hpp ESCAPE_QUOTES)
add_library(sysio_testing_contracts INTERFACE)
target_include_directories(sysio_testing_contracts INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/include/testing_contracts)

configure_file(contracts.cpp.in contracts.cpp ESCAPE_QUOTES)

## SORT .cpp by most likely to change / break compile
add_library( sysio_testing
             tester.cpp
             tester_network.cpp
             ${CMAKE_CURRENT_BINARY_DIR}/contracts.cpp
             ${HEADERS}
        )

target_link_libraries( sysio_testing sysio_testing_contracts sysio_chain fc chainbase Logging IR WAST WASM Runtime )
target_include_directories( sysio_testing
                            PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/../wasm-jit/Include"
                                   "${CMAKE_BINARY_DIR}/unittests/include"
                            )

if(MSVC)
  set_source_files_properties( db_init.cpp db_block.cpp database.cpp block_log.cpp PROPERTIES COMPILE_FLAGS "/bigobj" )
endif(MSVC)

set_target_properties( sysio_testing PROPERTIES PUBLIC_HEADER "${HEADERS}" )
install( TARGETS sysio_testing
   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT dev EXCLUDE_FROM_ALL
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}/sysio/testing COMPONENT dev EXCLUDE_FROM_ALL
)