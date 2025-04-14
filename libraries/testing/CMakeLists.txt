### Build contracts with cdt if available ###
include(ExternalProject)

if( SYSIO_COMPILE_TEST_CONTRACTS )
    set(SYSIO_WASM_OLD_BEHAVIOR "Off")
    if( USE_SYSIO_CDT_1_7_X OR USE_SYSIO_CDT_1_8_X )
        find_package( sysio.cdt REQUIRED )
        set(CMAKE_ARGS_VAL -DCMAKE_TOOLCHAIN_FILE=${SYSIO_CDT_ROOT}/lib/cmake/sysio.cdt/SysioWasmToolchain.cmake -DSYSIO_COMPILE_TEST_CONTRACTS=${SYSIO_COMPILE_TEST_CONTRACTS} )
    else()
        find_package( cdt REQUIRED )
        set(CMAKE_ARGS_VAL -DCMAKE_TOOLCHAIN_FILE=${CDT_ROOT}/lib/cmake/cdt/CDTWasmToolchain.cmake -DSYSIO_COMPILE_TEST_CONTRACTS=${SYSIO_COMPILE_TEST_CONTRACTS} )
    endif()

    if( USE_SYSIO_CDT_1_7_X )
        list(APPEND CMAKE_ARGS_VAL -DUSE_SYSIO_CDT_1_7_X=${USE_SYSIO_CDT_1_7_X})
    elseif( USE_SYSIO_CDT_1_8_X )
        list(APPEND CMAKE_ARGS_VAL -DUSE_SYSIO_CDT_1_8_X=${USE_SYSIO_CDT_1_8_X})
    endif()

    message( STATUS "Building contracts in directory `./libraries/contracts/`" )
    ExternalProject_Add(
            testing_contracts_project
            SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/contracts
            BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/contracts
            CMAKE_ARGS ${CMAKE_ARGS_VAL}
            UPDATE_COMMAND ""
            PATCH_COMMAND ""
            TEST_COMMAND ""
            INSTALL_COMMAND ""
            BUILD_ALWAYS 1
    )
else()
    message( STATUS "Not building contracts in directory `./libraries/contracts/`" )
    add_subdirectory(contracts)
endif()

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

target_link_libraries( sysio_testing sysio_testing_contracts sysio_chain fc chainbase Logging IR WAST WASM)
target_include_directories( sysio_testing
                            PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/../wasm-jit/Include"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/contracts"
                                   "${CMAKE_CURRENT_BINARY_DIR}/contracts"
                          )

set_target_properties( sysio_testing PROPERTIES PUBLIC_HEADER "${HEADERS}" )
install( TARGETS sysio_testing
   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT dev EXCLUDE_FROM_ALL
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}/sysio/testing COMPONENT dev EXCLUDE_FROM_ALL
)