cmake_minimum_required(VERSION 3.8)

project(wire-sysio)
include(CTest) # Suppresses DartConfiguration.tcl error
enable_testing()


list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules")

include(GNUInstallDirs)
include(MASSigning)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)
set(CXX_STANDARD_REQUIRED ON)

set(VERSION_MAJOR 3)
set(VERSION_MINOR 1)
set(VERSION_PATCH 6)
#set(VERSION_SUFFIX rc4)

if(VERSION_SUFFIX)
    set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-${VERSION_SUFFIX}")
else()
    set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
endif()

set(CLI_CLIENT_EXECUTABLE_NAME clio)
set(NODE_EXECUTABLE_NAME nodeop)
set(KEY_STORE_EXECUTABLE_NAME kiod)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8.0)
        message(FATAL_ERROR "GCC version must be at least 8.0!")
    endif()
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5.0)
        message(FATAL_ERROR "Clang version must be at least 5.0!")
    endif()
endif()

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

if("${CMAKE_GENERATOR}" STREQUAL "Ninja")
   add_compile_options(-fdiagnostics-color=always)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS "ON")
set(BUILD_DOXYGEN FALSE CACHE BOOL "Build doxygen documentation on every make")
set(ENABLE_MULTIVERSION_PROTOCOL_TEST FALSE CACHE BOOL "Enable nodeop multiversion protocol test")

# add defaults for openssl
if(APPLE AND UNIX AND "${OPENSSL_ROOT_DIR}" STREQUAL "")
   set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3;/usr/local/opt/openssl@1.1")
endif()
add_definitions(-DOPENSSL_API_COMPAT=0x10100000L -DOPENSSL_NO_DEPRECATED)

option(ENABLE_OC "Enable eosvm-oc on supported platforms" ON)

# WASM runtimes to enable
if(ENABLE_OC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT WIN32)
   if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux" AND "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
      list(APPEND SYSIO_WASM_RUNTIMES sys-vm-oc)
      find_package(LLVM REQUIRED CONFIG)
      if(LLVM_VERSION_MAJOR VERSION_LESS 7 OR LLVM_VERSION_MAJOR VERSION_GREATER_EQUAL 12)
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

set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_LIBS ON CACHE STRING "ON or OFF")
find_package(Boost 1.67 REQUIRED COMPONENTS program_options unit_test_framework system)

if(APPLE AND UNIX)
    message(STATUS "Configuring Wire-Sysio on macOS")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wno-deprecated-declarations")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-deprecated-declarations")
else()
    message(STATUS "Configuring Wire-Sysio on Linux")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
    if(FULL_STATIC_BUILD)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -static-libgcc")
    endif(FULL_STATIC_BUILD)
endif()

option(SYSIO_ENABLE_DEVELOPER_OPTIONS "Enable developer options for Wire-Sysio" OFF)

set(ENABLE_COVERAGE_TESTING FALSE CACHE BOOL "Build Wire-Sysio for code coverage analysis")

if(ENABLE_COVERAGE_TESTING)
  SET(CMAKE_C_FLAGS "--coverage ${CMAKE_C_FLAGS}")
  SET(CMAKE_CXX_FLAGS "--coverage ${CMAKE_CXX_FLAGS}")
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

if("${SYSIO_ROOT_KEY}" STREQUAL "")
   set(SYSIO_ROOT_KEY "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV")
endif()

message(STATUS "Using '${SYSIO_ROOT_KEY}' as public key for 'sysio' account")

find_package(Gperftools QUIET)
if(GPERFTOOLS_FOUND)
    message(STATUS "Found gperftools; compiling Wire-Sysio with TCMalloc")
    set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES} ${GPERFTOOLS_TCMALLOC}")
    set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} ${GPERFTOOLS_TCMALLOC}")
endif()

add_definitions(-DSYSIO_SYS_VM_RUNTIME_ENABLED)
add_subdirectory(libraries)
add_subdirectory(plugins)
add_subdirectory(programs)
add_subdirectory(scripts)
add_subdirectory(unittests)
add_subdirectory(tests)
add_subdirectory(tools)

option(DISABLE_WASM_SPEC_TESTS "Disable building of wasm spec unit tests" OFF)

if(NOT DISABLE_WASM_SPEC_TESTS)
  add_subdirectory(wasm-spec-tests/generated-tests)
endif()

