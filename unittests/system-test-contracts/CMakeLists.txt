cmake_minimum_required( VERSION 3.8 )
project(system_test_contracts)


if( BUILD_TEST_CONTRACTS )
    set(SYSIO_WASM_OLD_BEHAVIOR "Off")
    find_package( cdt REQUIRED )
endif()

if ("${CMAKE_GENERATOR}" STREQUAL "Ninja")
    add_compile_options(-fcolor-diagnostics)
endif()

add_subdirectory(sysio.mechanics)
add_subdirectory(jumborow)
