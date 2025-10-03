#!/usr/bin/env bash
#
# Build LLVM 11 (branch release/11.x) on Ubuntu 24.04, including:
#   - clang, clang-tools-extra, lld
#   - libc++, libc++abi, libunwind
#   - compiler-rt (builtins/crt; sanitizers optional)
# Targets: X86, WebAssembly (plus AArch64 optional).
#
# Suitable for Antelope / EOSIO / Spring / Leap toolchains.
#
# Usage:
#   bash build-llvm11-release-11x.sh [--prefix /opt/llvm-11] [--jobs N] [--with-sanitizers]
#                                     [--targets X86;WebAssembly;AArch64]
# Notes:
#   * First run on a clean build dir (script wipes its own).
#   * Re-run safe; it reclones shallow at the desired branch.
#   * Produces -11 convenience symlinks in $PREFIX/bin.
set -euo pipefail

BASE_DIR=${BASE_DIR:-$PWD}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_SRC_DIR="${BASE_DIR}/llvm-project"
BUILD_DIR="${BASE_DIR}/llvm-11-build"
PREFIX="${LLVM_11_PREFIX:-${BASE_DIR}/llvm-11}"

### ---------- Config (overridable by flags) ----------

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

echo "Configured to use ${JOBS} jobs for build"

BRANCH="release/11.x"

### ---------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
  --prefix)
    PREFIX="${2:?}"
    shift 2
    ;;
  --jobs)
    JOBS="${2:?}"
    shift 2
    ;;
  *)
    echo "Unknown arg: $1"
    exit 2
    ;;
  esac
done

echo "[+] Using:"
echo "    PREFIX        = ${PREFIX}"
echo "    JOBS          = ${JOBS}"
echo "    BRANCH        = ${BRANCH}"

if [[ $(uname) != "Linux" ]]; then
  echo "[+] You must install dependencies manually on non-Linux"
else
  echo "[+] Checking & installing deps (sudo)…"
  . "${SCRIPT_DIR}/llvm-11-ubuntu-deps.sh"
fi
# Clean env that might force freestanding or odd sysroots
unset CFLAGS CXXFLAGS LDFLAGS CPPFLAGS

echo "[+] Fetching llvm-project (${BRANCH})…"
if [[ ! -d "${LLVM_SRC_DIR}" ]]; then
  git clone --depth=1 -b "${BRANCH}" https://github.com/llvm/llvm-project.git "${LLVM_SRC_DIR}"
fi

rm -rf "${BUILD_DIR}"

# Base CMake flags
declare -a CMAKE_FLAGS=(
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"

  -DLLVM_TARGETS_TO_BUILD=host
  -DLLVM_BUILD_TOOLS=Off
  -DLLVM_ENABLE_RTTI=On
  -DLLVM_ENABLE_TERMINFO=Off
  -DLLVM_ENABLE_PIC=Off
  -DCOMPILER_RT_BUILD_SANITIZERS=OFF
  -DCMAKE_C_COMPILER=gcc-10
  -DCMAKE_CXX_COMPILER=g++-10
)

# Build & Install (top-level)
echo "[+] Configuring (top-level)…"
cmake -S "${LLVM_SRC_DIR}/llvm" -B "${BUILD_DIR}" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${CMAKE_FLAGS[@]}"

echo "[+] Building…"
ninja -C "${BUILD_DIR}" -j "${JOBS}"

echo "[+] Installing (sudo)…"
sudo mkdir -p "${PREFIX}"
sudo ninja -C "${BUILD_DIR}" install
