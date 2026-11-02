#!/usr/bin/env bash
# Install the verified upstream LLVM distribution, including libc++/libc++abi,
# libunwind and compiler-rt, before configuring any host C++ dependencies.
set -euo pipefail

prefix="${1:?usage: install-toolchain.sh PREFIX}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
version=23.1.3
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)
    platform=Linux-X64
    checksum=ca9fe385a5d9e2d88132dc0e23a00e9f87f38fa75f67207b5e457d208e71d013 ;;
  Linux-aarch64)
    platform=Linux-ARM64
    checksum=56d005e339c979a696ed6c244e6472e16f3b1371e48987a07e6145bda7ee0d03 ;;
  Darwin-arm64)
    platform=macOS-ARM64
    checksum=3dcfd88aec10be204a24037bcb0dc0ad777882e9dbe8ac60e2639060d8a794b1 ;;
  *) echo "No pinned LLVM distribution for this host; supply a source-built LLVM ${version} via WIRE_LLVM_ROOT." >&2; exit 1 ;;
esac

# A completed installation is reusable; never overwrite a partial/foreign prefix.
if [[ -f "$prefix/.wire-llvm-distribution" ]]; then
  [[ "$(cat "$prefix/.wire-llvm-distribution")" == "$version $platform $checksum" ]]
  "$prefix/bin/clang" --version
  exit 0
fi
if [[ -e "$prefix" ]]; then
  echo "Installation prefix already exists without the expected completion marker: $prefix" >&2
  exit 1
fi
mkdir -p "$(dirname "$prefix")"
stage="$(mktemp -d "${prefix}.stage.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
archive="LLVM-${version}-${platform}.tar.xz"
curl --fail --location --retry 3 --output "$stage/$archive" \
  "https://github.com/llvm/llvm-project/releases/download/llvmorg-${version}/${archive}"
python3 - "$stage/$archive" "$checksum" <<'PY'
"""Verify the pinned release archive before extracting executable content."""
import hashlib
import pathlib
import sys
with pathlib.Path(sys.argv[1]).open("rb") as archive:
    digest = hashlib.file_digest(archive, "sha256").hexdigest()
if digest != sys.argv[2]:
    raise SystemExit("LLVM distribution checksum mismatch")
PY
mkdir "$stage/install"
tar -xf "$stage/$archive" -C "$stage/install" --strip-components=1
"$stage/install/bin/clang" --version
cmake -DWIRE_LLVM_ROOT="$stage/install" -P "$script_dir/prepare-runtime.cmake"
printf '%s\n' "$version $platform $checksum" > "$stage/install/.wire-llvm-distribution"
mv "$stage/install" "$prefix"
