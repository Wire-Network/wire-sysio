# ##################################################################################################
# Defines the sysio-wasm-spec-tests generator and generated tests
# ##################################################################################################
cmake_minimum_required(VERSION 3.8)
set(VERSION_MAJOR 0)
set(VERSION_MINOR 0)
set(VERSION_PATCH 1)
project(sys-wasm-spec-tests VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})

if (VERSION_SUFFIX)
   set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-${VERSION_SUFFIX}")
else()
   set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
endif()

message(STATUS "Building sys-wasm-spec-tests v${VERSION_FULL}...")

option(WASM_SPEC_TESTS "Build WASM Spec Tests" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_executable( sysio_test_generator ${CMAKE_CURRENT_SOURCE_DIR}/sysio_test_generator.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sysio_wasm_spec_test_generator.cpp)
