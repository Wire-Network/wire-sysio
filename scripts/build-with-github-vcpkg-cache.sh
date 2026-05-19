#!/usr/bin/env bash

set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
JOBS="${JOBS:-$(nproc)}"
RUN_TESTS=0
CLEAN="${WIRE_SYSIO_CLEAN_BUILD:-}"
NEEDS_CI_OWNERSHIP_FIX=0
BUILD_MODE="${WIRE_SYSIO_BUILD_MODE:-developer}"
VCPKG_NUGET_FEED="${VCPKG_NUGET_FEED:-${VCPKG_NUGET_FEED_URL:-https://nuget.pkg.github.com/Wire-Network/index.json}}"

usage() {
  cat <<USAGE
Usage: $0 [options]

Build Wire Sysio with the GitHub Packages vcpkg NuGet binary cache.

Options:
  --build-dir DIR       CMake build directory. Default: $BUILD_DIR
  --jobs N             Parallel build jobs. Default: $JOBS
  --clean              Remove vcpkg build artifacts before configuring.
                       Default: enabled in CI modes, disabled in developer mode.
  --run-tests          Run the parallel-safe test subset after building.
  --mode MODE          Build mode: developer, trusted-ci, or forked-pr-ci.
                       Default: $BUILD_MODE
  -h, --help           Show this help.

Environment:
  VCPKG_NUGET_FEED     NuGet feed URL. Default: $VCPKG_NUGET_FEED
  GITHUB_TOKEN         GitHub token. Required for trusted-ci mode.
  GITHUB_USER          GitHub username or owner. Required for trusted-ci mode.
  CC, CXX              C and C++ compilers. Defaults to /usr/bin/clang-18.
  CMAKE_PREFIX_PATH    Optional CMake package prefix path.
USAGE
}

fail() {
  local message="$1"
  local correction="$2"

  printf '\nERROR: %s\n\nCorrection:\n%b\n\n' "$message" "$correction" >&2
  exit 1
}

info() {
  printf '==> %s\n' "$*"
}

