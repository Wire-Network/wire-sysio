cmake_minimum_required(VERSION 3.19)

# CMake 4 rejects dependency config files that still declare compatibility
# with policy versions older than 3.5. Some vcpkg packages used by WIRE still
# ship those legacy config files, so provide a floor while upstream ports catch
# up.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Select the same libc++ ABI for the application and vcpkg before project()
# enables languages or installs dependencies. Contract builds use a separate
# CDT toolchain and do not execute this host project.
if(WIRE_LLVM_ROOT)
  # A cached/-D prefix must also reach vcpkg's isolated port processes.
  set(ENV{WIRE_LLVM_ROOT} "${WIRE_LLVM_ROOT}")
endif()
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/llvm-toolchain.cmake"
    CACHE FILEPATH "Host LLVM/libc++ toolchain")
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/.github/vcpkg-triplets"
    CACHE PATH "Release-only dependency triplets")
if(NOT VCPKG_TARGET_TRIPLET)
  # Host processor variables are not populated until project() on a fresh build.
  cmake_host_system_information(RESULT CMAKE_HOST_SYSTEM_PROCESSOR QUERY OS_PLATFORM)
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_wire_host_arch arm64)
  elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(_wire_host_arch x64)
  else()
    message(FATAL_ERROR "Specify a supported VCPKG_TARGET_TRIPLET for ${CMAKE_HOST_SYSTEM_PROCESSOR}")
  endif()
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(VCPKG_TARGET_TRIPLET "${_wire_host_arch}-osx-release" CACHE STRING "vcpkg target")
  else()
    set(VCPKG_TARGET_TRIPLET "${_wire_host_arch}-linux-release" CACHE STRING "vcpkg target")
  endif()
endif()
set(VCPKG_HOST_TRIPLET "${VCPKG_TARGET_TRIPLET}" CACHE STRING "vcpkg host")

# Set vcpkg toolchain if not already set
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
endif()


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS ON)
set(CXX_STANDARD_REQUIRED ON)

set(VERSION_MAJOR 1)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(VERSION_SUFFIX "")

if(VERSION_SUFFIX)
  set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-${VERSION_SUFFIX}")
else()
  set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
endif()

project(wire-sysio)

include(cmake/check-libcxx.cmake)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
  message(FATAL_ERROR "libc++ 23 requires GCC 15 or later")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(build-options NO_POLICY_SCOPE)
include(build-init NO_POLICY_SCOPE)
include(compiler-config NO_POLICY_SCOPE)
include(dependencies NO_POLICY_SCOPE)
include(linker-config NO_POLICY_SCOPE)
include(hardening-config NO_POLICY_SCOPE)
include(plugin-tools NO_POLICY_SCOPE)
include(chain-tools NO_POLICY_SCOPE)
include(test-tools NO_POLICY_SCOPE)
include(contract-tools NO_POLICY_SCOPE)

include(CTest) # suppresses DartConfiguration.tcl error
enable_testing()

include(GNUInstallDirs)
include(test-helpers)
include(boost-test-discovery)
add_p_test(NAME boost_test_discovery_test
           COMMAND ${CMAKE_COMMAND} -DGENERATOR=${CMAKE_GENERATOR} -DCXX=${CMAKE_CXX_COMPILER} -DCONFIG=$<CONFIG>
                   -P ${CMAKE_SOURCE_DIR}/cmake/test/boost-test-discovery-test.cmake)

set(CLI_CLIENT_EXECUTABLE_NAME clio)
set(NODE_EXECUTABLE_NAME nodeop)
set(KEY_STORE_EXECUTABLE_NAME kiod)
set(SYS_UTIL_EXECUTABLE_NAME sys-util)

# http://stackoverflow.com/a/18369825
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11.4)
    message(FATAL_ERROR "GCC version must be at least 11.4.0!")
  endif()
endif()

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

if("${CMAKE_GENERATOR}" STREQUAL "Ninja")
  add_compile_options(-fdiagnostics-color=always)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS "ON")

# WASM runtimes to enable. Each runtime in this list will have:
#  * definition SYSIO_<RUNTIME>_RUNTIME_ENABLED defined in public libchain interface
#  * ctest entries with --runtime
if(ENABLE_OC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT WIN32)
  if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux" AND "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
    list(APPEND SYSIO_WASM_RUNTIMES sys-vm-oc)
  endif()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT WIN32)
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64 OR
     (APPLE AND (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64)))
    list(APPEND SYSIO_WASM_RUNTIMES sys-vm sys-vm-jit)
  else()
    list(APPEND SYSIO_WASM_RUNTIMES sys-vm)
  endif()
endif()

