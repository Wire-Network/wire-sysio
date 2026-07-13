#!/usr/bin/env bash
set -euo pipefail

# Keep release package jobs from publishing build trees configured to copy
# tracked system-contract artifacts. The build itself proves the ExternalProject
# can rebuild contracts; this guard catches future workflow edits before cpack runs.

BUILD_DIR="${1:-build}"
CMAKE_CACHE="$BUILD_DIR/CMakeCache.txt"

if [[ ! -f "$CMAKE_CACHE" ]]; then
  echo "CMake cache not found: $CMAKE_CACHE" >&2
  exit 1
fi

cache_value() {
  local key="$1"
  sed -n "s/^${key}:BOOL=//p" "$CMAKE_CACHE" | tail -n 1
}

require_on() {
  local key="$1"
  local value
  value="$(cache_value "$key")"
  if [[ "$value" != "ON" && "$value" != "TRUE" ]]; then
    echo "${key} must be ON before packaging; found '${value:-unset}'" >&2
    exit 1
  fi
}

require_on BUILD_SYSTEM_CONTRACTS

CONTRACT_BUILD_STAMP="$BUILD_DIR/contracts_project-prefix/src/contracts_project-stamp/contracts_project-done"
if [[ ! -f "$CONTRACT_BUILD_STAMP" ]]; then
  echo "System contract build stamp not found before packaging: $CONTRACT_BUILD_STAMP" >&2
  exit 1
fi
