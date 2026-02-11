#!/usr/bin/env bash

set -e
echo "Checking for Python 3.10"
SOURCE_SCRIPT="$(realpath "${BASH_SOURCE[0]}")"
while [ -L "$SOURCE_SCRIPT" ]; do
  DIR="$(cd -- "$(dirname -- "$SOURCE_SCRIPT")" && pwd -P)"
  SOURCE_SCRIPT="$(readlink -- "$SOURCE_SCRIPT")"
  [[ "$SOURCE_SCRIPT" != /* ]] && SOURCE_SCRIPT="$DIR/$SOURCE_SCRIPT"
done

SCRIPT_DIR="$(cd -- "$(dirname -- "$SOURCE_SCRIPT")" && pwd -P)"
BASE_DIR="$(realpath "${SCRIPT_DIR}/..")"
VENV_DIR="${BASE_DIR}/.venv"

if [[ -d "${VENV_DIR}" ]]; then
  source "${VENV_DIR}/bin/activate"
  if ! command python --version 2>&1 | grep -qE '\b3\.10\..*'; then
    echo "Python 3.10 not found in ${VENV_DIR}, you should delete it and re-run this script to re-create it..."
    exit 1
  fi
  echo "Python 3.10 found in ${VENV_DIR}, skipping creation..."
  exit 0
fi

echo "Checking for Python 3.10..."
if ! command -v python3.10 &> /dev/null; then
  echo "Python 3.10 not found, please install it first (make sure you have the deadsnakes PPA installed)..."
  exit 1
fi

echo "Checking for virtualenv..."
if ! command -v virtualenv &> /dev/null; then
  echo "virtualenv not found, please install it first. You can do so with 'sudo apt install python3-virtualenv'..."
  exit 1
fi

PY_EXE="$(which python3.10)"
echo "Creating venv in ${VENV_DIR} using Python ${PY_EXE}..."
virtualenv -p "${PY_EXE}" .venv

source "${VENV_DIR}/bin/activate"

echo "Installing pip, setuptools and wheel..."
python -m pip install -U pip setuptools wheel

echo "Installing requirements..."
python -m pip install -r "${BASE_DIR}/requirements.txt"
deactivate

echo "Done!"