# Native-module runtime: only in Debug builds on hosts that can load native contract modules.
if(DEBUG_ENABLED AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
   (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
  list(APPEND SYSIO_WASM_RUNTIMES native-module)
endif()


# based on http://www.delorie.com/gnu/docs/gdb/gdb_70.html
include(cmake/native-libcxx.cmake)

# uncomment this line to tell GDB about macros (slows compile times)
# set( CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -gdwarf-2 -g3" )

if(ENABLE_COVERAGE_TESTING)
  set(CMAKE_C_FLAGS "--coverage ${CMAKE_C_FLAGS}")
  set(CMAKE_CXX_FLAGS "--coverage ${CMAKE_CXX_FLAGS}")
  find_program(LCOV_PATH lcov)
  find_program(LLVMCOV_PATH llvm-cov)
  find_program(GENHTML_PATH NAMES genhtml)
endif()

include(utils)

if("${CORE_SYMBOL_NAME}" STREQUAL "")
  set(CORE_SYMBOL_NAME "SYS")
endif()
string(TOUPPER ${CORE_SYMBOL_NAME} CORE_SYMBOL_NAME)

string(LENGTH ${CORE_SYMBOL_NAME} CORE_SYMBOL_NAME_LENGTH)
if(CORE_SYMBOL_NAME_LENGTH GREATER 7)
  message(FATAL_ERROR "CORE_SYMBOL_NAME length must be between 1 and 7 characters")
endif()

message(STATUS "Using '${CORE_SYMBOL_NAME}' as CORE symbol name")

# sysio includes a bundled BoringSSL which conflicts with OpenSSL. Make sure any other bundled libraries (such as boost)
# do not attempt to use an external OpenSSL in any manner
set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL On)

# many tests require handling of signals themselves and even when they don't we'd like for them to generate a core dump, this
# is effectively --catch_system_errors=no broadly across all tests
add_compile_definitions(BOOST_TEST_DEFAULTS_TO_CORE_DUMP)
# softfloat's legacy float32_t/float64_t aliases collide with arm_neon.h, which Boost.Unordered pulls in on arm64;
# nothing here uses them.
add_compile_definitions(SOFTFLOAT_DISABLE_LEGACY_TYPE_ALIASES)

add_definitions(-DSYSIO_SYS_VM_RUNTIME_ENABLED)

contracts_target(
        contracts_project
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/contracts
        BINARY_DIR ${CMAKE_BINARY_DIR}/contracts
)

add_subdirectory(libraries)
add_subdirectory(plugins)
add_subdirectory(programs)
add_subdirectory(scripts)
add_subdirectory(unittests)
add_subdirectory(tests)
add_subdirectory(tools)
add_subdirectory(benchmark)

add_subdirectory(contracts)
# CDT's native MODULE helper predates the host libc++ ABI. Remove only its
# explicit GNU runtime libraries; WASM targets and CDT itself remain unchanged.
function(wire_native_contract_runtime directory)
  get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _targets)
    get_target_property(_type "${_target}" TYPE)
    if(_type STREQUAL "MODULE_LIBRARY")
      get_target_property(_libraries "${_target}" LINK_LIBRARIES)
      list(REMOVE_ITEM _libraries gcc_s stdc++)
      set_property(TARGET "${_target}" PROPERTY LINK_LIBRARIES "${_libraries}")
    endif()
  endforeach()
  get_property(_directories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(_directory IN LISTS _directories)
    wire_native_contract_runtime("${_directory}")
  endforeach()
endfunction()
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
  wire_native_contract_runtime("${CMAKE_CURRENT_SOURCE_DIR}/contracts")
  # Make Debug packages portable without depending on the build host's prefix.
  file(GLOB _wire_shared_runtime_files "${_wire_runtime_dir}/libc++.so*"
       "${_wire_runtime_dir}/libc++abi.so*" "${_wire_runtime_dir}/libunwind.so*"
       "${_wire_runtime_dir}/libc++*.dylib" "${_wire_runtime_dir}/libunwind*.dylib")
  install(FILES ${_wire_shared_runtime_files} DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT base)
endif()
add_subdirectory(contracts/tests)

if(TARGET test_chain_plugin AND TARGET contracts_project)
  add_dependencies(test_chain_plugin contracts_project)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.version.in ${CMAKE_CURRENT_BINARY_DIR}/sysio.version.hpp)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sysio.version.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev)

set(SYSIO_ROOT_DIR "${CMAKE_INSTALL_FULL_LIBDIR}")
configure_file(${CMAKE_SOURCE_DIR}/cmake/sysio-config.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/sysio/sysio-config.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/cmake/SysioTester.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioTester.cmake @ONLY)
configure_file(cmake/llvm-toolchain.cmake lib/cmake/sysio/llvm-toolchain.cmake COPYONLY)
configure_file(cmake/check-libcxx.cmake lib/cmake/sysio/check-libcxx.cmake COPYONLY)
configure_file(cmake/native-libcxx.cmake lib/cmake/sysio/native-libcxx.cmake COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/cmake/SysioCheckVersion.cmake ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioCheckVersion.cmake COPYONLY)

