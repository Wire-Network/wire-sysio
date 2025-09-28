#!/usr/bin/env bash

set -euo pipefail

# Determine script paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR=$(realpath "${SCRIPT_DIR}/..")
DOCKER_DIR="${BASE_DIR}/etc/docker"

# Default build target and image tag
TARGET="app-build-local"
TAG="wire/sysio"
BRANCH="master"

usage() {
  echo "Usage: $(basename "$0") [--target=<stage>]  [--branch=<branch-name>] [--tag=<tag-name>]" 1>&2
  echo "  --target=<stage>        Docker build target stage name (default: ${TARGET})" 1>&2
  echo "  --branch=<branch-or-ref> Git branch or ref (default: ${BRANCH})" 1>&2
  echo "  --tag=<tag-name> Docker image tag to produce (default: ${TAG})" 1>&2
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --target=*)
      TARGET="${1#--target=}"
      shift
      ;;
    --tag=*)
      TAG="${1#--tag=}"
      shift
      ;;
    --branch=*)
      BRANCH="${1#--branch=}"
      shift
      ;;  
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" 1>&2
      usage
      exit 1
      ;;
  esac
done

# Cross-platform total memory check (require >= 32 GiB)
required_bytes=$((32 * 1024 * 1024 * 1024))

get_total_mem_bytes() {
  local uname_out
  uname_out="$(uname -s 2>/dev/null || echo '')"

  case "${uname_out}" in
    Linux*)
      # /proc/meminfo provides MemTotal in kB
      if [[ -r /proc/meminfo ]]; then
        local kb
        kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || echo '')"
        [[ -n "${kb}" ]] && echo $((kb * 1024)) && return 0
      fi
      ;;
    Darwin*)
      # macOS: sysctl returns bytes
      if command -v sysctl >/dev/null 2>&1; then
        local bytes
        bytes="$(sysctl -n hw.memsize 2>/dev/null || echo '')"
        [[ -n "${bytes}" ]] && echo "${bytes}" && return 0
      fi
      ;;
    MINGW*|MSYS*|CYGWIN*)
      # Windows via Git Bash/MSYS/Cygwin: prefer PowerShell, fallback to WMIC
      if command -v powershell.exe >/dev/null 2>&1; then
        local bytes
        bytes="$(powershell.exe -NoProfile -Command "[int64](Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory" 2>/dev/null | tr -dc '0-9')"
        [[ -n "${bytes}" ]] && echo "${bytes}" && return 0
        # Older systems: Get-WmiObject fallback
        bytes="$(powershell.exe -NoProfile -Command "[int64](Get-WmiObject Win32_ComputerSystem).TotalPhysicalMemory" 2>/dev/null | tr -dc '0-9')"
        [[ -n "${bytes}" ]] && echo "${bytes}" && return 0
      fi
      if command -v wmic >/dev/null 2>&1; then
        # WMIC reports KB; convert to bytes
        local kb
        kb="$(wmic OS get TotalVisibleMemorySize /Value 2>/dev/null | awk -F= '/TotalVisibleMemorySize/{print $2}' | tr -dc '0-9')"
        [[ -n "${kb}" ]] && echo $((kb * 1024)) && return 0
      fi
      ;;
    *)
      :
      ;;
  esac

  # Unknown or failed detection
  return 1
}

mem_bytes="$(get_total_mem_bytes)"
echo "[*] Detected total system memory: ${mem_bytes:-unknown} bytes"
if [[ -n "${mem_bytes:-}" ]] && [[ "${mem_bytes}" =~ ^[0-9]+$ ]]; then

  mem_gib=$((mem_bytes / 1024 / 1024 / 1024))
  echo "[*] Total memory: ${mem_gib} GB)"
  if [[ $mem_bytes -lt $required_bytes ]]; then
    echo "Error: Detected total memory ${mem_gib} GiB, but at least 32 GiB is required for this build." 1>&2
    echo "Please increase available RAM (or Docker's memory limit) and try again." 1>&2
    exit 1
  fi
else
  echo "Warning: Could not determine total system memory; proceeding without enforcing the 32 GiB check." 1>&2
fi

cd "${DOCKER_DIR}"
docker build \
  --build-context llvm-11-scripts="${BASE_DIR}/scripts/llvm-11" \
  --build-context app-root="${BASE_DIR}" \
  --build-arg BRANCH=${BRANCH} \
  --memory 32G \
  --tag "${TAG}" \
  --target "${TARGET}" \
  .

echo "[*] Built Docker image '${TAG}' with target stage '${TARGET}'"