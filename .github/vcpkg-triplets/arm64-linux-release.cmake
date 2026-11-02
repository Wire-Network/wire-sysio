# Native Linux ARM64 uses the same pinned C++ ABI as x86_64.
include("${CMAKE_CURRENT_LIST_DIR}/x64-linux-release.cmake")
set(VCPKG_TARGET_ARCHITECTURE arm64)
