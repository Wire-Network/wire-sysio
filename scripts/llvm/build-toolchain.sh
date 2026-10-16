#!/usr/bin/env bash
# Source-build the same pinned toolchain for hosts without an upstream binary
# distribution (including Intel macOS). CDT is not configured or rebuilt here.
set -euo pipefail
prefix="${1:?usage: build-toolchain.sh PREFIX BUILD_DIRECTORY}"
work="${2:?provide a dedicated build directory}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
version=23.1.3
checksum=c44186a7762ed28954be72e5ff6df9808e0779d4f1bf014ecc4e7e211d31ee34
mkdir -p "$work"
work="$(cd "$work" && pwd)"
archive="$work/llvm-project-${version}.src.tar.xz"
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 3 --output "$archive" \
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-${version}/llvm-project-${version}.src.tar.xz"
fi
python3 - "$archive" "$checksum" <<'PY'
"""Verify the source distribution before configuring its build."""
import hashlib
import sys
with open(sys.argv[1], "rb") as source:
    if hashlib.file_digest(source, "sha256").hexdigest() != sys.argv[2]:
        raise SystemExit("LLVM source checksum mismatch")
PY
source_dir="$work/llvm-project-${version}.src"
if [[ ! -d "$source_dir" ]]; then
  tar -xf "$archive" -C "$work"
fi
cmake -S "$source_dir/llvm" -B "$work/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DLLVM_ENABLE_PROJECTS='clang;lld' \
  -DLLVM_ENABLE_RUNTIMES='libunwind;libcxx;libcxxabi;compiler-rt' \
  -DLLVM_TARGETS_TO_BUILD='X86;AArch64' \
  -DLLVM_ENABLE_RTTI=ON \
  -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
  -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXXABI_USE_COMPILER_RT=ON \
  -DCOMPILER_RT_USE_BUILTINS_LIBRARY=ON
cmake --build "$work/build" --parallel "${JOBS:-8}"
cmake --install "$work/build"
cmake -DWIRE_LLVM_ROOT="$prefix" -P "$script_dir/prepare-runtime.cmake"
