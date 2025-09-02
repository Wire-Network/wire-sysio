#!/usr/bin/env bash

set -euo pipefail

# Determine script paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR=$(realpath "${SCRIPT_DIR}/../..")
WIRE_ORG_DIR=$(realpath "${SCRIPT_DIR}/../../..")
WIRE_SYS_CONTRACT_DIR=$(realpath "${WIRE_ORG_DIR}/wire-system-contracts")

python3 bios-boot-tutorial.py --clio=clio --nodeop=nodeop --kiod=kiod --contracts-dir="${WIRE_SYS_CONTRACT_DIR}/build/contracts" -w -a --user-limit=10 --producer-limit=3