#!/usr/bin/env bash
set -euo pipefail

BUILD_JOBS="${1:?usage: build-sysio.sh <build-jobs>}"

echo "Building for ${SYSIO_PLATFORM_NAME:-unknown platform}"

# Use $GITHUB_WORKSPACE (container path /__w/...) so ccache/vcpkg write to
# the mounted volume and persist for actions/cache to save.
export CCACHE_DIR="$GITHUB_WORKSPACE/.ccache"
export VCPKG_BINARY_SOURCES="clear;files,$GITHUB_WORKSPACE/vcpkg-binary-cache,readwrite"
export VCPKG_TARGET_TRIPLET=x64-linux-release
export VCPKG_HOST_TRIPLET=x64-linux-release
export VCPKG_OVERLAY_TRIPLETS="$GITHUB_WORKSPACE/.github/vcpkg-triplets"

# Clean intermediate vcpkg artifacts but preserve binary cache and downloads.
rm -rf vcpkg/buildtrees vcpkg/packages vcpkg/vcpkg_installed \
        build/vcpkg_installed ~/.cache/vcpkg ~/.vcpkg

mkdir -p "$GITHUB_WORKSPACE/vcpkg-binary-cache"

./vcpkg/bootstrap-vcpkg.sh

chown -R "$(id -u):$(id -g)" "$PWD"

# Reset ccache stats for this build.
ccache -z || true

cmake -B build -S . -G Ninja ${SYSIO_PLATFORM_HAS_EXTRAS_CMAKE:+-C /extras.cmake} \
-DCMAKE_C_COMPILER="$CC" \
-DCMAKE_CXX_COMPILER="$CXX" \
-DCMAKE_MAKE_PROGRAM="$CMAKE_MAKE_PROGRAM" \
-DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
-DCMAKE_BUILD_TYPE=Release \
-DVCPKG_TARGET_TRIPLET="$VCPKG_TARGET_TRIPLET" \
-DVCPKG_HOST_TRIPLET="$VCPKG_HOST_TRIPLET" \
-DVCPKG_OVERLAY_TRIPLETS="$VCPKG_OVERLAY_TRIPLETS" \
-DENABLE_CCACHE=ON \
-DENABLE_TESTS=ON \
${CMAKE_PREFIX_PATH:+-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH}

cmake --build build -- -j "$BUILD_JOBS"

echo "=== ccache statistics ==="
ccache -s || true
