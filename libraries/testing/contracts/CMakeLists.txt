cmake_minimum_required( VERSION 3.16 )
project(testing_contracts)

if ("${CMAKE_GENERATOR}" STREQUAL "Ninja")
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fcolor-diagnostics)
    endif()
endif()

if( BUILD_TEST_CONTRACTS )
    set(SYSIO_WASM_OLD_BEHAVIOR "Off")
    find_package( cdt REQUIRED )
endif()



