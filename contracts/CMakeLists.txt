cmake_minimum_required( VERSION 3.8 )
project(contracts)

if( SYSIO_COMPILE_TEST_CONTRACTS )
    set(SYSIO_WASM_OLD_BEHAVIOR "Off")
    find_package( cdt REQUIRED )
endif()

if ("${CMAKE_GENERATOR}" STREQUAL "Ninja")
    add_compile_options(-fcolor-diagnostics)
endif()

add_subdirectory(sysio.bios)
add_subdirectory(sysio.msig)
add_subdirectory(sysio.roa)
add_subdirectory(sysio.system)
add_subdirectory(sysio.token)
add_subdirectory(sysio.wrap)
add_subdirectory(tests)