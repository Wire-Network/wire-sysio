#!/usr/bin/env bash
set -euo pipefail

# Install a Wire CDT Debian package downloaded from wire-cdt CI and expose its
# root to later sysio build steps. Release-tag builds need the package variant
# that includes CDT protobuf tools for OPP contract model generation.

# The wire-cdt deb uses the DISTRO-TOOLCHAIN layout (the shape distros use for a
# bundled compiler, cf. /usr/lib/llvm-18):
#
#   /usr/lib/cdt/                  the entire self-contained toolchain -- bin/
#                                  (incl. the bundled clang/lld/llvm-* set),
#                                  lib/, include/, lib/cmake/cdt/, scripts/,
#                                  share/, cdt.imports
#   /usr/bin/<tool>                symlinks -> ../lib/cdt/bin/<tool>, PUBLIC
#                                  ENTRY POINTS ONLY (no bare clang/llvm names)
#   /usr/lib/cmake/cdt/            discoverable cdt-config.cmake => find_package
#   /usr/share/licenses/wire-cdt/  license texts
#
# There is NO /usr/cdt subtree and no /opt payload (that is the portable
# tarball's root, not the deb's).
#
# CDT_ROOT is the SELF-CONTAINED HOME, not /usr: build-sysio.sh checks
# "$CDT_ROOT/lib/cmake/cdt/cdt-config.cmake" and cmake/contract-tools.cmake
# builds "$CDT_ROOT/lib/cmake/cdt/CDTWasmToolchain.cmake", both of which only
# resolve under /usr/lib/cdt. (find_package(cdt) alone would need nothing at all,
# thanks to the /usr/lib/cmake/cdt copy -- but this repo passes CDT_ROOT
# explicitly, so it must point at the home.)
CDT_EXPECTED_ROOT="/usr/lib/cdt"

# Public entry points that must be on PATH via /usr/bin after install.
CDT_ENTRY_POINTS=(cdt-cc cdt-cpp cdt-protoc cdt-protoc-gen-zpp)

CDT_DEB="${1:?usage: install-wire-cdt-package.sh <cdt-deb>}"

if [[ "$CDT_DEB" != /* ]]; then
  CDT_DEB="$PWD/$CDT_DEB"
fi
if [[ ! -f "$CDT_DEB" ]]; then
  echo "Wire CDT Debian package was not found: $CDT_DEB" >&2
  exit 1
fi

echo "Wire CDT Debian package SHA-256: $(sha256sum "$CDT_DEB" | awk '{print $1}')"

cdt_payload="$(dpkg-deb --contents "$CDT_DEB" | awk '{print $6}')"

# Derive the toolchain HOME from cdt.imports, which exists exactly once and sits
# at the root of the self-contained tree. Deriving from cdt-config.cmake would be
# ambiguous now that the deb ships TWO copies of it (the home's own, plus the
# discoverable one at /usr/lib/cmake/cdt).
cdt_root="$(
  printf '%s\n' "$cdt_payload" |
    awk '$0 ~ /\/cdt\.imports$/ { path=$0; sub(/^\.\//, "/", path); sub(/\/cdt\.imports$/, "", path); print path; exit }'
)"
if [[ -z "$cdt_root" ]]; then
  echo "Wire CDT Debian package does not contain cdt.imports" >&2
  exit 1
fi

# The home is DERIVED from the package above, then asserted against the layout
# this repo expects, so a layout change in wire-cdt fails here with a clear
# message instead of surfacing later as a missing toolchain during the build.
if [[ "$cdt_root" != "$CDT_EXPECTED_ROOT" ]]; then
  echo "Wire CDT toolchain home is '$cdt_root', expected '$CDT_EXPECTED_ROOT'" >&2
  echo "The deb must use the distro-toolchain layout (self-contained tree at /usr/lib/cdt)." >&2
  exit 1
fi

# Payload-shape assertions, before anything is installed.
for required in \
  "./usr/lib/cdt/bin/cdt-cc" \
  "./usr/lib/cdt/lib/cmake/cdt/cdt-config.cmake" \
  "./usr/lib/cdt/cdt.imports" \
  "./usr/lib/cmake/cdt/cdt-config.cmake"; do
  if ! printf '%s\n' "$cdt_payload" | grep -qx "$required"; then
    echo "Wire CDT package payload is missing $required" >&2
    exit 1
  fi
done

if printf '%s\n' "$cdt_payload" | grep -q '^\./usr/cdt/'; then
  echo "Wire CDT package installs into a /usr/cdt subtree; expected the /usr/lib/cdt home" >&2
  exit 1
fi

# The bundled clang / lld / llvm-* binaries must stay private to the home. In
# /usr/bin they would collide with the distro's own toolchain packages and either
# fail this apt-get install or hijack the compiler the sysio build itself uses.
for banned in clang clang++ opt llc lld ld.lld wasm-ld; do
  if printf '%s\n' "$cdt_payload" | grep -qx "./usr/bin/$banned"; then
    echo "Wire CDT package ships ./usr/bin/$banned, which collides with the distro toolchain" >&2
    exit 1
  fi
done
if printf '%s\n' "$cdt_payload" | grep -qE '^\./usr/bin/llvm-'; then
  echo "Wire CDT package ships bare llvm-* binaries in /usr/bin" >&2
  exit 1
fi

apt-get update
apt-get install -y "$CDT_DEB"

# Public entry points must be reachable on PATH through the /usr/bin symlinks.
for entry in "${CDT_ENTRY_POINTS[@]}"; do
  if [[ ! -x "/usr/bin/$entry" ]]; then
    echo "Wire CDT package did not expose /usr/bin/$entry (missing or dangling symlink)" >&2
    exit 1
  fi
done

# build-sysio.sh checks "$CDT_ROOT/lib/cmake/cdt/cdt-config.cmake" and
# cmake/contract-tools.cmake builds "$CDT_ROOT/lib/cmake/cdt/CDTWasmToolchain.cmake";
# both must resolve under the home for the contracts build to work at all.
for required in \
  "$cdt_root/lib/cmake/cdt/cdt-config.cmake" \
  "$cdt_root/lib/cmake/cdt/CDTWasmToolchain.cmake" \
  "$cdt_root/bin/cdt-cc" \
  "$cdt_root/bin/cdt-protoc" \
  "$cdt_root/bin/cdt-protoc-gen-zpp" \
  "$cdt_root/cdt.imports"; do
  if [[ ! -e "$required" ]]; then
    echo "Wire CDT package did not install required file: $required" >&2
    exit 1
  fi
done

if ! grep -Rq 'cdt::protoc-gen-zpp' "$cdt_root/lib/cmake/cdt"; then
  echo "Wire CDT package does not expose cdt::protoc-gen-zpp to CMake" >&2
  exit 1
fi

if [[ -n "${GITHUB_ENV:-}" ]]; then
  echo "CDT_ROOT=$cdt_root" >>"$GITHUB_ENV"
fi

echo "Prepared Wire CDT package from $CDT_DEB"
echo "Resolved CDT root: $cdt_root (self-contained toolchain home)"
echo "  entry points on PATH: /usr/bin/{$(IFS=,; echo "${CDT_ENTRY_POINTS[*]}")} -> ../lib/cdt/bin/"
echo "  discoverable cmake config: /usr/lib/cmake/cdt/cdt-config.cmake (find_package(cdt) needs no CMAKE_PREFIX_PATH)"
