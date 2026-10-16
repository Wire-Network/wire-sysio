# Intel macOS can supply a source-built LLVM 23.1.3 distribution.
include("${CMAKE_CURRENT_LIST_DIR}/arm64-osx-release.cmake")
set(VCPKG_TARGET_ARCHITECTURE x64)
