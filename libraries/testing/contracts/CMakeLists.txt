cmake_minimum_required( VERSION 3.8 )
project(testing_contracts)


if ("${CMAKE_GENERATOR}" STREQUAL "Ninja")
    add_compile_options(-fcolor-diagnostics)
endif()

if( BUILD_TEST_CONTRACTS )
    set(SYSIO_WASM_OLD_BEHAVIOR "Off")
    find_package( cdt REQUIRED )
    add_subdirectory(dancer)
    
endif()



