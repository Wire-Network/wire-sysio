# GLOBAL DEPENDENCIES
# -------------------

# GLOBAL VARIABLES
set(BOOST_VERSION 1.83.0)
set(vcpkgPrefixDir "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-linux")
message(NOTICE "VCPKG PREFIX Directory: ${vcpkgPrefixDir}")
list(APPEND CMAKE_PREFIX_PATH "${vcpkgPrefixDir}/lib/cmake" "${vcpkgPrefixDir}")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# LOAD CMAKE TOOLS
find_package(PkgConfig REQUIRED)

# FIND PACKAGES WITH VCPKG
# BOOST
include(${CMAKE_SOURCE_DIR}/cmake/dependencies.boost.cmake)

# GMP
pkg_check_modules(gmp REQUIRED IMPORTED_TARGET gmp)
add_library(GMP::gmp ALIAS PkgConfig::gmp)

# Catch2
find_package(Catch2 CONFIG REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/catch2")


#find_path(GMP_INCLUDE_DIR NAMES gmp.h)
#find_library(GMP_LIBRARY gmp)
#if(NOT GMP_LIBRARY MATCHES ${CMAKE_SHARED_LIBRARY_SUFFIX})
#  message( FATAL_ERROR "GMP shared library not found" )
#endif()
#set(gmp_library_type SHARED)
#message(STATUS "GMP: ${GMP_LIBRARY}, ${GMP_INCLUDE_DIR}")
#add_library(GMP::gmp ${gmp_library_type} IMPORTED)
#set_target_properties(
#  GMP::gmp PROPERTIES
#  IMPORTED_LOCATION ${GMP_LIBRARY}
#  INTERFACE_INCLUDE_DIRECTORIES ${GMP_INCLUDE_DIR}
#)

# THREADS
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# OTHER DEPENDENCIES
find_package(ZLIB REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(libsodium CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(softfloat CONFIG REQUIRED)
find_package(secp256k1-internal CONFIG REQUIRED)
find_package(bn256 CONFIG REQUIRED)
find_package(bls12-381 CONFIG REQUIRED)
find_package(boringssl-custom CONFIG REQUIRED)