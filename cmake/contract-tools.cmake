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

  # Expose the build step as a separate target for better dependency tracking
  ExternalProject_Add_StepTargets(${TARGET} build)
endfunction()

# ── Native contract compilation (for native-module runtime) ───────────────
#
# Compiles contract sources with the HOST compiler (not CDT's WASM toolchain)
# to produce a shared library (.so) that can be dlopen'd by the native_module
# runtime for debugging with GDB/LLDB/IDE debuggers.
#
# Usage:
#   native_contract_target(
#     TARGET         sysio.bios_native
#     SOURCES        sysio.bios.cpp
#     INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}
#     CONTRACT_CLASS "sysiobios::bios"
#     HEADERS        "sysio.bios.hpp"
#     ABI_TARGET     sysio.bios
#   )
#
# For contracts with actions on multiple classes:
#   native_contract_target(
#     ...
#     CONTRACT_CLASS "sysiosystem::system_contract"
#     HEADERS        "sysio.system/sysio.system.hpp"
#                    "sysio.system/peer_keys.hpp"
#                    "sysio.system/trx_priority.hpp"
#     EXTRA_DISPATCH "sysiosystem::peer_keys:regpeerkey,delpeerkey,getpeerkeys"
#                    "sysiosystem::trx_priority:addtrxp,deltrxp"
#   )
#
# CONTRACT_CLASS + HEADERS + ABI_TARGET: The dispatch .cpp is auto-generated
# from the ABI at build time. No hand-written dispatch file is needed.
#
# The resulting .so exports an `apply(uint64_t, uint64_t, uint64_t)` function.
# Intrinsic symbols (db_store_i64, etc.) are left undefined and resolved at
# dlopen time against the test executable's --dynamic-list exports.

set(NATIVE_CDT_DIR "" CACHE PATH
  "Path to wire-cdt source tree root for native contract compilation")

# Clean up stale native .so files from previous builds when NATIVE_CDT_DIR
# is no longer set (e.g. developer removed it from their CMake config).
if(NOT NATIVE_CDT_DIR)
  file(GLOB_RECURSE _stale_native_sos "${CMAKE_BINARY_DIR}/contracts/*_native.so")
  if(_stale_native_sos)
    message(STATUS "Cleaning stale native contract .so files from previous build")
    file(REMOVE ${_stale_native_sos})
  endif()
endif()
function(native_contract_target)
  cmake_parse_arguments(ARG "" "TARGET;CONTRACT_CLASS;ABI_TARGET"
    "SOURCES;INCLUDE_DIRS;HEADERS;EXTRA_DISPATCH" ${ARGN})

  if(NOT ARG_TARGET)
    message(FATAL_ERROR "native_contract_target: TARGET is required")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "native_contract_target: SOURCES is required")
  endif()

  # Auto-generate dispatch .cpp from ABI when CONTRACT_CLASS + HEADERS + ABI_TARGET are given
  if(ARG_CONTRACT_CLASS AND ARG_HEADERS AND ARG_ABI_TARGET)
    set(_gen_script "${CMAKE_SOURCE_DIR}/scripts/gen_native_dispatch.py")
    set(_abi_file "${CMAKE_BINARY_DIR}/contracts/${ARG_ABI_TARGET}/${ARG_ABI_TARGET}.abi")
    set(_dispatch_cpp "${CMAKE_CURRENT_BINARY_DIR}/${ARG_TARGET}_dispatch.cpp")

    set(_gen_args
      --abi "${_abi_file}"
      --contract-class "${ARG_CONTRACT_CLASS}"
      -o "${_dispatch_cpp}"
    )
    foreach(_hdr ${ARG_HEADERS})
      list(APPEND _gen_args --header "${_hdr}")
    endforeach()
    foreach(_extra ${ARG_EXTRA_DISPATCH})
      list(APPEND _gen_args --extra-dispatch "${_extra}")
    endforeach()

    add_custom_command(
      OUTPUT "${_dispatch_cpp}"
      COMMAND python3 "${_gen_script}" ${_gen_args}
      DEPENDS "${_gen_script}" "${_abi_file}"
      COMMENT "Generating native dispatch for ${ARG_TARGET} from ${ARG_ABI_TARGET}.abi"
      VERBATIM
    )

    list(APPEND ARG_SOURCES "${_dispatch_cpp}")
  endif()

  # Validate CDT source tree
  if(NOT NATIVE_CDT_DIR OR NOT IS_DIRECTORY "${NATIVE_CDT_DIR}/libraries/sysiolib")
    message(FATAL_ERROR "native_contract_target: CDT source tree not found.\n"
      "Set NATIVE_CDT_DIR to the wire-cdt source root.\n"
      "Example: -DNATIVE_CDT_DIR=/path/to/wire-cdt")
  endif()

  set(_sysiolib "${NATIVE_CDT_DIR}/libraries/sysiolib")

  # CDT's own sysiolib source files provide C++ wrappers (sha256,
  # set_blockchain_parameters, base64, etc.) that are normally compiled into
  # WASM contracts.  We compile them with the host compiler directly.
  set(_cdt_sources
    "${_sysiolib}/crypto.cpp"
    "${_sysiolib}/sysiolib.cpp"
    "${_sysiolib}/base64.cpp"
  )

  # Build as a MODULE library (dlopen-able, no lib prefix)
  add_library(${ARG_TARGET} MODULE ${ARG_SOURCES} ${_cdt_sources})
  set_target_properties(${ARG_TARGET} PROPERTIES
    PREFIX ""                     # no "lib" prefix
    OUTPUT_NAME "${ARG_TARGET}"
  )

  # CDT include paths from source tree
  target_include_directories(${ARG_TARGET} PRIVATE
    ${ARG_INCLUDE_DIRS}
    "${_sysiolib}"
    "${_sysiolib}/core"
    "${_sysiolib}/contracts"
    "${_sysiolib}/capi"
    "${_sysiolib}/native"
    "${NATIVE_CDT_DIR}/libraries/meta_refl/include"
  )

  # Compile definitions for native mode
  # uint128_t / int128_t are defined in CDT's libc/bits/stdint.h but we can't
  # include CDT's libc (conflicts with host libc), so define them directly.
  target_compile_definitions(${ARG_TARGET} PRIVATE
    __sysio_cdt_native__
    SYSIO_NATIVE
    "uint128_t=unsigned __int128"
    "int128_t=__int128"
  )

  # Suppress unknown-attribute warnings from __attribute__((sysio_wasm_import))
  # Force-include standard headers since CDT headers expect standard integer types
  target_compile_options(${ARG_TARGET} PRIVATE
    -Wno-unknown-attributes
    -fPIC
    "SHELL:-include cstdint"
    "SHELL:-include cstdlib"
    "SHELL:-include cstring"
    "SHELL:-include memory"
  )

  # Allow undefined symbols — intrinsics are resolved at dlopen time.
  # Link against shared libgcc_s and libstdc++ to ensure C++ exceptions can
  # propagate across the .so boundary. The host executable must also use
  # shared libgcc_s (see test-tools.cmake).
  if(APPLE)
    target_link_options(${ARG_TARGET} PRIVATE -undefined dynamic_lookup)
  else()
    target_link_options(${ARG_TARGET} PRIVATE -Wl,--allow-shlib-undefined)
    target_link_libraries(${ARG_TARGET} PRIVATE gcc_s stdc++)
  endif()
endfunction()

