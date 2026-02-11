#!/usr/bin/env bash
set -euo pipefail

CONTAINER_NAME="ethereum-devnet-anvil"
CHAIN_ID=31337
RPC_PORT=8545
# shellcheck disable=SC2046
SCRIPT_DIR=$(realpath $(dirname $(readlink -f "${BASH_SOURCE[0]}")))
REPO_ROOT=$(realpath "${SCRIPT_DIR}/../..")
PROJECT_DIR="${REPO_ROOT}/.data/${CONTAINER_NAME}"
DOCKER_COMPOSE="docker compose"

# --- Basic sanity checks ---
if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: docker is not installed or not in PATH."
  echo "Install Docker first and re-run this script."
  exit 1
fi

echo "Stopping any existing devnet..."
eval "${DOCKER_COMPOSE}  --project-directory ${PROJECT_DIR} rm --force --stop" &> /dev/null || true

echo "Creating project in: ${PROJECT_DIR}"
#if [[ -d "${PROJECT_DIR}" ]]; then
#  rm -Rf "${PROJECT_DIR}"
#fi
mkdir -p "${PROJECT_DIR}"
cd "${PROJECT_DIR}"

# --- docker-compose.yml to run devnet as restartable container ---
cat > docker-compose.yml << EOF
# version: "3.9"
services:
  ${CONTAINER_NAME}:
    image: ghcr.io/foundry-rs/foundry:latest
    container_name: ${CONTAINER_NAME}
    network_mode: host
    command:
      - anvil
      - -vvv
      - --host
      - 0.0.0.0
      - --port
      - "${RPC_PORT}"
      - --chain-id
      - "${CHAIN_ID}"
      - --accounts
      - "0"
      - --state
      - /data/state.json
    ports:
      - "${RPC_PORT}:${RPC_PORT}"
    volumes:
      - ./anvil-state:/data
    restart: unless-stopped
EOF

mkdir -p anvil-state

echo "Building Docker image and starting Hardhat devnet..."
eval "${DOCKER_COMPOSE} up -d"

echo
echo "==================================================================="
echo "Local Ethereum DevNet is running in Docker."
echo
echo "JSON-RPC endpoint: http://localhost:8545"
echo "Chain ID:         31337"
echo
echo "To stop:   ${DOCKER_COMPOSE} down"
echo "To start:  ${DOCKER_COMPOSE} up -d"
echo
echo "Next steps:"
echo "  1) Query accounts:"
echo "     curl -s -X POST -H 'Content-Type: application/json' \\"
echo "       --data '{\"jsonrpc\":\"2.0\",\"method\":\"eth_accounts\",\"params\":[],\"id\":1}' \\"
echo "       http://localhost:8545 | jq"
echo
echo "  2) Send a transaction (see detailed examples in the README below)."
echo "==================================================================="