install(FILES testnet.template DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/sysio/launcher COMPONENT dev EXCLUDE_FROM_ALL)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/testnet.template ${CMAKE_CURRENT_BINARY_DIR}/etc/sysio/launcher/testnet.template COPYONLY)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.version.in ${CMAKE_CURRENT_BINARY_DIR}/sysio.version.hpp)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sysio.version.hpp DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL)

set(EOS_ROOT_DIR "${CMAKE_BINARY_DIR}/lib")
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/sysio-config.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/sysio/sysio-config.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/SysioTesterBuild.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioTester.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/SysioCheckVersion.cmake ${CMAKE_BINARY_DIR}/lib/cmake/sysio/SysioCheckVersion.cmake COPYONLY)

set(EOS_ROOT_DIR "${CMAKE_INSTALL_FULL_LIBDIR}")
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/SysioTester.cmake.in ${CMAKE_BINARY_DIR}/modules/SysioTester.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/sysio-config.cmake.in ${CMAKE_BINARY_DIR}/modules/sysio-config.cmake @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/modules/sysio-config.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/sysio COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES ${CMAKE_BINARY_DIR}/modules/SysioTester.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/sysio COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES ${CMAKE_SOURCE_DIR}/CMakeModules/SysioCheckVersion.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/sysio COMPONENT dev EXCLUDE_FROM_ALL)


configure_file(${CMAKE_SOURCE_DIR}/LICENSE.md
               ${CMAKE_BINARY_DIR}/licenses/wire-sysio/LICENSE.md COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/softfloat/COPYING.txt ${CMAKE_BINARY_DIR}/licenses//wire-sysio/LICENSE.softfloat COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/wasm-jit/LICENSE
               ${CMAKE_BINARY_DIR}/licenses//wire-sysio/LICENSE.wavm COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/fc/secp256k1/secp256k1/COPYING
               ${CMAKE_BINARY_DIR}/licenses//wire-sysio/LICENSE.secp256k1 COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/fc/include/fc/crypto/webauthn_json/license.txt
               ${CMAKE_BINARY_DIR}/licenses//wire-sysio/LICENSE.rapidjson COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/fc/src/network/LICENSE.go
               ${CMAKE_BINARY_DIR}/licenses//wire-sysio/LICENSE.go COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/yubihsm/LICENSE ${CMAKE_BINARY_DIR}/licenses//wire-sysio/LICENSE.yubihsm COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/sys-vm/LICENSE.md
               ${CMAKE_BINARY_DIR}/licenses//wire-sysio/LICENSE.sys-vm COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/programs/clio/LICENSE.CLI11
               ${CMAKE_BINARY_DIR}/programs/clio/LICENSE.CLI11 COPYONLY)



install(FILES LICENSE.md DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ COMPONENT base)
install(FILES libraries/softfloat/COPYING.txt DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ RENAME LICENSE.softfloat COMPONENT base)
install(FILES libraries/wasm-jit/LICENSE DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ RENAME LICENSE.wavm COMPONENT base)
install(FILES libraries/fc/secp256k1/secp256k1/COPYING DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ RENAME LICENSE.secp256k1 COMPONENT base)
install(FILES libraries/fc/include/fc/crypto/webauthn_json/license.txt DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ RENAME LICENSE.rapidjson COMPONENT base)
install(FILES libraries/fc/src/network/LICENSE.go DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ COMPONENT base)
install(FILES libraries/yubihsm/LICENSE DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ RENAME LICENSE.yubihsm COMPONENT base)
install(FILES libraries/sys-vm/LICENSE.md DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ RENAME LICENSE.sys-vm COMPONENT base)
install(FILES libraries/fc/libraries/ff/LICENSE DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ RENAME LICENSE.libff COMPONENT base)
install(FILES programs/clio/LICENSE.CLI11 DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/wire-sysio/ COMPONENT base)

add_custom_target(dev-install
COMMAND "${CMAKE_COMMAND}" –build "${CMAKE_BINARY_DIR}"
COMMAND "${CMAKE_COMMAND}" –install "${CMAKE_BINARY_DIR}"
COMMAND "${CMAKE_COMMAND}" –install "${CMAKE_BINARY_DIR}" –component dev
USES_TERMINAL
)

get_property(_CTEST_CUSTOM_TESTS_IGNORE GLOBAL PROPERTY CTEST_CUSTOM_TESTS_IGNORE)
file(WRITE "${CMAKE_BINARY_DIR}/CTestCustom.cmake" "SET(CTEST_CUSTOM_TESTS_IGNORE ${_CTEST_CUSTOM_TESTS_IGNORE})")

include(doxygen)
include(package.cmake)
include(CPack)
