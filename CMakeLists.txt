cmake_minimum_required( VERSION 3.16 )

project( wire-sysio )
include(CTest) # suppresses DartConfiguration.tcl error
enable_testing()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules")

include( GNUInstallDirs )
include( MASSigning )

set( CMAKE_CXX_STANDARD 20 )
set( CMAKE_CXX_EXTENSIONS ON )
set( CXX_STANDARD_REQUIRED ON)

set(VERSION_MAJOR 5)
set(VERSION_MINOR 2)
set(VERSION_PATCH 0)
set(VERSION_SUFFIX rc1)

if(VERSION_SUFFIX)
    set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-${VERSION_SUFFIX}")
else()
    set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
endif()


# Options
option(ENABLE_WERROR "Enable `-Werror` compilation flag." Off)
option(ENABLE_WEXTRA "Enable `-Wextra` compilation flag." Off)


set( CLI_CLIENT_EXECUTABLE_NAME clio )
set( NODE_EXECUTABLE_NAME nodeop )
set( KEY_STORE_EXECUTABLE_NAME kiod )
set( SYS_UTIL_EXECUTABLE_NAME sys-util )

# http://stackoverflow.com/a/18369825
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10.2)
        message(FATAL_ERROR "GCC version must be at least 10.2.0!")
    endif()
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

if ("${CMAKE_GENERATOR}" STREQUAL "Ninja")
   add_compile_options(-fdiagnostics-color=always)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS "ON")
set(BUILD_DOXYGEN FALSE CACHE BOOL "Build doxygen documentation on every make")
set(ENABLE_MULTIVERSION_PROTOCOL_TEST FALSE CACHE BOOL "Enable nodeop multiversion protocol test")

option(ENABLE_OC "Enable sysvm-oc on supported platforms" ON)

# WASM runtimes to enable. Each runtime in this list will have:
#  * definition SYSIO_<RUNTIME>_RUNTIME_ENABLED defined in public libchain interface
#  * ctest entries with --runtime
if(ENABLE_OC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT WIN32)
   if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux" AND "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
      list(APPEND SYSIO_WASM_RUNTIMES sys-vm-oc)
      # SYS VM OC requires LLVM, but move the check up here to a central location so that the SysioTester.cmakes
      # can be created with the exact version found
      find_package(LLVM REQUIRED CONFIG)
      if(LLVM_VERSION_MAJOR VERSION_LESS 7)# OR LLVM_VERSION_MAJOR VERSION_GREATER_EQUAL 12)
	      message(FATAL_ERROR "Leap requires an LLVM version 7 through 11")
      endif()
   endif()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT WIN32)
   if(CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64)
      list(APPEND SYSIO_WASM_RUNTIMES sys-vm sys-vm-jit)
   else()
      list(APPEND SYSIO_WASM_RUNTIMES sys-vm)
   endif()
endif()

if(UNIX)
  if(APPLE)
    set(whole_archive_flag "-force_load")
    set(no_whole_archive_flag "")
  else()
    set(whole_archive_flag "--whole-archive")
    set(no_whole_archive_flag "--no-whole-archive")
  endif()
else()
  set(whole_archive_flag "--whole-archive")
  set(no_whole_archive_flag "--no-whole-archive")
endif()

