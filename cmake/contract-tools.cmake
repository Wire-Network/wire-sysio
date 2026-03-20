include(CMakeDependentOption)
include(CMakeParseArguments)
include(ExternalProject)

option(BUILD_SYSTEM_CONTRACTS "Build system contracts" OFF)
cmake_dependent_option(BUILD_TEST_CONTRACTS "Build test contracts" OFF "BUILD_SYSTEM_CONTRACTS" OFF)
cmake_dependent_option(CDT_BUILD "Indicates that the CDT is being built" OFF "BUILD_SYSTEM_CONTRACTS" ON)

message(STATUS "CDT_ROOT is set to: ${CDT_ROOT}")

if(BUILD_SYSTEM_CONTRACTS AND NOT CDT_BUILD)
  set(SYSIO_WASM_OLD_BEHAVIOR "Off")
  find_package(cdt REQUIRED)
  set(
    CDT_CMAKE_ARGS -DCDT_ROOT=${CDT_ROOT} -DCMAKE_TOOLCHAIN_FILE=${CDT_ROOT}/lib/cmake/cdt/CDTWasmToolchain.cmake -DCMAKE_PREFIX_PATH=${CMAKE_BINARY_DIR}  -DBUILD_SYSTEM_CONTRACTS=ON -DBUILD_TEST_CONTRACTS=${BUILD_TEST_CONTRACTS} -DCDT_BUILD=ON
  )
  if (CDT_ROOT AND EXISTS ${CDT_ROOT}/lib/cmake/cdt/cdt-config.cmake)
      list(APPEND CDT_CMAKE_ARGS -DCDT_ROOT=${CDT_ROOT})
  endif ()
endif()

# Global properties to track ExternalProject compile command databases for merging.
set_property(GLOBAL PROPERTY _CONTRACT_COMPILE_COMMANDS_INPUTS "")
set_property(GLOBAL PROPERTY _CONTRACT_COMPILE_COMMANDS_TARGETS "")

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

  # Collect CMakeLists.txt files so the parent build re-runs cmake when
  # contract build structure changes (e.g. new contracts added/removed).
  file(GLOB_RECURSE _contract_cmake_files "${ARG_SOURCE_DIR}/CMakeLists.txt"
                                          "${ARG_SOURCE_DIR}/*/CMakeLists.txt")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_contract_cmake_files})

  # Also collect source files so the ExternalProject reconfigures when
  # contract code changes on branch switch.  Without this, stale object
  # files and ABIs from a previous branch persist and cause link errors
  # (e.g. undefined symbols for actions that no longer exist).
  # These are only added to the check_reconfigure step DEPENDS below,
  # NOT to CMAKE_CONFIGURE_DEPENDS, to avoid re-running the parent cmake
  # on every source edit.
  file(GLOB_RECURSE _contract_source_files "${ARG_SOURCE_DIR}/*.cpp"
                                           "${ARG_SOURCE_DIR}/*.hpp"
                                           "${ARG_SOURCE_DIR}/*.h")

  ExternalProject_Add(
    ${TARGET}
    SOURCE_DIR ${ARG_SOURCE_DIR}
    BINARY_DIR ${ARG_BINARY_DIR}
    CMAKE_ARGS ${CDT_CMAKE_ARGS} -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    TEST_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_ALWAYS 1
  )

  # Force the ExternalProject to re-configure when any contract file changes
  # (e.g. source files added/removed or modified on a different branch).
  ExternalProject_Add_Step(${TARGET} check_reconfigure
    DEPENDEES download
    DEPENDERS configure
    DEPENDS ${_contract_cmake_files} ${_contract_source_files}
    COMMENT "Checking if ${TARGET} needs reconfiguration"
  )

  # When contract headers change (e.g. branch switch adding/removing
  # [[sysio::action]] attributes), the inner build's cached dependency
  # info may be stale, leaving .obj files that reference removed symbols
  # and preventing ABI/native-dispatch regeneration.  Force a clean build
  # so everything is compiled fresh from the updated headers.
  file(GLOB_RECURSE _contract_header_files
    "${ARG_SOURCE_DIR}/*.hpp"
    "${ARG_SOURCE_DIR}/*.h")

  if(_contract_header_files)
    ExternalProject_Add_Step(${TARGET} force_clean_on_header_change
      DEPENDEES configure
      DEPENDERS build
      DEPENDS ${_contract_header_files}
      COMMAND ${CMAKE_COMMAND} --build "${ARG_BINARY_DIR}" --target clean
      COMMENT "Contract headers changed – cleaning build for ${TARGET}"
    )
  endif()

  # Expose the build step as a separate target for better dependency tracking
  ExternalProject_Add_StepTargets(${TARGET} build)

  # Track this ExternalProject for compile_commands.json merging
  set_property(GLOBAL APPEND PROPERTY _CONTRACT_COMPILE_COMMANDS_INPUTS
    "${ARG_BINARY_DIR}/compile_commands.json")
  set_property(GLOBAL APPEND PROPERTY _CONTRACT_COMPILE_COMMANDS_TARGETS
    "${TARGET}")
endfunction()

# Call after all contracts_target() invocations and add_subdirectory() calls.
# Creates a custom target that merges all ExternalProject compile_commands.json
# files with the root project's compile_commands.json so that IDEs can navigate
# contract sources built with the CDT toolchain.
function(add_merged_compile_commands_target)
  # Ninja always generates compile_commands.json regardless of CMAKE_EXPORT_COMPILE_COMMANDS.
  # Enable merging when either the variable is set or we're using a Ninja generator.
  if(NOT CMAKE_EXPORT_COMPILE_COMMANDS AND NOT CMAKE_GENERATOR MATCHES "Ninja")
    return()
  endif()

  get_property(_inputs GLOBAL PROPERTY _CONTRACT_COMPILE_COMMANDS_INPUTS)
  get_property(_targets GLOBAL PROPERTY _CONTRACT_COMPILE_COMMANDS_TARGETS)
  if(NOT _inputs)
    return()
  endif()

  set(_root_cc "${CMAKE_BINARY_DIR}/compile_commands.json")
  set(_merge_script "${CMAKE_SOURCE_DIR}/cmake/scripts/merge_compile_commands.py")

  # The merge script reads the root compile_commands.json (regenerated by Ninja
  # on every build) plus each ExternalProject's database, de-duplicates by
  # (directory, file) key, and writes the result back to compile_commands.json.
  set(_merge_args -o "${_root_cc}" "${_root_cc}")
  foreach(_cc IN LISTS _inputs)
    list(APPEND _merge_args "${_cc}")
  endforeach()

  add_custom_target(merge_compile_commands ALL
    COMMAND python3 "${_merge_script}" ${_merge_args}
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Merging compile_commands.json from ExternalProject builds"
    VERBATIM
  )

  # Ensure merge runs after all ExternalProject builds complete
  foreach(_target IN LISTS _targets)
    add_dependencies(merge_compile_commands ${_target})
  endforeach()
endfunction()
