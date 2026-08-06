#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build}"
if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi

for required_tool in rustc forge; do
  command -v "$required_tool" >/dev/null || {
    echo "Missing required generated-codec test tool: $required_tool" >&2
    exit 1
  }
done

SOLANA_MODELS="$BUILD_DIR/opp/solana/src"
SOLIDITY_MODELS="$BUILD_DIR/opp/solidity"
if [[ ! -f "$SOLANA_MODELS/protobuf_runtime.rs" ||
      ! -f "$SOLIDITY_MODELS/contracts/ProtobufRuntime.sol" ]]; then
  echo "Generated OPP models are missing; build and run CTest before this script" >&2
  exit 1
fi

mkdir -p "$BUILD_DIR/libraries/opp"
RUST_HARNESS_DIR="$(mktemp -d "$BUILD_DIR/libraries/opp/uic-cross-generator-rust.XXXXXX")"
cleanup() {
  rm -rf -- "$RUST_HARNESS_DIR"
}
trap cleanup EXIT

cp "$SCRIPT_DIR/uic_cross_generator_rust.rs" "$RUST_HARNESS_DIR/uic_cross_generator_rust.rs"
ln -s "$SOLANA_MODELS" "$RUST_HARNESS_DIR/generated"
RUST_TEST_BINARY="$RUST_HARNESS_DIR/test_uic_cross_generator_rust"
rustc \
  --edition=2021 \
  --test "$RUST_HARNESS_DIR/uic_cross_generator_rust.rs" \
  -o "$RUST_TEST_BINARY"
"$RUST_TEST_BINARY"

forge test \
  --root "$SCRIPT_DIR/uic_cross_generator_solidity" \
  --remappings "@wireio/opp-solidity-models/=$SOLIDITY_MODELS/" \
  --use 0.8.25 \
  --evm-version cancun \
  --out "$BUILD_DIR/libraries/opp/uic-solidity-out" \
  --cache-path "$BUILD_DIR/libraries/opp/uic-solidity-cache"
