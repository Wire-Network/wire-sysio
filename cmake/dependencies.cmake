# GLOBAL DEPENDENCIES
# -------------------

# GLOBAL VARIABLES

set(vcpkgPrefixDir "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-linux")
message(NOTICE "VCPKG PREFIX Directory: ${vcpkgPrefixDir}")
list(APPEND CMAKE_PREFIX_PATH "${vcpkgPrefixDir}/lib/cmake" "${vcpkgPrefixDir}")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# LOAD CMAKE TOOLS
find_package(PkgConfig REQUIRED)

# ZLIB & zstd
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)

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
            message(STATUS "LLVM target (${llvm_target}) to use release build")
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

# GMP
pkg_check_modules(gmp REQUIRED IMPORTED_TARGET gmp)
add_library(GMP::gmp ALIAS PkgConfig::gmp)

# Catch2
find_package(Catch2 CONFIG REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/catch2")

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