if( APPLE AND UNIX )
# Apple Specific Options Here
    message( STATUS "Configuring Leap on macOS" )
    set( CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Wall -Wno-deprecated-declarations" )
    set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-deprecated-declarations" )
else()
    # Linux Specific Options Here
    message( STATUS "Configuring Leap on Linux" )
    set( CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Wall" )
    set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall" )
    if ( FULL_STATIC_BUILD )
      set( CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -static-libgcc")
    endif ( FULL_STATIC_BUILD )

    if( "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" )
        if( CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL 4.0.0 OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 4.0.0 )
            set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-invalid-partial-specialization" )
        endif()
    endif()
endif()

if(ENABLE_WERROR)
  set( CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Werror" )
  set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror" )
endif()
if(ENABLE_WEXTRA)
  set( CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Wextra" )
  set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wextra" )
endif()


option(SYSIO_ENABLE_DEVELOPER_OPTIONS "enable developer options for Leap" OFF)

# based on http://www.delorie.com/gnu/docs/gdb/gdb_70.html
# uncomment this line to tell GDB about macros (slows compile times)
# set( CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -gdwarf-2 -g3" )

set(ENABLE_COVERAGE_TESTING FALSE CACHE BOOL "Build Leap for code coverage analysis")

if(ENABLE_COVERAGE_TESTING)
  SET(CMAKE_C_FLAGS   "--coverage ${CMAKE_C_FLAGS}")
  SET(CMAKE_CXX_FLAGS "--coverage ${CMAKE_CXX_FLAGS}")
  find_program( LCOV_PATH lcov )
  find_program( LLVMCOV_PATH  llvm-cov )
  find_program( GENHTML_PATH NAMES genhtml)
endif()

include(utils)

if ("${CORE_SYMBOL_NAME}" STREQUAL "")
  set( CORE_SYMBOL_NAME "SYS" )
endif()
string(TOUPPER ${CORE_SYMBOL_NAME} CORE_SYMBOL_NAME)

string(LENGTH ${CORE_SYMBOL_NAME} CORE_SYMBOL_NAME_LENGTH)
if (CORE_SYMBOL_NAME_LENGTH GREATER 7)
  message(FATAL_ERROR "CORE_SYMBOL_NAME length must be between 1 and 7 characters")
endif()

message( STATUS "Using '${CORE_SYMBOL_NAME}' as CORE symbol name" )

if ("${SYSIO_ROOT_KEY}" STREQUAL "")
   set(SYSIO_ROOT_KEY "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV")
endif()

message( STATUS "Using '${SYSIO_ROOT_KEY}' as public key for 'sysio' account" )

option(ENABLE_TCMALLOC "use tcmalloc (requires gperftools)" OFF)
if( ENABLE_TCMALLOC )
    find_package( Gperftools REQUIRED )
    message( STATUS "Compiling Leap with TCMalloc")
    #if doing this by the book, simply link_libraries( ${GPERFTOOLS_TCMALLOC} ) here. That will
    #give the performance benefits of tcmalloc but since it won't be linked last
    #the heap profiler & checker may not be accurate. This here is rather undocumented behavior
    #to stuff a library toward the end of the link list
    set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES} ${GPERFTOOLS_TCMALLOC}")
    set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} ${GPERFTOOLS_TCMALLOC}")
endif()

# sysio includes a bundled BoringSSL which conflicts with OpenSSL. Make sure any other bundled libraries (such as boost)
# do not attempt to use an external OpenSSL in any manner
set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL On)

# many tests require handling of signals themselves and even when they don't we'd like for them to generate a core dump, this
# is effectively --catch_system_errors=no broadly across all tests
add_compile_definitions(BOOST_TEST_DEFAULTS_TO_CORE_DUMP)

add_definitions(-DSYSIO_SYS_VM_RUNTIME_ENABLED)
add_subdirectory( libraries )
add_subdirectory( plugins )
add_subdirectory( programs )
add_subdirectory( scripts )
add_subdirectory( unittests )
add_subdirectory( tests )
add_subdirectory( tools )
add_subdirectory( benchmark )

option(DISABLE_WASM_SPEC_TESTS "disable building of wasm spec unit tests" OFF)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.version.in ${CMAKE_CURRENT_BINARY_DIR}/sysio.version.hpp)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sysio.version.hpp DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL)

set(SYSIO_ROOT_DIR "${CMAKE_BINARY_DIR}/lib")
# legacy sysio CMake files
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/sysio-config.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/sysio/sysio-config.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/SysioTesterBuild.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioTester.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/SysioCheckVersion.cmake ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioCheckVersion.cmake COPYONLY)
# new sysio CMake files
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/sysio-config.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/sysio/sysio-config.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/SysioTesterBuild.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioTester.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/SysioCheckVersion.cmake ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioCheckVersion.cmake COPYONLY)

set(SYSIO_ROOT_DIR "${CMAKE_INSTALL_FULL_LIBDIR}")
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/SysioTester.cmake.in ${CMAKE_BINARY_DIR}/modules/SysioTester.cmake @ONLY)
# legacy sysio CMake files
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/sysio-config.cmake.in ${CMAKE_BINARY_DIR}/modules/sysio-config.cmake @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/modules/sysio-config.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/sysio COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES ${CMAKE_BINARY_DIR}/modules/SysioTester.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/sysio COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES ${CMAKE_SOURCE_DIR}/CMakeModules/SysioCheckVersion.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/sysio COMPONENT dev EXCLUDE_FROM_ALL)
# new sysio CMake files
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/sysio-config.cmake.in ${CMAKE_BINARY_DIR}/modules/sysio-config.cmake @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/modules/sysio-config.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/sysio COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES ${CMAKE_BINARY_DIR}/modules/SysioTester.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/sysio COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES ${CMAKE_SOURCE_DIR}/CMakeModules/SysioCheckVersion.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/sysio COMPONENT dev EXCLUDE_FROM_ALL)

