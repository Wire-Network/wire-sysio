#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR=$(realpath "${SCRIPT_DIR}/../..")
cd ${BASE_DIR}/etc/docker
docker build --build-context scripts="${BASE_DIR}/scripts" \
  --memory 32G --tag wire/sysio:aarch64 --target app-build-repo ${SCRIPT_DIR}