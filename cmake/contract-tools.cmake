include(CMakeDependentOption)
include(CMakeParseArguments)
include(ExternalProject)

option(BUILD_SYSTEM_CONTRACTS "Build system contracts" OFF)
cmake_dependent_option(BUILD_TEST_CONTRACTS "Build test contracts" OFF "BUILD_SYSTEM_CONTRACTS" OFF)
cmake_dependent_option(CDT_BUILD "Indicates that the CDT is being built" OFF "BUILD_SYSTEM_CONTRACTS" ON)

if(BUILD_SYSTEM_CONTRACTS AND NOT CDT_BUILD)
  set(SYSIO_WASM_OLD_BEHAVIOR "Off")
  find_package(cdt REQUIRED)
  set(
    CDT_CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CDT_ROOT}/lib/cmake/cdt/CDTWasmToolchain.cmake -DCMAKE_PREFIX_PATH=${CMAKE_BINARY_DIR}  -DBUILD_SYSTEM_CONTRACTS=ON -DBUILD_TEST_CONTRACTS=${BUILD_TEST_CONTRACTS} -DCDT_BUILD=ON
  )
  if (CDT_ROOT AND EXISTS ${CDT_ROOT}/lib/cmake/cdt/cdt-config.cmake)
      list(APPEND CDT_CMAKE_ARGS -DCDT_ROOT=${CDT_ROOT})
  endif ()
endif()

# Adds an external project target for building smart contracts
#
# Arguments:
#   TARGET - Name of the contract target to build
#   SOURCE_DIR - Directory containing contract source files
#   BINARY_DIR - Directory for contract build output
#
# This function creates an external project target that builds smart contracts
# using the CDT toolchain. It skips the build if CDT_BUILD is ON or if
# BUILD_SYSTEM_CONTRACTS is OFF.
function(contracts_target TARGET)
  if (CDT_BUILD)
    message(NOTICE "CDT_BUILD is ON. Skipping build of contracts target: ${TARGET}")
    return()
  endif()
  if(NOT BUILD_SYSTEM_CONTRACTS)
    message(NOTICE "BUILD_SYSTEM_CONTRACTS is OFF. Skipping build of contracts target: ${TARGET}")
    return()
  endif()

  cmake_parse_arguments(ARG "" "SOURCE_DIR;BINARY_DIR" "" ${ARGN})
  ExternalProject_Add(
    ${TARGET}
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

