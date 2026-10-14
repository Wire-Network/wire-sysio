#!/usr/bin/env bash
set -euo pipefail

BUILD_JOBS="${1:?usage: build-sysio.sh <build-jobs>}"
GITHUB_WORKSPACE="${GITHUB_WORKSPACE:-$PWD}"
BUILD_DIR="${BUILD_DIR:-build}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
VCPKG_TARGET_TRIPLET="${VCPKG_TARGET_TRIPLET:-x64-linux-release}"
VCPKG_HOST_TRIPLET="${VCPKG_HOST_TRIPLET:-$VCPKG_TARGET_TRIPLET}"
VCPKG_OVERLAY_TRIPLETS="${VCPKG_OVERLAY_TRIPLETS:-$GITHUB_WORKSPACE/.github/vcpkg-triplets}"

# ANTLR and its generated consumers share libc++ container annotations.
# Track this choice in vcpkg so ASan never reuses the ordinary runtime archive.
export WIRE_ASAN_ANTLR=0
if [[ "${SYSIO_PLATFORM_NAME:-}" == asan ]]; then
  export WIRE_ASAN_ANTLR=1
fi

# Provision libc++ before vcpkg: all host C++ dependencies must be rebuilt with
# this ABI, while CDT remains an independently selected contract toolchain.
export WIRE_LLVM_ROOT="${WIRE_LLVM_ROOT:-${RUNNER_TEMP:-$GITHUB_WORKSPACE/build}/llvm-23.1.3}"
if [[ ! -x "$WIRE_LLVM_ROOT/bin/clang" ]]; then
  bash "$GITHUB_WORKSPACE/scripts/llvm/install-toolchain.sh" "$WIRE_LLVM_ROOT"
fi
if [[ "${SYSIO_PLATFORM_NAME:-}" == gcc ]]; then
  export CC=gcc-15 CXX=g++-15
else
  export CC="$WIRE_LLVM_ROOT/bin/clang" CXX="$WIRE_LLVM_ROOT/bin/clang++"
fi

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

SYSIO_BUILD_SYSTEM_CONTRACTS="${SYSIO_BUILD_SYSTEM_CONTRACTS:-OFF}"
SYSIO_BUILD_TEST_CONTRACTS="${SYSIO_BUILD_TEST_CONTRACTS:-OFF}"

# Release packages must contain system-contract artifacts rebuilt from source.
# When enabled, require an explicit CDT_ROOT so a missing toolchain fails before
# CMake can fall back to copying tracked .wasm/.abi files into the package tree.
if [[ "$SYSIO_BUILD_SYSTEM_CONTRACTS" == "ON" ]]; then
  CDT_ROOT="${CDT_ROOT:?CDT_ROOT must point to the Wire CDT root when building system contracts}"
  if [[ ! -f "$CDT_ROOT/lib/cmake/cdt/cdt-config.cmake" ]]; then
    echo "CDT_ROOT does not contain cdt-config.cmake: $CDT_ROOT" >&2
    exit 1
  fi
  CMAKE_PREFIX_PATH="$CDT_ROOT${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}"
  export CMAKE_PREFIX_PATH
fi

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
   -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$GITHUB_WORKSPACE/cmake/llvm-toolchain.cmake"
   -DWIRE_LLVM_ROOT="$WIRE_LLVM_ROOT"
   -DSYSIO_DETECT_LISTENING_TEST_PORTS=ON
   -DBUILD_SYSTEM_CONTRACTS="$SYSIO_BUILD_SYSTEM_CONTRACTS"
   -DBUILD_TEST_CONTRACTS="$SYSIO_BUILD_TEST_CONTRACTS"
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
if [[ -n "${CDT_ROOT:-}" ]]; then
   cmake_args+=(-DCDT_ROOT="$CDT_ROOT")
fi
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
   cmake_args+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH")
fi

cmake "${cmake_args[@]}"

cmake --build "$BUILD_DIR" -- -j "$BUILD_JOBS"

# Native Debug modules require a shared runtime; Release packages use static archives.
runtime_check_args=()
if [[ "$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$BUILD_DIR/CMakeCache.txt")" == Debug ]]; then
  runtime_check_args+=(--shared-runtime-root "$WIRE_LLVM_ROOT")
fi
python3 .github/scripts/check-libcxx-linkage.py "${runtime_check_args[@]}" \
  "$BUILD_DIR/programs/nodeop/nodeop" "$BUILD_DIR/programs/clio/clio" \
  "$BUILD_DIR/programs/kiod/kiod" "$BUILD_DIR/programs/sys-util/sys-util" \
  "$BUILD_DIR/tests/libcxx_runtime_test"
ctest --test-dir "$BUILD_DIR" -R '^libcxx_runtime_test$' --no-tests=error --output-on-failure

echo "=== ccache statistics ==="
ccache -s || true
