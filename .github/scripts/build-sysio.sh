#!/usr/bin/env bash
set -euo pipefail

BUILD_JOBS="${1:?usage: build-sysio.sh <build-jobs>}"
GITHUB_WORKSPACE="${GITHUB_WORKSPACE:-$PWD}"
BUILD_DIR="${BUILD_DIR:-build}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
VCPKG_TARGET_TRIPLET="${VCPKG_TARGET_TRIPLET:-x64-linux-release}"
VCPKG_HOST_TRIPLET="${VCPKG_HOST_TRIPLET:-$VCPKG_TARGET_TRIPLET}"
VCPKG_OVERLAY_TRIPLETS="${VCPKG_OVERLAY_TRIPLETS:-$GITHUB_WORKSPACE/.github/vcpkg-triplets}"

echo "Building for ${SYSIO_PLATFORM_NAME:-unknown platform}"
echo "Build directory: ${BUILD_DIR}"
echo "vcpkg target triplet: ${VCPKG_TARGET_TRIPLET}"

# Use $GITHUB_WORKSPACE (container path /__w/...) so ccache/vcpkg write to
# the mounted volume and persist for actions/cache to save.
export CCACHE_DIR="${CCACHE_DIR:-$GITHUB_WORKSPACE/.ccache}"
export VCPKG_BINARY_SOURCES="${VCPKG_BINARY_SOURCES:-clear;files,$GITHUB_WORKSPACE/vcpkg-binary-cache,readwrite}"
export VCPKG_TARGET_TRIPLET
export VCPKG_HOST_TRIPLET
export VCPKG_OVERLAY_TRIPLETS

# Clean intermediate vcpkg artifacts but preserve binary cache and downloads.
rm -rf vcpkg/buildtrees vcpkg/packages vcpkg/vcpkg_installed \
        "$BUILD_DIR/vcpkg_installed" ~/.cache/vcpkg ~/.vcpkg

mkdir -p "$GITHUB_WORKSPACE/vcpkg-binary-cache"

./vcpkg/bootstrap-vcpkg.sh

# Reset ccache stats for this build.
ccache -z || true

cmake_args=(
   -B "$BUILD_DIR"
   -S .
   -G Ninja
   -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake"
   -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
   -DVCPKG_TARGET_TRIPLET="$VCPKG_TARGET_TRIPLET"
   -DVCPKG_HOST_TRIPLET="$VCPKG_HOST_TRIPLET"
   -DVCPKG_OVERLAY_TRIPLETS="$VCPKG_OVERLAY_TRIPLETS"
   -DENABLE_CCACHE=ON
   -DENABLE_TESTS=ON
)

if [[ -n "${SYSIO_PLATFORM_HAS_EXTRAS_CMAKE:-}" ]]; then
   cmake_args+=(-C /extras.cmake)
fi
if [[ -n "${CC:-}" ]]; then
   cmake_args+=(-DCMAKE_C_COMPILER="$CC")
fi
if [[ -n "${CXX:-}" ]]; then
   cmake_args+=(-DCMAKE_CXX_COMPILER="$CXX")
fi
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
   cmake_args+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH")
fi

cmake "${cmake_args[@]}"

cmake --build "$BUILD_DIR" -- -j "$BUILD_JOBS"

echo "=== ccache statistics ==="
ccache -s || true