require_command() {
  local command_name="$1"
  local package_hint="$2"

  if ! command -v "$command_name" >/dev/null 2>&1; then
    fail "'$command_name' is not installed." "Install it with:\n  sudo apt-get install -y $package_hint"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 ]] || fail "--build-dir requires a value." "Run '$0 --build-dir /path/to/build'."
      BUILD_DIR="$2"
      shift 2
      ;;
    --jobs)
      [[ $# -ge 2 ]] || fail "--jobs requires a value." "Run '$0 --jobs $(nproc)'."
      JOBS="$2"
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    --run-tests)
      RUN_TESTS=1
      shift
      ;;
    --mode)
      [[ $# -ge 2 ]] || fail "--mode requires a value." "Use '--mode developer', '--mode trusted-ci', or '--mode forked-pr-ci'."
      BUILD_MODE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "Unknown option '$1'." "Run '$0 --help' to see supported options."
      ;;
  esac
done

if [[ "$BUILD_MODE" != "developer" && "$BUILD_MODE" != "trusted-ci" && "$BUILD_MODE" != "forked-pr-ci" ]]; then
  fail "Unsupported build mode '$BUILD_MODE'." "Use '--mode developer' for local builds, '--mode trusted-ci' for trusted GitHub Actions runs, or '--mode forked-pr-ci' for fork pull requests."
fi

if [[ -z "$CLEAN" ]]; then
  if [[ "$BUILD_MODE" == "developer" ]]; then
    CLEAN=0
  else
    CLEAN=1
  fi
fi

if [[ "$BUILD_MODE" == "trusted-ci" || "$BUILD_MODE" == "forked-pr-ci" ]]; then
  NEEDS_CI_OWNERSHIP_FIX=1
fi

if [[ ! -f "$ROOT_DIR/CMakeLists.txt" || ! -d "$ROOT_DIR/vcpkg" ]]; then
  fail "The script could not locate the Wire Sysio repository root." "Run this script from a complete Wire Sysio checkout with submodules initialized:\n  git submodule update --init --recursive"
fi

require_command cmake cmake
require_command ninja ninja-build
require_command git git

if [[ "$BUILD_MODE" != "forked-pr-ci" ]]; then
  require_command mono mono-complete
fi

if [[ "$BUILD_MODE" == "developer" ]]; then
  require_command gh gh
fi

if [[ "$NEEDS_CI_OWNERSHIP_FIX" -eq 1 ]]; then
  chown -R "$(id -u):$(id -g)" "$ROOT_DIR"
fi

if [[ "$CLEAN" == "1" ]]; then
  rm -rf "$ROOT_DIR/vcpkg/buildtrees" "$ROOT_DIR/vcpkg/packages" "$ROOT_DIR/vcpkg/vcpkg_installed" \
         "$BUILD_DIR/vcpkg_installed" ~/.cache/vcpkg ~/.vcpkg
fi

info "Bootstrapping vcpkg"
"$ROOT_DIR/vcpkg/bootstrap-vcpkg.sh"

export VCPKG_DISABLE_METRICS="${VCPKG_DISABLE_METRICS:-1}"

if [[ "$BUILD_MODE" == "forked-pr-ci" ]]; then
  # Fork PRs cannot use package credentials, so they intentionally fall back to
  # vcpkg's ephemeral runner-local binary cache and may rebuild dependencies.
  export VCPKG_BINARY_SOURCES="clear;default,readwrite"
else
  if [[ "$BUILD_MODE" == "trusted-ci" ]]; then
    GITHUB_TOKEN="${GITHUB_TOKEN:-${VCPKG_NUGET_TOKEN:-}}"
    GITHUB_USER="${GITHUB_USER:-${VCPKG_NUGET_USER:-}}"

    if [[ -z "$GITHUB_TOKEN" || -z "$GITHUB_USER" ]]; then
      fail "trusted-ci mode requires GITHUB_TOKEN and GITHUB_USER." "In the workflow step, pass:\n  GITHUB_TOKEN: \${{ secrets.GH_TOKEN_DEV }}\n  GITHUB_USER: \${{ github.repository_owner }}\nFor fork PRs, use '--mode forked-pr-ci' instead."
    fi
  else
    if ! gh auth status -h github.com >/dev/null 2>&1; then
      fail "GitHub CLI is not authenticated." "Authenticate and request package-read scope:\n  gh auth login -h github.com\n  gh auth refresh -h github.com -s read:packages"
    fi

    GH_STATUS="$(gh auth status -h github.com 2>&1 || true)"
    if [[ "$GH_STATUS" != *"read:packages"* ]]; then
      fail "GitHub CLI token is missing the 'read:packages' scope." "Refresh the token scope, then rerun this script:\n  gh auth refresh -h github.com -s read:packages\n  gh auth status -h github.com"
    fi

    GITHUB_TOKEN="${GITHUB_TOKEN:-$(gh auth token)}"
    GITHUB_USER="${GITHUB_USER:-$(gh api user --jq .login)}"

    if [[ -z "$GITHUB_TOKEN" || -z "$GITHUB_USER" ]]; then
      fail "Could not resolve GitHub token or username." "Set GITHUB_TOKEN and GITHUB_USER explicitly, or fix GitHub CLI authentication with 'gh auth login'."
    fi
  fi

  NUGET_EXE="$("$ROOT_DIR/vcpkg/vcpkg" fetch nuget | tail -n 1)"
  if [[ ! -f "$NUGET_EXE" ]]; then
    fail "vcpkg did not return a usable nuget.exe path." "Run '$ROOT_DIR/vcpkg/vcpkg fetch nuget' and fix any reported vcpkg download errors."
  fi

  info "Configuring GitHub Packages NuGet source"
  mono "$NUGET_EXE" sources remove -Name "github" >/dev/null 2>&1 || true
  # NuGet on Linux under Mono needs this flag to persist the feed credential.
  mono "$NUGET_EXE" sources add \
    -Name "github" \
    -Source "$VCPKG_NUGET_FEED" \
    -UserName "$GITHUB_USER" \
    -Password "$GITHUB_TOKEN" \
    -StorePasswordInClearText >/dev/null
  mono "$NUGET_EXE" setapikey "$GITHUB_TOKEN" -Source "$VCPKG_NUGET_FEED" >/dev/null

  if [[ "$BUILD_MODE" == "developer" ]]; then
    export VCPKG_BINARY_SOURCES="clear;nuget,$VCPKG_NUGET_FEED,read"
  else
    export VCPKG_BINARY_SOURCES="clear;nuget,$VCPKG_NUGET_FEED,readwrite"
  fi
fi

export CC="${CC:-/usr/bin/clang-18}"
export CXX="${CXX:-/usr/bin/clang++-18}"
export CMAKE_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM:-/usr/bin/ninja}"
export CCACHE_DIR="${CCACHE_DIR:-$ROOT_DIR/.ccache}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-10G}"

CMAKE_ARGS=(
  -B "$BUILD_DIR"
  -S "$ROOT_DIR"
  -G Ninja
)

if [[ -n "${SYSIO_PLATFORM_HAS_EXTRAS_CMAKE:-}" ]]; then
  CMAKE_ARGS+=(-C /extras.cmake)
fi

CMAKE_ARGS+=(
  -DCMAKE_C_COMPILER="$CC"
  -DCMAKE_CXX_COMPILER="$CXX"
  -DCMAKE_MAKE_PROGRAM="$CMAKE_MAKE_PROGRAM"
  -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/vcpkg/scripts/buildsystems/vcpkg.cmake"
  -DCMAKE_BUILD_TYPE=Release
  -DENABLE_CCACHE=ON
  -DENABLE_TESTS=ON
)

if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
  CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH")
fi

CONFIGURE_LOG="$BUILD_DIR/cmake-configure.log"
mkdir -p "$BUILD_DIR"

info "Build directory: $BUILD_DIR"
info "NuGet feed: $VCPKG_NUGET_FEED"
info "Build mode: $BUILD_MODE"
info "vcpkg binary sources: $VCPKG_BINARY_SOURCES"

info "Configuring CMake"
set +e
cmake "${CMAKE_ARGS[@]}" 2>&1 | tee "$CONFIGURE_LOG"
configure_status=${PIPESTATUS[0]}
set -e

if [[ "$configure_status" -ne 0 ]]; then
  fail "CMake configure failed." "Review $CONFIGURE_LOG. Common fixes:\n  sudo apt-get install -y mono-complete ninja-build cmake\n  gh auth refresh -h github.com -s read:packages\n  rm -rf '$BUILD_DIR' and rerun this script after changing compilers."
fi

if grep -q "Restored 0 package(s) from NuGet" "$CONFIGURE_LOG"; then
  info "Warning: vcpkg reported zero NuGet restores. This can mean the ABI does not match CI, or that the packages were already installed in '$BUILD_DIR'."
elif grep -q "Restored [1-9][0-9]* package(s) from NuGet" "$CONFIGURE_LOG"; then
  info "Confirmed vcpkg restored packages from the GitHub NuGet cache"
else
  info "No NuGet restore line was printed. This usually means vcpkg packages were already installed in '$BUILD_DIR'."
fi

info "Building"
cmake --build "$BUILD_DIR" -- -j "$JOBS"

if [[ "$RUN_TESTS" -eq 1 ]]; then
  info "Running tests"
  ctest --test-dir "$BUILD_DIR" -j "$JOBS" --output-on-failure \
    -LE "(nonparallelizable_tests|long_running_tests|wasm_spec_tests)"
fi

info "Done"
