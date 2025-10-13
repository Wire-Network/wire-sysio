#!/usr/bin/env bash
#
# Build Clang/LLVM 18 (tag llvmorg/18.1.8) on Ubuntu, including:
#   - clang, clang-tools-extra, lld
#   - libc++, libc++abi, compiler-rt
# Targets: X86 (add more if needed).
#
# Usage:
#   BASE_DIR=/opt/clang \
#   CLANG_18_PREFIX=/opt/clang/clang-18 \
#   JOBS=16 \
#   bash clang-18-ubuntu-build-source.sh
#
# Notes:
#   * Mirrors style of llvm-11-ubuntu-build-source.sh (variables, Ninja, comments).
set -euo pipefail

BASE_DIR=${BASE_DIR:-$PWD}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_SRC_DIR="${BASE_DIR}/llvm-project-18"
BUILD_DIR="${BASE_DIR}/clang-18-build"
PREFIX="${CLANG_18_PREFIX:-${BASE_DIR}/clang-18}"

# ---------- Config (overridable) ----------
LLVM_18_TAG="${LLVM_18_TAG:-llvmorg-18.1.8}"

if which nproc >/dev/null 2>&1; then
  echo "nproc is installed"
  NPROC=$(nproc)
else
  NPROC=0
fi

if [[ "${NPROC}" -gt 0 ]]; then
  if [[ "${NPROC}" -gt 32 ]]; then
    JOBS="32"
  else
    JOBS="${NPROC}"
  fi
else
  JOBS="4"
fi

echo "[+] Using:"
echo "  PREFIX       = ${PREFIX}"
echo "  JOBS         = ${JOBS}"
echo "  LLVM_18_TAG  = ${LLVM_18_TAG}"
echo "  BASE_DIR     = ${BASE_DIR}"

# Clean up env that might leak odd flags
unset CFLAGS CXXFLAGS LDFLAGS CPPFLAGS

echo "[+] Fetching llvm-project (${LLVM_18_TAG})…"
rm -rf "${LLVM_SRC_DIR}" "${BUILD_DIR}"
git clone --depth=1 -b "${LLVM_18_TAG}" https://github.com/llvm/llvm-project.git "${LLVM_SRC_DIR}"

# Base CMake flags
declare -a CMAKE_FLAGS=(
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"

  -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra;lld"
  -DLLVM_ENABLE_RUNTIMES="libunwind;libcxx;libcxxabi;compiler-rt"

  # Make the linkage explicit and consistent
  -DLIBCXXABI_USE_LLVM_UNWINDER=ON
  -DLIBCXX_USE_COMPILER_RT=ON
  -DLIBCXXABI_USE_COMPILER_RT=ON
  -DCOMPILER_RT_USE_BUILTINS_LIBRARY=ON

  -DLLVM_TARGETS_TO_BUILD="X86"
  # -DLLVM_INCLUDE_TESTS=OFF
  -DLLVM_INCLUDE_EXAMPLES=OFF
)

echo "[+] Configuring…"
cmake -S "${LLVM_SRC_DIR}/llvm" -B "${BUILD_DIR}" "${CMAKE_FLAGS[@]}"

echo "[+] Building…"
ninja -C "${BUILD_DIR}" -j "${JOBS}"

echo "[+] Installing…"
if command -v sudo >/dev/null 2>&1; then
  sudo mkdir -p "${PREFIX}"
  sudo ninja -C "${BUILD_DIR}" install
else
  mkdir -p "${PREFIX}"
  ninja -C "${BUILD_DIR}" install
fi

echo "[+] Done. Toolchain at: ${PREFIX}"
ls -l "${PREFIX}/bin" | sed -n '1,200p'
