cmake_minimum_required(VERSION 3.16)

project(wasm-unittests-harness CXX)

# The harness is a small utility that shells out to a WASI runtime (wasmtime/wasmer)
# to execute provided .wasm unit test modules that use Boost.Test. It does not
# require linking against a WASI runtime library.

add_executable(wasm-unittests-harness
  main.cpp
)

target_link_libraries(wasm-unittests-harness PUBLIC Boost::program_options)

# Keep dependencies minimal; use C++17 for std::filesystem and std::string_view
set_target_properties(wasm-unittests-harness PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS YES
)

# On UNIX, link stdc++fs for older libstdc++ if necessary (Clang 18 usually fine).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
#check_cxx_source_compiles("#include <filesystem>\nint main(){std::filesystem::path p; (void)p; return 0;}" HAS_STD_FILESYSTEM)
#if(NOT HAS_STD_FILESYSTEM AND UNIX)
#  target_link_libraries(wasm-unittests-harness PRIVATE stdc++fs)
#endif()

install(TARGETS wasm-unittests-harness RUNTIME DESTINATION bin)
