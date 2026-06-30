#!/usr/bin/env bash
set -euo pipefail

# Build the pinned Wire CDT source tree that release-tag sysio CI uses for
# contract provenance. Sysio needs CDT's build-tree package because it exports the
# cdt::protoc and cdt::protoc-gen-zpp targets used by OPP contract protobufs.

CDT_TARGET="${1:-${CDT_TARGET:-}}"
CDT_REPO_URL="${CDT_REPO_URL:-https://github.com/Wire-Network/wire-cdt.git}"
GITHUB_WORKSPACE="${GITHUB_WORKSPACE:-$PWD}"
CDT_SOURCE_DIR="${CDT_SOURCE_DIR:-$GITHUB_WORKSPACE/../wire-cdt}"
CDT_BUILD_DIR="${CDT_BUILD_DIR:-$CDT_SOURCE_DIR/build/release}"
NUM_JOBS="${NUM_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)}"

if [[ -z "$CDT_TARGET" ]]; then
  echo "usage: setup-wire-cdt.sh <wire-cdt-ref>" >&2
  exit 2
fi
if [[ -z "$NUM_JOBS" || "$NUM_JOBS" -le 0 ]]; then
  echo "NUM_JOBS must be a positive integer" >&2
  exit 1
fi

git_auth=()
if [[ -n "${GH_TOKEN:-${GITHUB_TOKEN:-}}" ]]; then
  git_auth=(-c "http.https://github.com/.extraheader=AUTHORIZATION: bearer ${GH_TOKEN:-$GITHUB_TOKEN}")
fi

release_ref="$CDT_TARGET"
if [[ "$release_ref" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+._A-Za-z0-9]*)?$ ]]; then
  release_ref="v$release_ref"
fi

if [[ ! -d "$CDT_SOURCE_DIR/.git" ]]; then
  mkdir -p "$(dirname "$CDT_SOURCE_DIR")"
  git "${git_auth[@]}" clone "$CDT_REPO_URL" "$CDT_SOURCE_DIR"
fi

git -C "$CDT_SOURCE_DIR" "${git_auth[@]}" fetch origin --tags --prune
if ! git -C "$CDT_SOURCE_DIR" rev-parse --verify --quiet "$release_ref^{commit}" >/dev/null; then
  git -C "$CDT_SOURCE_DIR" "${git_auth[@]}" fetch origin "$release_ref"
fi
git -C "$CDT_SOURCE_DIR" checkout --detach "$release_ref"
git -C "$CDT_SOURCE_DIR" submodule update --init --recursive

(
  cd "$CDT_SOURCE_DIR"
  ./vcpkg/bootstrap-vcpkg.sh
)

export CC="${CC:-/usr/bin/clang-18}"
export CXX="${CXX:-/usr/bin/clang++-18}"
export CMAKE_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM:-/usr/bin/ninja}"
export VCPKG_TARGET_TRIPLET="${CDT_VCPKG_TARGET_TRIPLET:-${VCPKG_TARGET_TRIPLET:-x64-linux-release}}"
export VCPKG_HOST_TRIPLET="${CDT_VCPKG_HOST_TRIPLET:-${VCPKG_HOST_TRIPLET:-x64-linux-release}}"
export VCPKG_OVERLAY_TRIPLETS="${CDT_VCPKG_OVERLAY_TRIPLETS:-$CDT_SOURCE_DIR/.github/vcpkg-triplets}"

cmake -B "$CDT_BUILD_DIR" -S "$CDT_SOURCE_DIR" -G Ninja \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_MAKE_PROGRAM="$CMAKE_MAKE_PROGRAM" \
  -DCMAKE_TOOLCHAIN_FILE="$CDT_SOURCE_DIR/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_TARGET_TRIPLET="$VCPKG_TARGET_TRIPLET" \
  -DVCPKG_HOST_TRIPLET="$VCPKG_HOST_TRIPLET" \
  -DVCPKG_OVERLAY_TRIPLETS="$VCPKG_OVERLAY_TRIPLETS"

cmake --build "$CDT_BUILD_DIR" -- -j"$NUM_JOBS"

for required in \
  "$CDT_BUILD_DIR/lib/cmake/cdt/cdt-config.cmake" \
  "$CDT_BUILD_DIR/bin/cdt-cc" \
  "$CDT_BUILD_DIR/bin/cdt-protoc" \
  "$CDT_BUILD_DIR/bin/cdt-protoc-gen-zpp"; do
  if [[ ! -e "$required" ]]; then
    echo "Wire CDT build did not produce required file: $required" >&2
    exit 1
  fi
done

if [[ -n "${GITHUB_ENV:-}" ]]; then
  echo "CDT_ROOT=$CDT_BUILD_DIR" >>"$GITHUB_ENV"
fi

echo "Prepared Wire CDT at $(git -C "$CDT_SOURCE_DIR" rev-parse HEAD) in $CDT_BUILD_DIR"
