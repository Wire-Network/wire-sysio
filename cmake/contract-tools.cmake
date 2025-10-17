
include(CMakeParseArguments)
include(ExternalProject)
include(CMakeDependentOption)


option(BUILD_SYSTEM_CONTRACTS "Build system contracts" ON)
cmake_dependent_option(BUILD_TEST_CONTRACTS "Build test contracts" ON "BUILD_SYSTEM_CONTRACTS" ON)
cmake_dependent_option(CDT_BUILD "Indicates that the CDT is being built" OFF "BUILD_SYSTEM_CONTRACTS" ON)

if(BUILD_SYSTEM_CONTRACTS AND NOT CDT_BUILD)
  set(SYSIO_WASM_OLD_BEHAVIOR "Off")
  find_package(cdt REQUIRED)
  set(
    CDT_CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CDT_ROOT}/lib/cmake/cdt/CDTWasmToolchain.cmake -DCMAKE_PREFIX_PATH=${CMAKE_BINARY_DIR}  -DBUILD_SYSTEM_CONTRACTS=ON -DCDT_BUILD=ON
  )
endif()

function(contracts_target targetName)
  if (CDT_BUILD)
    message(NOTICE "CDT_BUILD is ON. Skipping build of contracts target: ${targetName}")
    return()
  endif()
  if(NOT BUILD_SYSTEM_CONTRACTS)
    message(NOTICE "BUILD_SYSTEM_CONTRACTS is OFF. Skipping build of contracts target: ${targetName}")
    return()
  endif()

  cmake_parse_arguments(ARG "" "SOURCE_DIR;BINARY_DIR" "" ${ARGN})
  ExternalProject_Add(
    ${targetName}
    SOURCE_DIR ${ARG_SOURCE_DIR}
    BINARY_DIR ${ARG_BINARY_DIR}
    CMAKE_ARGS ${CDT_CMAKE_ARGS}
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    TEST_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_ALWAYS 1
  )
endfunction()