configure_file(LICENSE.md                                                  licenses/sysio/LICENSE.md        COPYONLY)
configure_file(libraries/softfloat/COPYING.txt                             licenses/sysio/LICENSE.softfloat COPYONLY)
configure_file(libraries/wasm-jit/LICENSE                                  licenses/sysio/LICENSE.wavm      COPYONLY)
configure_file(libraries/libfc/secp256k1/secp256k1/COPYING                 licenses/sysio/LICENSE.secp256k1 COPYONLY)
configure_file(libraries/libfc/include/fc/crypto/webauthn_json/license.txt licenses/sysio/LICENSE.rapidjson COPYONLY)
configure_file(libraries/sys-vm/LICENSE                                    licenses/sysio/LICENSE.sys-vm    COPYONLY)
configure_file(libraries/prometheus/prometheus-cpp/LICENSE                 licenses/sysio/LICENSE.prom      COPYONLY)
configure_file(programs/clio/LICENSE.CLI11                                 licenses/sysio/LICENSE.CLI11     COPYONLY)
configure_file(libraries/libfc/libraries/bls12-381/LICENSE                 licenses/sysio/LICENSE.bls12-381 COPYONLY)
configure_file(libraries/libfc/libraries/boringssl/boringssl/src/LICENSE   licenses/sysio/LICENSE.boringssl COPYONLY)

install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/licenses/sysio" DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/" COMPONENT base)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/libraries/testing/contracts DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/leap_testing/libraries/testing COMPONENT dev EXCLUDE_FROM_ALL
        PATTERN "CMakeFiles" EXCLUDE
        PATTERN "*.cmake" EXCLUDE
        PATTERN "Makefile" EXCLUDE)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/unittests/contracts DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/leap_testing/unittests COMPONENT dev EXCLUDE_FROM_ALL
        PATTERN "CMakeFiles" EXCLUDE
        PATTERN "*.cmake" EXCLUDE
        PATTERN "Makefile" EXCLUDE)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests/TestHarness DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/leap_testing/tests COMPONENT dev EXCLUDE_FROM_ALL
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
      COMMAND ${CMAKE_COMMAND} -E create_symlink ../../../share/leap_testing/tests/TestHarness lib/python3/dist-packages/TestHarness
      COMMAND ${CMAKE_COMMAND} -E make_directory share/leap_testing
      COMMAND ${CMAKE_COMMAND} -E create_symlink ../../bin share/leap_testing/bin)

   install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lib/python3/dist-packages/TestHarness DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/python3/dist-packages COMPONENT dev EXCLUDE_FROM_ALL)
   install(FILES ${CMAKE_CURRENT_BINARY_DIR}/share/leap_testing/bin DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/leap_testing COMPONENT dev EXCLUDE_FROM_ALL)
else()
   # The following install(SCRIPT ...) steps are necessary for `make dev-install` to work on cmake versions < 3.21.
   install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/scripts/install_testharness_symlinks.cmake COMPONENT dev EXCLUDE_FROM_ALL)

   # The `make package` installation of symlinks happens via the `postinst` script installed in cmake.package via the line below
endif()

configure_file(${CMAKE_SOURCE_DIR}/libraries/cli11/bash-completion/completions/sys-util
               ${CMAKE_BINARY_DIR}/programs/sys-util/bash-completion/completions/sys-util COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/cli11/bash-completion/completions/clio
               ${CMAKE_BINARY_DIR}/programs/clio/bash-completion/completions/clio COPYONLY)

install(FILES libraries/cli11/bash-completion/completions/sys-util DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/bash-completion/completions COMPONENT base)
install(FILES libraries/cli11/bash-completion/completions/clio DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/bash-completion/completions COMPONENT base)

# Add the boost submodule we used to build to our install package, so headers can be found for libtester
install(DIRECTORY "${CMAKE_SOURCE_DIR}/libraries/boost/"
        DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/leap_boost
        COMPONENT dev EXCLUDE_FROM_ALL
        PATTERN ".git/*" EXCLUDE
        PATTERN "example/*" EXCLUDE
        PATTERN "bench/*" EXCLUDE
        PATTERN "doc/*" EXCLUDE
        PATTERN "libs/*/test" EXCLUDE
        PATTERN "tools/*/test" EXCLUDE
        )

add_custom_target(dev-install
  COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --component dev
  USES_TERMINAL
)

include(doxygen)

option(ENABLE_LEAP_DEV_DEB "Enable building the sysio-dev .deb package" OFF)

include(package.cmake)
include(CPack)
