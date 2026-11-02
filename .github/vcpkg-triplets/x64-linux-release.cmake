set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
# Track the runtime prefix and apply the same ABI to every dependency.
set(VCPKG_ENV_PASSTHROUGH WIRE_LLVM_ROOT CC CXX)
# Generated ANTLR code and the runtime mutate the same libc++ containers.
# Keep their ASan annotations consistent and record the choice in the port ABI.
if(PORT STREQUAL "antlr4")
    list(APPEND VCPKG_ENV_PASSTHROUGH WIRE_ASAN_ANTLR)
    if("$ENV{WIRE_ASAN_ANTLR}" STREQUAL "1")
        set(VCPKG_C_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
        set(VCPKG_CXX_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
    endif()
endif()
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../../cmake/llvm-toolchain.cmake")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    -DCMAKE_C_COMPILER_LAUNCHER=ccache
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
)