install(FILES ${CMAKE_BINARY_DIR}/lib/cmake/sysio/sysio-config.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sysio COMPONENT dev)
install(FILES ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioTester.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sysio COMPONENT dev)
install(FILES cmake/llvm-toolchain.cmake cmake/check-libcxx.cmake cmake/native-libcxx.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sysio COMPONENT dev)
install(FILES ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioCheckVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sysio COMPONENT dev)

include(cmake/licenses.cmake)

# Merge compile_commands.json from ExternalProject contract builds with the root
# project's database so that IDEs (CLion, clangd) can navigate contract sources.
add_merged_compile_commands_target()

install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/licenses/sysio" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/" COMPONENT base)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/libraries/testing/contracts DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/sysio_testing/libraries/testing OPTIONAL COMPONENT dev EXCLUDE_FROM_ALL
  PATTERN "CMakeFiles" EXCLUDE
  PATTERN "*.cmake" EXCLUDE
  PATTERN "Makefile" EXCLUDE)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/unittests/system-test-contracts DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/sysio_testing/unittests OPTIONAL COMPONENT dev EXCLUDE_FROM_ALL
  PATTERN "CMakeFiles" EXCLUDE
  PATTERN "*.cmake" EXCLUDE
  PATTERN "Makefile" EXCLUDE)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests/TestHarness DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/sysio_testing/tests OPTIONAL COMPONENT dev EXCLUDE_FROM_ALL
  FILES_MATCHING
  PATTERN "*.py"
  PATTERN "*.json"
  PATTERN "__pycache__" EXCLUDE
  PATTERN "CMakeFiles" EXCLUDE)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
  # Cmake versions < 3.21 did not support installing symbolic links to a directory via install(FILES ...)
  # Cmake 3.21 fixed this (https://gitlab.kitware.com/cmake/cmake/-/commit/d71a7cc19d6e03f89581efdaa8d63db216184d40)
  # If/when the lowest cmake version supported becomes >= 3.21, the else block as well as the postinit and prerm scripts
  # would be a good option to remove in favor of using the facilities in cmake / cpack directly.

  add_custom_target(link_target ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory lib/python3/dist-packages
    COMMAND ${CMAKE_COMMAND} -E create_symlink ../../../share/sysio_testing/tests/TestHarness lib/python3/dist-packages/TestHarness
    COMMAND ${CMAKE_COMMAND} -E make_directory share/sysio_testing
    COMMAND ${CMAKE_COMMAND} -E create_symlink ../../bin share/sysio_testing/bin)

  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lib/python3/dist-packages/TestHarness DESTINATION ${CMAKE_INSTALL_LIBDIR}/python3/dist-packages COMPONENT dev EXCLUDE_FROM_ALL)
  install(PROGRAMS
          ${CMAKE_CURRENT_BINARY_DIR}/bin/clio
          ${CMAKE_CURRENT_BINARY_DIR}/bin/kiod
          ${CMAKE_CURRENT_BINARY_DIR}/bin/nodeop
          ${CMAKE_CURRENT_BINARY_DIR}/bin/sys-util
          ${CMAKE_CURRENT_BINARY_DIR}/bin/trace_api_util
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/sysio_testing/bin
          COMPONENT dev
          EXCLUDE_FROM_ALL)
else()
  # The following install(SCRIPT ...) steps are necessary for `make dev-install` to work on cmake versions < 3.21.
  install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/scripts/install_testharness_symlinks.cmake COMPONENT dev EXCLUDE_FROM_ALL)

  # The `make package` installation of symlinks happens via the `postinst` script installed in cmake.package via the line below
endif()

# Note: CLI11 bash completion files are no longer available since moving to vcpkg
# The completions would need to be created separately or copied from the original CLI11 repository

# Add the boost submodule we used to build to our install package, so headers can be found for libtester
# install(DIRECTORY "${CMAKE_SOURCE_DIR}/libraries/boost/"
#   DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/leap_boost
#   COMPONENT dev EXCLUDE_FROM_ALL
#   PATTERN ".git/*" EXCLUDE
#   PATTERN "example/*" EXCLUDE
#   PATTERN "bench/*" EXCLUDE
#   PATTERN "doc/*" EXCLUDE
#   PATTERN "libs/*/test" EXCLUDE
#   PATTERN "tools/*/test" EXCLUDE
# )

# add_custom_target(dev-install
#   COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
#   COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
#   COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --component dev
#   USES_TERMINAL
# )

include(doxygen)

include(cmake/package.cmake)
