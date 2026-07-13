#!/usr/bin/env bash
set -euo pipefail

# Install a Wire CDT Debian package downloaded from wire-cdt CI and expose its
# root to later sysio build steps. Release-tag builds need the package variant
# that includes CDT protobuf tools for OPP contract model generation.

CDT_DEB="${1:?usage: install-wire-cdt-package.sh <cdt-deb>}"

if [[ "$CDT_DEB" != /* ]]; then
  CDT_DEB="$PWD/$CDT_DEB"
fi
if [[ ! -f "$CDT_DEB" ]]; then
  echo "Wire CDT Debian package was not found: $CDT_DEB" >&2
  exit 1
fi

echo "Wire CDT Debian package SHA-256: $(sha256sum "$CDT_DEB" | awk '{print $1}')"

cdt_config_path="$(
  dpkg-deb --contents "$CDT_DEB" |
    awk '$NF ~ /\/lib\/cmake\/cdt\/cdt-config\.cmake$/ { path=$NF; sub(/^\.\//, "/", path); print path; exit }'
)"
if [[ -z "$cdt_config_path" ]]; then
  echo "Wire CDT Debian package does not contain lib/cmake/cdt/cdt-config.cmake" >&2
  exit 1
fi

cdt_root="${cdt_config_path%/lib/cmake/cdt/cdt-config.cmake}"

apt-get update
apt-get install -y "$CDT_DEB"

for required in \
  "$cdt_root/lib/cmake/cdt/cdt-config.cmake" \
  "$cdt_root/bin/cdt-cc" \
  "$cdt_root/bin/cdt-protoc" \
  "$cdt_root/bin/cdt-protoc-gen-zpp"; do
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

echo "Prepared Wire CDT package from $CDT_DEB at $cdt_root"
