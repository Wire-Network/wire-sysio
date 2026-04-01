# GLOBAL DEPENDENCIES
# -------------------

# GLOBAL VARIABLES
set(VCPKG_INSTALLED_TARGET_DIR "${CMAKE_BINARY_DIR}/${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
message(NOTICE "VCPKG PREFIX Directory: ${VCPKG_INSTALLED_TARGET_DIR}")

# CMAKE PATHS
list(
        PREPEND CMAKE_PREFIX_PATH
        "${VCPKG_INSTALLED_TARGET_DIR}/lib/cmake"
        "${VCPKG_INSTALLED_TARGET_DIR}"
)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# LOAD CMAKE TOOLS
find_package(PkgConfig REQUIRED)

# PkgConfig dependencies
# ZLIB, BZip2 (static via vcpkg, required by Boost::iostreams)
# Restrict search to vcpkg to avoid picking up system libraries
set(ENV{PKG_CONFIG_PATH} "${VCPKG_INSTALLED_TARGET_DIR}/lib/pkgconfig")
pkg_check_modules(zlib REQUIRED IMPORTED_TARGET zlib)
pkg_check_modules(bzip2 REQUIRED IMPORTED_TARGET bzip2)
pkg_check_modules(gmp REQUIRED IMPORTED_TARGET gmp)
add_library(ZLIB::ZLIB ALIAS PkgConfig::zlib)
add_library(BZip2::BZip2 ALIAS PkgConfig::bzip2)
add_library(GMP::gmp ALIAS PkgConfig::gmp)

# ZSTD, LibLZMA (static via vcpkg, required by Boost::iostreams)
find_package(zstd CONFIG REQUIRED PATHS "${VCPKG_INSTALLED_TARGET_DIR}" NO_DEFAULT_PATH)
find_package(LibLZMA REQUIRED PATHS "${VCPKG_INSTALLED_TARGET_DIR}" NO_DEFAULT_PATH)

# FIND PACKAGES WITH VCPKG
# LLVM
find_package(LLVM CONFIG REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake at: ${LLVM_DIR}")
if(LLVM_VERSION_MAJOR VERSION_LESS 18)
    message(FATAL_ERROR "WIRE requires LLVM version 18 or later")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT DISABLE_LLVM_LINKAGE_OVERRIDE)
    message(NOTICE "DEBUG build, and DISABLE_LLVM_LINKAGE_OVERRIDE=OFF, overriding LLVM linkage to Release")
    foreach(llvm_target ${LLVM_AVAILABLE_LIBS})
        if(TARGET ${llvm_target})
            set_target_properties(
                    ${llvm_target}
                    PROPERTIES
                    MAP_IMPORTED_CONFIG_DEBUG Release
            )
        endif()
    endforeach()
endif()

# BOOST
include(dependencies.boost NO_POLICY_SCOPE)

# Catch2
find_package(Catch2 CONFIG REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/catch2")

# protobuf
include(dependencies.protobuf NO_POLICY_SCOPE)

# THREADS
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# OTHER DEPENDENCIES
find_package(magic_enum CONFIG REQUIRED)
find_package(boringssl-custom CONFIG REQUIRED)
find_package(ethash CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(gsl-lite CONFIG REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(libsodium CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(softfloat CONFIG REQUIRED)
find_package(secp256k1-internal CONFIG REQUIRED)
find_package(bn256 CONFIG REQUIRED)
find_package(bls12-381 CONFIG REQUIRED)

find_package(CURL 8.16.0 CONFIG REQUIRED)
find_package(sys-vm CONFIG REQUIRED)
